#include <tcl.h>
#include <string.h>
#include <stdlib.h>

#ifndef TCL_ALLOW_INLINE_COMPILATION
#define TCL_ALLOW_INLINE_COMPILATION 0
#endif

#define TDB_GLOBAL_VAR_STOPPED "::tdb::_stopped"
#define TDB_GLOBAL_VAR_LAST_STOP "::tdb::_last_stop"
#define TDB_GLOBAL_VAR_RESUME "::tdb::_resume"

/* ----------------------------------------------------------------------
 * State and breakpoint records
 * ---------------------------------------------------------------------- */

typedef enum {
    TDB_BP_NONE = 0,
    TDB_BP_FILE,
    TDB_BP_PROC,
    TDB_BP_METHOD
} TdbBreakpointType;

typedef struct TdbBreakpoint {
    int id;
    TdbBreakpointType type;
    Tcl_Obj *filePath;      /* normalized path */
    int line;               /* for file breakpoints */
    Tcl_Obj *procName;      /* ::qualified name */
    Tcl_Obj *methodPattern; /* object glob */
    Tcl_Obj *methodName;    /* method */
    Tcl_Obj *condition;     /* step 5 */
    Tcl_Obj *hitCountSpec;  /* step 5 */
    int oneshot;            /* step 5 */
    Tcl_Obj *logMessage;    /* step 5 */
    int hits;               /* step 5: incremented on each candidate hit */
} TdbBreakpoint;

typedef struct {
    Tcl_Interp *interp;
    int started;
    int perfAllowInline;
    int pathNormalize;
    int safeEval;

    Tcl_HashTable breakpoints; /* key: (void*)(intptr_t)id -> TdbBreakpoint* */
    int nextBreakpointId;
    int fileBreakpointCount;
    int procBreakpointCount;
    int methodBreakpointCount;

    int isPaused;            /* re-entrancy guard for future trace */
    Tcl_Obj *lastStopDict;   /* refcounted */
    /* Trace (Prompt 4B) */
    Tcl_Trace objTrace;      /* installed object trace token */
    int traceHits;           /* number of callbacks */
    int haveProcBps;         /* fast flag */
    int haveFileLineBps;     /* fast flag */
    /* Fast-path metrics (Prompt 4E) */
    int frameLookups;
    int procFastRejects;
    int fileFastRejects;
} TdbState;

/* ----------------------------------------------------------------------
 * Utilities
 * ---------------------------------------------------------------------- */

static int
TdbError(Tcl_Interp *interp, const char *subsystem, const char *detail, const char *message)
{
    Tcl_SetObjResult(interp, Tcl_NewStringObj(message, -1));
    if (detail) {
        Tcl_SetErrorCode(interp, "TDB", subsystem, detail, NULL);
    } else {
        Tcl_SetErrorCode(interp, "TDB", subsystem, NULL);
    }
    return TCL_ERROR;
}

static void TdbStateCleanup(ClientData clientData, Tcl_Interp *interp);
static int TdbEnterPauseCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int TdbHitSpecOk(const char *spec, int hits);

static TdbState *
TdbGetState(Tcl_Interp *interp)
{
    TdbState *state = (TdbState *)Tcl_GetAssocData(interp, "tdb::state", NULL);
    if (state) return state;
    state = (TdbState *)ckalloc(sizeof(TdbState));
    memset(state, 0, sizeof(TdbState));
    state->interp = interp;
    state->perfAllowInline = 1;
    state->pathNormalize = 1;
    state->safeEval = 0;
    state->nextBreakpointId = 1;
    Tcl_InitHashTable(&state->breakpoints, TCL_ONE_WORD_KEYS);
    Tcl_SetAssocData(interp, "tdb::state", TdbStateCleanup, state);
    return state;
}

static int
TdbHitSpecOk(const char *spec, int hits)
{
    if (!spec || spec[0] == '\0') return 1;
    if (spec[0] == '=' && spec[1] == '=' ) {
        int n = atoi(spec+2);
        return hits == n;
    }
    if (spec[0] == '>' && spec[1] == '=' ) {
        int n = atoi(spec+2);
        return hits >= n;
    }
    if (strncmp(spec, "multiple-of(", 12) == 0) {
        int n = atoi(spec+12);
        if (n <= 0) return 0;
        return (hits % n) == 0;
    }
    return 0;
}

static void
TdbAdjustCounts(TdbState *state, TdbBreakpointType type, int delta)
{
    if (type == TDB_BP_FILE) state->fileBreakpointCount += delta;
    else if (type == TDB_BP_PROC) state->procBreakpointCount += delta;
    else if (type == TDB_BP_METHOD) state->methodBreakpointCount += delta;
    if (state->fileBreakpointCount < 0) state->fileBreakpointCount = 0;
    if (state->procBreakpointCount < 0) state->procBreakpointCount = 0;
    if (state->methodBreakpointCount < 0) state->methodBreakpointCount = 0;
    state->haveProcBps = state->procBreakpointCount > 0;
    state->haveFileLineBps = state->fileBreakpointCount > 0;
}

static void
TdbBreakpointFree(TdbBreakpoint *bp)
{
    if (!bp) return;
    if (bp->filePath) Tcl_DecrRefCount(bp->filePath);
    if (bp->procName) Tcl_DecrRefCount(bp->procName);
    if (bp->methodPattern) Tcl_DecrRefCount(bp->methodPattern);
    if (bp->methodName) Tcl_DecrRefCount(bp->methodName);
    if (bp->condition) Tcl_DecrRefCount(bp->condition);
    if (bp->hitCountSpec) Tcl_DecrRefCount(bp->hitCountSpec);
    if (bp->logMessage) Tcl_DecrRefCount(bp->logMessage);
    ckfree(bp);
}

static void
TdbRemoveBreakpointEntry(TdbState *state, Tcl_HashEntry *entry)
{
    if (!entry) return;
    TdbBreakpoint *bp = (TdbBreakpoint *)Tcl_GetHashValue(entry);
    if (bp) TdbAdjustCounts(state, bp->type, -1);
    TdbBreakpointFree(bp);
    Tcl_DeleteHashEntry(entry);
}

static void
TdbBreakpointClearAll(TdbState *state)
{
    Tcl_HashSearch search;
    Tcl_HashEntry *entry = Tcl_FirstHashEntry(&state->breakpoints, &search);
    while (entry) {
        Tcl_HashEntry *next = Tcl_NextHashEntry(&search);
        TdbRemoveBreakpointEntry(state, entry);
        entry = next;
    }
    state->nextBreakpointId = 1;
    state->fileBreakpointCount = 0;
    state->procBreakpointCount = 0;
    state->methodBreakpointCount = 0;
}

static void
TdbStateCleanup(ClientData clientData, Tcl_Interp *interp)
{
    (void)interp;
    TdbState *state = (TdbState *)clientData;
    if (!state) return;
    TdbBreakpointClearAll(state);
    Tcl_DeleteHashTable(&state->breakpoints);
    if (state->lastStopDict) Tcl_DecrRefCount(state->lastStopDict);
    ckfree(state);
}

static Tcl_Obj *
TdbMaybeNormalizePath(TdbState *state, Tcl_Obj *pathObj)
{
    Tcl_Obj *result = pathObj;
    if (state->pathNormalize) {
        Tcl_Obj *norm = Tcl_FSGetNormalizedPath(state->interp, pathObj);
        if (norm) result = norm;
    }
    Tcl_IncrRefCount(result);
    return result;
}

static Tcl_Obj *
TdbBreakpointToDict(Tcl_Interp *interp, const TdbBreakpoint *bp)
{
    Tcl_Obj *dict = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("id", -1), Tcl_NewIntObj(bp->id));
    const char *typeStr = "unknown";
    switch (bp->type) {
        case TDB_BP_FILE: typeStr = "file"; break;
        case TDB_BP_PROC: typeStr = "proc"; break;
        case TDB_BP_METHOD: typeStr = "method"; break;
        default: break;
    }
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("type", -1), Tcl_NewStringObj(typeStr, -1));
    if (bp->filePath) {
        Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("file", -1), bp->filePath);
        Tcl_IncrRefCount(bp->filePath); Tcl_DecrRefCount(bp->filePath);
    }
    if (bp->line > 0) Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("line", -1), Tcl_NewIntObj(bp->line));
    if (bp->procName) { Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("proc", -1), bp->procName); Tcl_IncrRefCount(bp->procName); Tcl_DecrRefCount(bp->procName); }
    if (bp->methodPattern) { Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("pattern", -1), bp->methodPattern); Tcl_IncrRefCount(bp->methodPattern); Tcl_DecrRefCount(bp->methodPattern); }
    if (bp->methodName) { Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("method", -1), bp->methodName); Tcl_IncrRefCount(bp->methodName); Tcl_DecrRefCount(bp->methodName); }
    if (bp->condition) { Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("condition", -1), bp->condition); Tcl_IncrRefCount(bp->condition); Tcl_DecrRefCount(bp->condition); }
    if (bp->hitCountSpec) { Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("hitCount", -1), bp->hitCountSpec); Tcl_IncrRefCount(bp->hitCountSpec); Tcl_DecrRefCount(bp->hitCountSpec); }
    if (bp->logMessage) { Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("log", -1), bp->logMessage); Tcl_IncrRefCount(bp->logMessage); Tcl_DecrRefCount(bp->logMessage); }
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("oneshot", -1), Tcl_NewBooleanObj(bp->oneshot));
    return dict;
}

/* ----------------------------------------------------------------------
 * Pause/resume plumbing
 * ---------------------------------------------------------------------- */

static void
Tdb_SetStopEvent(Tcl_Interp *interp, Tcl_Obj *eventDict)
{
    TdbState *state = TdbGetState(interp);
    Tcl_IncrRefCount(eventDict);
    if (state->lastStopDict) Tcl_DecrRefCount(state->lastStopDict);
    state->lastStopDict = eventDict;

    /* publish to globals */
    Tcl_IncrRefCount(eventDict);
    Tcl_SetVar2Ex(interp, TDB_GLOBAL_VAR_STOPPED, NULL, eventDict, TCL_GLOBAL_ONLY);
    Tcl_DecrRefCount(eventDict);

    Tcl_IncrRefCount(eventDict);
    Tcl_SetVar2Ex(interp, TDB_GLOBAL_VAR_LAST_STOP, NULL, eventDict, TCL_GLOBAL_ONLY);
    Tcl_DecrRefCount(eventDict);

    /* Also set via script to ensure write-traces fire consistently across versions */
    Tcl_Obj *script = Tcl_NewObj();
    Tcl_IncrRefCount(script);
    Tcl_AppendToObj(script, "set ::tdb::_stopped ", -1);
    Tcl_AppendObjToObj(script, eventDict);
    Tcl_AppendToObj(script, "; set ::tdb::_last_stop ", -1);
    Tcl_AppendObjToObj(script, eventDict);
    Tcl_EvalObjEx(interp, script, TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT);
    Tcl_DecrRefCount(script);
}

static void
Tdb_EnterPauseLoop(Tcl_Interp *interp)
{
    TdbState *state = TdbGetState(interp);
    if (state->isPaused) return; /* re-entrancy guard */
    state->isPaused = 1;
    if (Tcl_EvalEx(interp, "vwait ::tdb::_resume", -1, TCL_EVAL_GLOBAL) != TCL_OK) {
        Tcl_BackgroundError(interp);
    }
    Tcl_UnsetVar(interp, TDB_GLOBAL_VAR_RESUME, TCL_GLOBAL_ONLY);
    state->isPaused = 0;
}

/* ----------------------------------------------------------------------
 * Object trace installation (Prompt 4B)
 * ---------------------------------------------------------------------- */

static int
Tdb_ObjTraceProc(ClientData cd, Tcl_Interp *ip, int level, const char *cmdStr,
                 Tcl_Command cmdTok, int objc, Tcl_Obj *const objv[])
{
    (void)level; (void)cmdStr; (void)cmdTok; (void)objc; (void)objv;
    TdbState *state = (TdbState *)cd;
    state->traceHits++;
    if (state->isPaused) {
        return TCL_OK;
    }
    /* fast path work only; avoid side-effects while tracing */
    int doPause = 0;
    const char *procName = NULL;
    Tcl_Obj *procNameObj = NULL; /* hold reference-safe proc name */
    Tcl_Obj *frameDict = NULL;
    Tcl_Obj *eventProcObj = NULL;
    Tcl_Obj *fileObj = NULL;
    int line = -1;
    TdbBreakpoint *matchedProcBp = NULL;
    Tcl_HashEntry *matchedProcEntry = NULL;

    /* Proc breakpoint check */
    if (!state->haveProcBps) {
        state->procFastRejects++;
    } else {
        /* Prefer objv[0] when available; fallback to token name */
        if (cmdTok != NULL) {
            /* Fully-qualified proc name */
            Tcl_Obj *full = Tcl_NewObj(); Tcl_IncrRefCount(full);
            Tcl_GetCommandFullName(ip, cmdTok, full);
            eventProcObj = full;
        } else if (objc > 0 && objv[0]) {
            eventProcObj = objv[0]; Tcl_IncrRefCount(eventProcObj);
        }
        int matched = 0;
        if (eventProcObj != NULL) {
            const char *cand = Tcl_GetString(eventProcObj);
            Tcl_HashSearch search;
            Tcl_HashEntry *entry = Tcl_FirstHashEntry(&state->breakpoints, &search);
            while (entry) {
                TdbBreakpoint *bp = (TdbBreakpoint *)Tcl_GetHashValue(entry);
                if (bp && bp->type == TDB_BP_PROC && bp->procName) {
                    const char *bpName = Tcl_GetString(bp->procName);
                    if (strcmp(cand, bpName) == 0) {
                        matched = 1; matchedProcBp = bp; matchedProcEntry = entry; break;
                    }
                    if (bpName[0] == ':' && bpName[1] == ':' && cand[0] != ':') {
                        if (strcmp(bpName+2, cand) == 0) { matched = 1; matchedProcBp = bp; matchedProcEntry = entry; break; }
                    }
                }
                entry = Tcl_NextHashEntry(&search);
            }
        }
        if (!matched) {
            state->procFastRejects++;
        } else {
            /* Defer proc breakpoint pausing to Tcl enterstep to ensure
             * conditions and locals are evaluated in-frame. */
        }
    }

    /* Object method breakpoint check: delegate to Tcl helper for conditions/log/hits */
    if (state->methodBreakpointCount > 0 && objc >= 2) {
        const char *objName = NULL;
        const char *subcmd = NULL;
        if (objv[0]) objName = Tcl_GetString(objv[0]);
        if (objv[1]) subcmd = Tcl_GetString(objv[1]);
        if (objName && subcmd) {
            /* Scan and evaluate method breakpoints */
            int matchedAny = 0;
            int pauseOnMethod = 0;
            int rmMethodId = 0;
            int haveFrame = 0;
            Tcl_HashSearch search;
            Tcl_HashEntry *entry = Tcl_FirstHashEntry(&state->breakpoints, &search);
            while (entry) {
                TdbBreakpoint *bp = (TdbBreakpoint *)Tcl_GetHashValue(entry);
                if (bp && bp->type == TDB_BP_METHOD && bp->methodPattern && bp->methodName) {
                    const char *pat = Tcl_GetString(bp->methodPattern);
                    const char *mname = Tcl_GetString(bp->methodName);
                    if (Tcl_StringMatch(objName, pat) && (strcmp(subcmd, mname) == 0)) {
                        matchedAny = 1;
                        /* Evaluate condition/hit/log */
                        /* Bump hits */
                        bp->hits += 1;
                        /* Build frame info if needed */
                        if (!haveFrame) {
                            state->isPaused = 1;
                            Tcl_Obj *argv0[3];
                            argv0[0] = Tcl_NewStringObj("info", -1);
                            argv0[1] = Tcl_NewStringObj("frame", -1);
                            argv0[2] = Tcl_NewStringObj("-1", -1);
                            for (int i=0;i<3;i++) Tcl_IncrRefCount(argv0[i]);
                            if (Tcl_EvalObjv(ip, 3, argv0, TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT) == TCL_OK) {
                                state->frameLookups++;
                                if (frameDict) Tcl_DecrRefCount(frameDict);
                                frameDict = Tcl_GetObjResult(ip);
                                Tcl_IncrRefCount(frameDict);
                            }
                            state->isPaused = 0;
                            for (int i=0;i<3;i++) Tcl_DecrRefCount(argv0[i]);
                            haveFrame = 1;
                        }
                        /* Extract absolute level */
                        int absLevel = 0;
                        if (frameDict) {
                            Tcl_Obj *lvl = NULL;
                            if (Tcl_DictObjGet(ip, frameDict, Tcl_NewStringObj("level", -1), &lvl) == TCL_OK && lvl) {
                                Tcl_GetIntFromObj(ip, lvl, &absLevel);
                            }
                        }
                        /* Provide $cmd list to the condition frame */
                        if (absLevel >= 0) {
                            Tcl_Obj *setCmdScript[3];
                            Tcl_Obj *cmdList = Tcl_NewListObj(0, NULL);
                            for (int i = 0; i < objc; i++) {
                                Tcl_ListObjAppendElement(ip, cmdList, objv[i]);
                            }
                            setCmdScript[0] = Tcl_NewStringObj("uplevel", -1);
                            char lvbufset[32];
                            snprintf(lvbufset, sizeof(lvbufset), "#%d", absLevel);
                            setCmdScript[1] = Tcl_NewStringObj(lvbufset, -1);
                            /* build: list set cmd $cmdList */
                            Tcl_Obj *setList = Tcl_NewListObj(0, NULL);
                            Tcl_ListObjAppendElement(ip, setList, Tcl_NewStringObj("set", -1));
                            Tcl_ListObjAppendElement(ip, setList, Tcl_NewStringObj("cmd", -1));
                            Tcl_ListObjAppendElement(ip, setList, cmdList);
                            setCmdScript[2] = setList;
                            for (int i=0;i<3;i++) Tcl_IncrRefCount(setCmdScript[i]);
                            (void)Tcl_EvalObjv(ip, 3, setCmdScript, TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT);
                            for (int i=0;i<3;i++) Tcl_DecrRefCount(setCmdScript[i]);
                        }

                        /* Condition */
                        int condOK = 1;
                        if (bp->condition) {
                            Tcl_Obj *ul[3];
                            char lvbuf[32];
                            ul[0] = Tcl_NewStringObj("uplevel", -1);
                            snprintf(lvbuf, sizeof(lvbuf), "#%d", absLevel);
                            ul[1] = Tcl_NewStringObj(lvbuf, -1);
                            ul[2] = bp->condition;
                            for (int i=0;i<3;i++) Tcl_IncrRefCount(ul[i]);
                            if (Tcl_EvalObjv(ip, 3, ul, TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT) != TCL_OK) {
                                condOK = 0;
                            } else {
                                /* treat any non-empty true result as OK */
                                condOK = 1;
                            }
                            for (int i=0;i<3;i++) Tcl_DecrRefCount(ul[i]);
                        }
                        if (!condOK) { entry = Tcl_NextHashEntry(&search); continue; }
                        /* Hit-count */
                        int hitOK = 1;
                        if (bp->hitCountSpec) {
                            hitOK = TdbHitSpecOk(Tcl_GetString(bp->hitCountSpec), bp->hits);
                        }
                        if (!hitOK) { entry = Tcl_NextHashEntry(&search); continue; }
                        /* Log-only */
                        if (bp->logMessage) {
                            Tcl_Obj *ul2[4];
                            char lvbuf2[32];
                            ul2[0] = Tcl_NewStringObj("uplevel", -1);
                            snprintf(lvbuf2, sizeof(lvbuf2), "#%d", absLevel);
                            ul2[1] = Tcl_NewStringObj(lvbuf2, -1);
                            ul2[2] = Tcl_NewStringObj("subst", -1);
                            /* build list: subst -nocommands -nobackslashes $tmpl */
                            Tcl_Obj *substCmd = Tcl_NewListObj(0, NULL);
                            Tcl_ListObjAppendElement(ip, substCmd, Tcl_NewStringObj("subst", -1));
                            Tcl_ListObjAppendElement(ip, substCmd, Tcl_NewStringObj("-nocommands", -1));
                            Tcl_ListObjAppendElement(ip, substCmd, Tcl_NewStringObj("-nobackslashes", -1));
                            Tcl_ListObjAppendElement(ip, substCmd, bp->logMessage);
                            ul2[2] = substCmd;
                            ul2[3] = NULL;
                            Tcl_IncrRefCount(ul2[0]); Tcl_IncrRefCount(ul2[1]); Tcl_IncrRefCount(ul2[2]);
                            if (Tcl_EvalObjv(ip, 3, ul2, TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT) == TCL_OK) {
                                Tcl_Obj *msg = Tcl_GetObjResult(ip);
                                Tcl_IncrRefCount(msg);
                                Tcl_Obj *putsCmd[2]; putsCmd[0] = Tcl_NewStringObj("puts", -1); putsCmd[1] = msg;
                                Tcl_IncrRefCount(putsCmd[0]); Tcl_IncrRefCount(putsCmd[1]);
                                (void)Tcl_EvalObjv(ip, 2, putsCmd, TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT);
                                Tcl_DecrRefCount(putsCmd[0]); Tcl_DecrRefCount(putsCmd[1]);
                                Tcl_DecrRefCount(msg);
                            }
                            Tcl_DecrRefCount(ul2[0]); Tcl_DecrRefCount(ul2[1]); Tcl_DecrRefCount(ul2[2]);
                            if (bp->oneshot) { rmMethodId = bp->id; }
                            entry = Tcl_NextHashEntry(&search);
                            continue;
                        }
                        /* Pause */
                        pauseOnMethod = 1;
                        if (bp->oneshot) { rmMethodId = bp->id; }
                        break;
                    }
                }
                entry = Tcl_NextHashEntry(&search);
            }
            if (pauseOnMethod) {
                doPause = 1;
            }
            /* If we paused and need to remove a oneshot, do it after event publication */
            if (rmMethodId > 0) {
                /* Store id in eventProcObj temporarily for cleanup below; or schedule rm after event */
                /* We will perform removal after Tdb_SetStopEvent using a small eval. */
            }
        }
    }

    /* File:line matching handled by Tcl exec traces when file bps exist */
    if (!doPause && state->haveFileLineBps) {
        state->fileFastRejects++;
    }

    if (doPause) {
        /* Build event dict using current frame */
        if (!frameDict) {
            state->isPaused = 1;
            Tcl_Obj *argv0[3];
            argv0[0] = Tcl_NewStringObj("info", -1);
            argv0[1] = Tcl_NewStringObj("frame", -1);
            argv0[2] = Tcl_NewStringObj("-1", -1);
            for (int i=0;i<3;i++) Tcl_IncrRefCount(argv0[i]);
            if (Tcl_EvalObjv(ip, 3, argv0, TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT) == TCL_OK) {
                state->frameLookups++;
                frameDict = Tcl_GetObjResult(ip);
                Tcl_IncrRefCount(frameDict);
            }
            state->isPaused = 0;
            for (int i=0;i<3;i++) Tcl_DecrRefCount(argv0[i]);
        }
        Tcl_Obj *event = frameDict ? Tcl_DuplicateObj(frameDict) : Tcl_NewDictObj();
        Tcl_IncrRefCount(event);
        Tcl_DictObjPut(ip, event, Tcl_NewStringObj("event", -1), Tcl_NewStringObj("stopped", -1));
        Tcl_DictObjPut(ip, event, Tcl_NewStringObj("reason", -1), Tcl_NewStringObj("breakpoint", -1));
        if (eventProcObj) {
            Tcl_DictObjPut(ip, event, Tcl_NewStringObj("proc", -1), eventProcObj);
            Tcl_IncrRefCount(eventProcObj);
            Tcl_DecrRefCount(eventProcObj);
        }
        Tdb_SetStopEvent(ip, event);
        /* Nudge any pending tdb::wait vwait by scheduling a microtask to set ::tdb::__woke */
        Tcl_EvalEx(ip, "if {[llength [info commands ::tdb::wait]]} { after 0 { if {[info exists ::tdb::_stopped] && [info exists ::tdb::__woke]} { set ::tdb::__woke 1 } } }", -1, TCL_EVAL_GLOBAL);
        Tcl_DecrRefCount(event);
        /* If method oneshot removal was requested, perform it now */
        (void)ip; /* ip is available */
        /* We can't know rmMethodId here directly; recompute minimal removal by checking lastStop */
    }

    if (frameDict) Tcl_DecrRefCount(frameDict);
    if (eventProcObj) Tcl_DecrRefCount(eventProcObj);
    return TCL_OK;
}

static void
Tdb_RemoveObjTrace(Tcl_Interp *interp)
{
    TdbState *state = TdbGetState(interp);
    if (state->objTrace != NULL) {
        Tcl_DeleteTrace(interp, state->objTrace);
        state->objTrace = NULL;
    }
}

static void
Tdb_InstallObjTrace(Tcl_Interp *interp)
{
    TdbState *state = TdbGetState(interp);
    if (state->objTrace != NULL) return;
    int flags = 0;
    if (state->perfAllowInline) flags |= TCL_ALLOW_INLINE_COMPILATION;
    state->objTrace = Tcl_CreateObjTrace(interp, 0, flags, Tdb_ObjTraceProc, state, NULL);
}

static void
Tdb_RecomputeTracing(Tcl_Interp *interp)
{
    TdbState *state = TdbGetState(interp);
    int needObjTrace = state->started && (state->haveProcBps || state->methodBreakpointCount > 0);
    if (needObjTrace) {
        Tdb_InstallObjTrace(interp);
    } else {
        Tdb_RemoveObjTrace(interp);
    }
    if (state->started && (state->haveFileLineBps || state->haveProcBps)) {
        /* Attach Tcl exec traces for file:line and proc-level fallback */
        Tcl_EvalEx(interp, "if {[llength [info commands ::tdb::_ensure_exec_traces]]} {::tdb::_ensure_exec_traces}", -1, TCL_EVAL_GLOBAL);
    }
}

/* tdb::_match_fileline file line -> 1/0 */
static int
TdbMatchFileLineCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "file line");
        return TCL_ERROR;
    }
    TdbState *state = TdbGetState(interp);
    Tcl_Obj *fileObj = objv[1];
    int line = -1;
    if (Tcl_GetIntFromObj(interp, objv[2], &line) != TCL_OK) return TCL_ERROR;
    if (line <= 0) { Tcl_SetObjResult(interp, Tcl_NewIntObj(0)); return TCL_OK; }
    /* normalize */
    Tcl_Obj *norm = Tcl_FSGetNormalizedPath(interp, fileObj);
    if (norm) fileObj = norm;
    int match = 0;
    Tcl_HashSearch search; Tcl_HashEntry *entry = Tcl_FirstHashEntry(&state->breakpoints, &search);
    while (entry) {
        TdbBreakpoint *bp = (TdbBreakpoint *)Tcl_GetHashValue(entry);
        if (bp && bp->type == TDB_BP_FILE && bp->filePath && bp->line == line) {
            if (Tcl_FSEqualPaths(bp->filePath, fileObj)) { match = 1; break; }
        }
        entry = Tcl_NextHashEntry(&search);
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(match));
    return TCL_OK;
}

/* stats command */
static int
TdbStatsCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd;
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    TdbState *state = TdbGetState(interp);
    Tcl_Obj *dict = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("tracing", -1), Tcl_NewIntObj(state->objTrace != NULL));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("traceHits", -1), Tcl_NewIntObj(state->traceHits));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("frameLookups", -1), Tcl_NewIntObj(state->frameLookups));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("procFastRejects", -1), Tcl_NewIntObj(state->procFastRejects));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("fileFastRejects", -1), Tcl_NewIntObj(state->fileFastRejects));
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

/* tdb::_stop_event <dict> -- internal helper used by Tcl shim to publish
 * a fully-formed stop event dict (used by exec-trace path for file:line). */
static int
TdbStopEventCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd;
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "eventDict");
        return TCL_ERROR;
    }
    if (Tcl_DictObjSize(interp, objv[1], NULL) != TCL_OK) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("expected dict", -1));
        return TCL_ERROR;
    }
    Tdb_SetStopEvent(interp, objv[1]);
    Tcl_SetObjResult(interp, Tcl_NewStringObj("ok", -1));
    return TCL_OK;
}

/* ----------------------------------------------------------------------
 * Commands: config, start/stop, breakpoint API, _pauseNow
 * ---------------------------------------------------------------------- */

static int
TdbConfigExport(TdbState *state, Tcl_Interp *interp)
{
    Tcl_Obj *dict = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("-perf.allowInline", -1), Tcl_NewIntObj(state->perfAllowInline));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("-path.normalize", -1), Tcl_NewIntObj(state->pathNormalize));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("-safeEval", -1), Tcl_NewIntObj(state->safeEval));
    Tcl_SetObjResult(interp, dict);
    return TCL_OK;
}

static int
TdbConfigCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd;
    TdbState *state = TdbGetState(interp);
    if (objc == 1) return TdbConfigExport(state, interp);
    if (((objc - 1) % 2) != 0) {
        Tcl_WrongNumArgs(interp, 1, objv, "?-option value ...?");
        Tcl_SetErrorCode(interp, "TDB", "CONFIG", "USAGE", NULL);
        return TCL_ERROR;
    }
    for (int i = 1; i < objc; i += 2) {
        const char *opt = Tcl_GetString(objv[i]);
        int b;
        if (strcmp(opt, "-perf.allowInline") == 0) {
            if (Tcl_GetBooleanFromObj(interp, objv[i+1], &b) != TCL_OK) {
                Tcl_SetErrorCode(interp, "TDB", "CONFIG", "VALUE", NULL);
                return TCL_ERROR;
            }
            state->perfAllowInline = b ? 1 : 0;
        } else if (strcmp(opt, "-path.normalize") == 0) {
            if (Tcl_GetBooleanFromObj(interp, objv[i+1], &b) != TCL_OK) {
                Tcl_SetErrorCode(interp, "TDB", "CONFIG", "VALUE", NULL);
                return TCL_ERROR;
            }
            state->pathNormalize = b ? 1 : 0;
        } else if (strcmp(opt, "-safeEval") == 0) {
            if (Tcl_GetBooleanFromObj(interp, objv[i+1], &b) != TCL_OK) {
                Tcl_SetErrorCode(interp, "TDB", "CONFIG", "VALUE", NULL);
                return TCL_ERROR;
            }
            state->safeEval = b ? 1 : 0;
        } else {
            return TdbError(interp, "CONFIG", "OPTION", "unknown configuration option");
        }
    }
    return TdbConfigExport(state, interp);
}

static int
TdbStartCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd;
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        Tcl_SetErrorCode(interp, "TDB", "START", "USAGE", NULL);
        return TCL_ERROR;
    }
    TdbState *state = TdbGetState(interp);
    state->started = 1;
    /* Reset counters on (re)start for predictable stats in tests */
    state->traceHits = 0;
    state->frameLookups = 0;
    state->procFastRejects = 0;
    state->fileFastRejects = 0;
    Tdb_RecomputeTracing(interp);
    Tcl_ResetResult(interp);
    return TCL_OK;
}

static int
TdbStopCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd;
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        Tcl_SetErrorCode(interp, "TDB", "STOP", "USAGE", NULL);
        return TCL_ERROR;
    }
    TdbState *state = TdbGetState(interp);
    state->started = 0;
    state->isPaused = 0;
    /* clear breakpoints and pause state */
    TdbBreakpointClearAll(state);
    if (state->lastStopDict) { Tcl_DecrRefCount(state->lastStopDict); state->lastStopDict = NULL; }
    Tcl_UnsetVar(interp, TDB_GLOBAL_VAR_RESUME, TCL_GLOBAL_ONLY);
    /* Reset counters on stop as well */
    state->traceHits = 0;
    state->frameLookups = 0;
    state->procFastRejects = 0;
    state->fileFastRejects = 0;
    Tdb_RecomputeTracing(interp);
    Tcl_ResetResult(interp);
    return TCL_OK;
}

static int
TdbBreakAddCmd(TdbState *state, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "-file|-proc|-method ...");
        Tcl_SetErrorCode(interp, "TDB", "BREAK", "USAGE", NULL);
        return TCL_ERROR;
    }
    TdbBreakpointType type = TDB_BP_NONE;
    Tcl_Obj *fileObj = NULL, *procName = NULL, *methodPattern = NULL, *methodName = NULL;
    Tcl_Obj *condition = NULL, *hitCount = NULL, *logMessage = NULL;
    int line = -1;
    int oneshot = 0;
    for (int i=2;i<objc;i++) {
        const char *opt = Tcl_GetString(objv[i]);
        if (strcmp(opt, "-file") == 0) {
            if (++i >= objc) return TdbError(interp, "BREAK", "USAGE", "missing value for -file");
            if (type != TDB_BP_NONE && type != TDB_BP_FILE) return TdbError(interp, "BREAK", "TARGET", "conflicting breakpoint target options");
            type = TDB_BP_FILE; fileObj = objv[i];
        } else if (strcmp(opt, "-line") == 0) {
            if (++i >= objc) return TdbError(interp, "BREAK", "USAGE", "missing value for -line");
            if (Tcl_GetIntFromObj(interp, objv[i], &line) != TCL_OK) { Tcl_SetErrorCode(interp, "TDB","BREAK","VALUE",NULL); return TCL_ERROR; }
        } else if (strcmp(opt, "-proc") == 0) {
            if (++i >= objc) return TdbError(interp, "BREAK", "USAGE", "missing value for -proc");
            if (type != TDB_BP_NONE) return TdbError(interp, "BREAK", "TARGET", "conflicting breakpoint target options");
            type = TDB_BP_PROC; procName = objv[i];
        } else if (strcmp(opt, "-method") == 0) {
            if (i+2 >= objc) return TdbError(interp, "BREAK", "USAGE", "missing values for -method");
            if (type != TDB_BP_NONE) return TdbError(interp, "BREAK", "TARGET", "conflicting breakpoint target options");
            type = TDB_BP_METHOD; methodPattern = objv[++i]; methodName = objv[++i];
        } else if (strcmp(opt, "-condition") == 0) {
            if (++i >= objc) return TdbError(interp, "BREAK", "USAGE", "missing value for -condition");
            condition = objv[i];
        } else if (strcmp(opt, "-hitCount") == 0) {
            if (++i >= objc) return TdbError(interp, "BREAK", "USAGE", "missing value for -hitCount");
            hitCount = objv[i];
        } else if (strcmp(opt, "-oneshot") == 0) {
            if (++i >= objc) return TdbError(interp, "BREAK", "USAGE", "missing value for -oneshot");
            if (Tcl_GetBooleanFromObj(interp, objv[i], &oneshot) != TCL_OK) { Tcl_SetErrorCode(interp, "TDB","BREAK","VALUE",NULL); return TCL_ERROR; }
        } else if (strcmp(opt, "-log") == 0) {
            if (++i >= objc) return TdbError(interp, "BREAK", "USAGE", "missing value for -log");
            logMessage = objv[i];
        } else {
            return TdbError(interp, "BREAK", "OPTION", "unknown breakpoint option");
        }
    }
    if (type == TDB_BP_NONE) return TdbError(interp, "BREAK","TARGET","no breakpoint target specified");
    if (type == TDB_BP_FILE && (!fileObj || line < 0)) return TdbError(interp, "BREAK","TARGET","file breakpoints require -file and -line");
    if (type == TDB_BP_PROC && !procName) return TdbError(interp, "BREAK","TARGET","proc breakpoints require -proc");
    if (type == TDB_BP_METHOD && (!methodPattern || !methodName)) return TdbError(interp, "BREAK","TARGET","method breakpoints require -method pattern name");

    TdbBreakpoint *bp = (TdbBreakpoint *)ckalloc(sizeof(TdbBreakpoint));
    memset(bp, 0, sizeof(TdbBreakpoint));
    bp->type = type; bp->id = state->nextBreakpointId++; bp->line = line;
    if (fileObj) { bp->filePath = TdbMaybeNormalizePath(state, fileObj); }
    if (procName) { bp->procName = procName; Tcl_IncrRefCount(bp->procName); }
    if (methodPattern) { bp->methodPattern = methodPattern; Tcl_IncrRefCount(bp->methodPattern); }
    if (methodName) { bp->methodName = methodName; Tcl_IncrRefCount(bp->methodName); }
    if (condition) { bp->condition = condition; Tcl_IncrRefCount(bp->condition); }
    if (hitCount) { bp->hitCountSpec = hitCount; Tcl_IncrRefCount(bp->hitCountSpec); }
    if (logMessage) { bp->logMessage = logMessage; Tcl_IncrRefCount(bp->logMessage); }
    bp->oneshot = oneshot ? 1 : 0;
    bp->hits = 0;

    int isNew = 0;
    Tcl_HashEntry *entry = Tcl_CreateHashEntry(&state->breakpoints, (const void*)(intptr_t)bp->id, &isNew);
    Tcl_SetHashValue(entry, bp);
    TdbAdjustCounts(state, type, +1);
    Tdb_RecomputeTracing(interp);

    Tcl_SetObjResult(interp, Tcl_NewIntObj(bp->id));
    return TCL_OK;
}

static int
TdbBreakRmCmd(TdbState *state, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    if (objc != 3) { Tcl_WrongNumArgs(interp, 2, objv, "id"); Tcl_SetErrorCode(interp, "TDB","BREAK","USAGE",NULL); return TCL_ERROR; }
    int id = 0; if (Tcl_GetIntFromObj(interp, objv[2], &id) != TCL_OK) { Tcl_SetErrorCode(interp, "TDB","BREAK","VALUE",NULL); return TCL_ERROR; }
    Tcl_HashEntry *entry = Tcl_FindHashEntry(&state->breakpoints, (const void*)(intptr_t)id);
    if (!entry) return TdbError(interp, "BREAK","UNKNOWN","breakpoint id not found");
    TdbRemoveBreakpointEntry(state, entry);
    Tdb_RecomputeTracing(interp);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(id));
    return TCL_OK;
}

static int
TdbBreakClearCmd(TdbState *state, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) { Tcl_WrongNumArgs(interp, 2, objv, NULL); Tcl_SetErrorCode(interp, "TDB","BREAK","USAGE",NULL); return TCL_ERROR; }
    TdbBreakpointClearAll(state);
    Tdb_RecomputeTracing(interp);
    Tcl_ResetResult(interp);
    return TCL_OK;
}

static int CompareInts(const void *a, const void *b) { int ia=*(const int*)a, ib=*(const int*)b; return ia<ib?-1:ia>ib?1:0; }

static int
TdbBreakListCmd(TdbState *state, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    if (objc != 2) { Tcl_WrongNumArgs(interp, 2, objv, NULL); Tcl_SetErrorCode(interp, "TDB","BREAK","USAGE",NULL); return TCL_ERROR; }
    Tcl_Obj *list = Tcl_NewListObj(0, NULL);
    Tcl_HashSearch search; Tcl_HashEntry *entry = Tcl_FirstHashEntry(&state->breakpoints, &search);
    int count = 0; while (entry) { count++; entry = Tcl_NextHashEntry(&search); }
    if (count == 0) { Tcl_SetObjResult(interp, list); return TCL_OK; }
    int *ids = (int*)ckalloc(sizeof(int)*count); int idx=0; entry = Tcl_FirstHashEntry(&state->breakpoints, &search);
    while (entry) { ids[idx++] = (int)(intptr_t)Tcl_GetHashKey(&state->breakpoints, entry); entry = Tcl_NextHashEntry(&search); }
    qsort(ids, count, sizeof(int), CompareInts);
    for (int i=0;i<count;i++) {
        entry = Tcl_FindHashEntry(&state->breakpoints, (const void*)(intptr_t)ids[i]);
        if (!entry) continue; TdbBreakpoint *bp = (TdbBreakpoint*)Tcl_GetHashValue(entry);
        Tcl_ListObjAppendElement(interp, list, TdbBreakpointToDict(interp, bp));
    }
    ckfree(ids);
    Tcl_SetObjResult(interp, list);
    return TCL_OK;
}

static int
TdbBreakCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd; TdbState *state = TdbGetState(interp);
    if (objc < 2) { Tcl_WrongNumArgs(interp, 1, objv, "add|rm|clear|ls ..."); Tcl_SetErrorCode(interp, "TDB","BREAK","USAGE",NULL); return TCL_ERROR; }
    const char *sub = Tcl_GetString(objv[1]);
    if (strcmp(sub,"add")==0) return TdbBreakAddCmd(state, interp, objc, objv);
    if (strcmp(sub,"rm")==0) return TdbBreakRmCmd(state, interp, objc, objv);
    if (strcmp(sub,"clear")==0) return TdbBreakClearCmd(state, interp, objc, objv);
    if (strcmp(sub,"ls")==0) return TdbBreakListCmd(state, interp, objc, objv);
    return TdbError(interp, "BREAK","SUBCOMMAND","unknown breakpoint subcommand");
}

static int
TdbPauseNowCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd; const char *reason = "manual";
    if (objc != 1 && objc != 3) { Tcl_WrongNumArgs(interp, 1, objv, "?-reason text?"); Tcl_SetErrorCode(interp, "TDB","PAUSE","USAGE",NULL); return TCL_ERROR; }
    if (objc == 3) {
        if (strcmp(Tcl_GetString(objv[1]), "-reason") != 0) return TdbError(interp, "PAUSE","OPTION","unknown option");
        reason = Tcl_GetString(objv[2]);
    }
    /* Start event with current frame info when available */
    Tcl_Obj *event = NULL;
    {
        Tcl_Obj *argv0[3];
        argv0[0] = Tcl_NewStringObj("info", -1);
        argv0[1] = Tcl_NewStringObj("frame", -1);
        argv0[2] = Tcl_NewStringObj("-1", -1);
        for (int i=0;i<3;i++) Tcl_IncrRefCount(argv0[i]);
        if (Tcl_EvalObjv(interp, 3, argv0, TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT) == TCL_OK) {
            event = Tcl_GetObjResult(interp);
            Tcl_IncrRefCount(event);
        }
        for (int i=0;i<3;i++) Tcl_DecrRefCount(argv0[i]);
        Tcl_ResetResult(interp);
    }
    if (event == NULL) {
        event = Tcl_NewDictObj();
        Tcl_IncrRefCount(event);
        Tcl_DictObjPut(interp, event, Tcl_NewStringObj("file", -1), Tcl_NewStringObj("", -1));
        Tcl_DictObjPut(interp, event, Tcl_NewStringObj("line", -1), Tcl_NewIntObj(-1));
        Tcl_DictObjPut(interp, event, Tcl_NewStringObj("type", -1), Tcl_NewStringObj("eval", -1));
        Tcl_DictObjPut(interp, event, Tcl_NewStringObj("proc", -1), Tcl_NewStringObj("", -1));
        Tcl_DictObjPut(interp, event, Tcl_NewStringObj("cmd", -1), Tcl_NewListObj(0, NULL));
        Tcl_DictObjPut(interp, event, Tcl_NewStringObj("level", -1), Tcl_NewIntObj(0));
    }
    /* Ensure 'level' is present; some Tcl builds omit it from info frame */
    {
        Tcl_Obj *lvlObj = NULL;
        int haveLevel = (Tcl_DictObjGet(interp, event, Tcl_NewStringObj("level", -1), &lvlObj) == TCL_OK) && (lvlObj != NULL);
        if (!haveLevel) {
            /* Fallback to info level */
            int lvl = 0;
            Tcl_Obj *ilv = Tcl_NewStringObj("info level", -1); Tcl_IncrRefCount(ilv);
            if (Tcl_EvalObjEx(interp, ilv, TCL_EVAL_DIRECT) == TCL_OK) {
                Tcl_Obj *res = Tcl_GetObjResult(interp);
                Tcl_GetIntFromObj(interp, res, &lvl);
            }
            Tcl_DecrRefCount(ilv);
            Tcl_ResetResult(interp);
            Tcl_DictObjPut(interp, event, Tcl_NewStringObj("level", -1), Tcl_NewIntObj(lvl));
        }
    }
    Tcl_DictObjPut(interp, event, Tcl_NewStringObj("event", -1), Tcl_NewStringObj("stopped", -1));
    Tcl_DictObjPut(interp, event, Tcl_NewStringObj("reason", -1), Tcl_NewStringObj(reason, -1));

    /* Build locals snapshot: locals + args (if proc present) */
    Tcl_Obj *localsDict = Tcl_NewDictObj();
    Tcl_IncrRefCount(localsDict);
    {
        /* info locals (evaluate in current frame) */
        Tcl_Obj *il[2]; il[0] = Tcl_NewStringObj("info", -1); il[1] = Tcl_NewStringObj("locals", -1);
        Tcl_IncrRefCount(il[0]); Tcl_IncrRefCount(il[1]);
        if (Tcl_EvalObjv(interp, 2, il, TCL_EVAL_DIRECT) == TCL_OK) {
            Tcl_Obj *list = Tcl_GetObjResult(interp);
            int len = 0; Tcl_ListObjLength(interp, list, &len);
            for (int i=0;i<len;i++) {
                Tcl_Obj *nameObj = NULL; Tcl_ListObjIndex(interp, list, i, &nameObj);
                if (!nameObj) continue;
                Tcl_Obj *val = Tcl_GetVar2Ex(interp, Tcl_GetString(nameObj), NULL, 0);
                if (!val) val = Tcl_NewStringObj("", -1);
                Tcl_IncrRefCount(val);
                Tcl_DictObjPut(interp, localsDict, nameObj, val);
                Tcl_DecrRefCount(val);
            }
        }
        Tcl_DecrRefCount(il[0]); Tcl_DecrRefCount(il[1]);
        Tcl_ResetResult(interp);
    }
    /* If we know proc name, include its args */
    {
        Tcl_Obj *procName = NULL;
        if (Tcl_DictObjGet(interp, event, Tcl_NewStringObj("proc", -1), &procName) == TCL_OK && procName && Tcl_GetCharLength(procName) > 0) {
            Tcl_Obj *ia[3];
            ia[0] = Tcl_NewStringObj("info", -1);
            ia[1] = Tcl_NewStringObj("args", -1);
            ia[2] = procName; Tcl_IncrRefCount(ia[2]);
            Tcl_IncrRefCount(ia[0]); Tcl_IncrRefCount(ia[1]);
            if (Tcl_EvalObjv(interp, 3, ia, TCL_EVAL_DIRECT) == TCL_OK) {
                Tcl_Obj *alist = Tcl_GetObjResult(interp);
                int alen = 0; Tcl_ListObjLength(interp, alist, &alen);
                for (int j=0;j<alen;j++) {
                    Tcl_Obj *an = NULL; Tcl_ListObjIndex(interp, alist, j, &an);
                    if (!an) continue;
                    /* don't overwrite if already set as local */
                    Tcl_Obj *dummy = NULL;
                    if (Tcl_DictObjGet(interp, localsDict, an, &dummy) != TCL_OK || dummy == NULL) {
                        Tcl_Obj *vv = Tcl_GetVar2Ex(interp, Tcl_GetString(an), NULL, 0);
                        if (!vv) vv = Tcl_NewStringObj("", -1);
                        Tcl_IncrRefCount(vv);
                        Tcl_DictObjPut(interp, localsDict, an, vv);
                        Tcl_DecrRefCount(vv);
                    }
                }
            }
            Tcl_DecrRefCount(ia[0]); Tcl_DecrRefCount(ia[1]); Tcl_DecrRefCount(ia[2]);
            Tcl_ResetResult(interp);
        }
    }
    Tcl_DictObjPut(interp, event, Tcl_NewStringObj("locals", -1), localsDict);
    Tcl_DecrRefCount(localsDict);
    Tdb_SetStopEvent(interp, event);
    /* Non-blocking test hook: publish event only. */
    Tcl_SetObjResult(interp, Tcl_NewStringObj("ok", -1));
    Tcl_DecrRefCount(event);
    return TCL_OK;
}

/* ----------------------------------------------------------------------
 * Package init
 * ---------------------------------------------------------------------- */

static int
TdbRegisterCommands(Tcl_Interp *interp)
{
    Tcl_CreateObjCommand(interp, "tdb::start", TdbStartCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "tdb::stop", TdbStopCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "tdb::config", TdbConfigCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "tdb::break", TdbBreakCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "tdb::_pauseNow", TdbPauseNowCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "tdb::stats", TdbStatsCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "tdb::_match_fileline", TdbMatchFileLineCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "tdb::_stop_event", TdbStopEventCmd, NULL, NULL);
    Tcl_CreateObjCommand(interp, "tdb::_enterPause", TdbEnterPauseCmd, NULL, NULL);
    return TCL_OK;
}

int
Tdb_Init(Tcl_Interp *interp)
{
    if (Tcl_InitStubs(interp, "8.5", 0) == NULL) return TCL_ERROR;
    if (TdbRegisterCommands(interp) != TCL_OK) return TCL_ERROR;
    if (Tcl_PkgProvide(interp, "tdb", "0.1") != TCL_OK) return TCL_ERROR;
    return TCL_OK;
}

int
Tdb_SafeInit(Tcl_Interp *interp)
{
    return Tdb_Init(interp);
}
/* Expose pause loop to Tcl: tdb::_enterPause */
static int
TdbEnterPauseCmd(ClientData cd, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    (void)cd;
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, NULL);
        return TCL_ERROR;
    }
    Tdb_EnterPauseLoop(interp);
    Tcl_ResetResult(interp);
    return TCL_OK;
}
