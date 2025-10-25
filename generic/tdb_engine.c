#include <tcl.h>
#include <string.h>

#ifndef TCL_ALLOW_INLINE_COMPILATION
#define TCL_ALLOW_INLINE_COMPILATION 0
#endif

#define TDB_GLOBAL_VAR_STOPPED "::tdb::_stopped"
#define TDB_GLOBAL_VAR_LAST_STOP "::tdb::_last_stop"
#define TDB_GLOBAL_VAR_RESUME "::tdb::_resume"

typedef enum {
    TDB_BP_NONE = 0,
    TDB_BP_FILE,
    TDB_BP_PROC,
    TDB_BP_METHOD
} TdbBreakpointType;

typedef struct TdbBreakpoint {
    int id;
    TdbBreakpointType type;
    Tcl_Obj *filePath;
    int line;
    Tcl_Obj *procName;
    Tcl_Obj *methodPattern;
    Tcl_Obj *methodName;
    Tcl_Obj *condition;
    Tcl_Obj *hitCountSpec;
    int oneshot;
    Tcl_Obj *logMessage;
} TdbBreakpoint;

typedef struct {
    Tcl_Interp *interp;
    int started;
    int perfAllowInline;
    int pathNormalize;
    int nextBreakpointId;

    Tcl_HashTable breakpoints;
    int fileBreakpointCount;
    int procBreakpointCount;
    int methodBreakpointCount;

    int isPaused;
    Tcl_Obj *lastStopDict;
} TdbState;

static void TdbStateCleanup(ClientData clientData, Tcl_Interp *interp);
static TdbState *TdbGetState(Tcl_Interp *interp);
static void TdbBreakpointFree(TdbBreakpoint *bp);
static void TdbBreakpointClearAll(TdbState *state);
static Tcl_Obj *TdbBreakpointToDict(Tcl_Interp *interp, const TdbBreakpoint *bp);
static Tcl_Obj *TdbMaybeNormalizePath(TdbState *state, Tcl_Obj *pathObj);
static int TdbConfigCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int TdbStartCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int TdbStopCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int TdbBreakCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int TdbPauseNowCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static void Tdb_SetStopEvent(Tcl_Interp *interp, Tcl_Obj *eventDict);
static void Tdb_EnterPauseLoop(Tcl_Interp *interp);

static int TdbBreakAddCmd(TdbState *state, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int TdbBreakRmCmd(TdbState *state, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int TdbBreakClearCmd(TdbState *state, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int TdbBreakListCmd(TdbState *state, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
static int CompareInts(const void *a, const void *b);
static void
TdbAdjustCounts(TdbState *state, TdbBreakpointType type, int delta)
{
    switch (type) {
        case TDB_BP_FILE:
            state->fileBreakpointCount += delta;
            if (state->fileBreakpointCount < 0) state->fileBreakpointCount = 0;
            break;
        case TDB_BP_PROC:
            state->procBreakpointCount += delta;
            if (state->procBreakpointCount < 0) state->procBreakpointCount = 0;
            break;
        case TDB_BP_METHOD:
            state->methodBreakpointCount += delta;
            if (state->methodBreakpointCount < 0) state->methodBreakpointCount = 0;
            break;
        default:
            break;
    }
}

static void
TdbBreakpointFree(TdbBreakpoint *bp)
{
    if (!bp) {
        return;
    }
    if (bp->filePath) {
        Tcl_DecrRefCount(bp->filePath);
    }
    if (bp->procName) {
        Tcl_DecrRefCount(bp->procName);
    }
    if (bp->methodPattern) {
        Tcl_DecrRefCount(bp->methodPattern);
    }
    if (bp->methodName) {
        Tcl_DecrRefCount(bp->methodName);
    }
    if (bp->condition) {
        Tcl_DecrRefCount(bp->condition);
    }
    if (bp->hitCountSpec) {
        Tcl_DecrRefCount(bp->hitCountSpec);
    }
    if (bp->logMessage) {
        Tcl_DecrRefCount(bp->logMessage);
    }
    ckfree(bp);
}

static void
TdbRemoveBreakpointEntry(TdbState *state, Tcl_HashEntry *entry)
{
    if (entry == NULL) {
        return;
    }
    TdbBreakpoint *bp = (TdbBreakpoint *)Tcl_GetHashValue(entry);
    if (bp) {
        TdbAdjustCounts(state, bp->type, -1);
    }
    TdbBreakpointFree(bp);
    Tcl_DeleteHashEntry(entry);
}

static void
TdbBreakpointClearAll(TdbState *state)
{
    Tcl_HashSearch search;
    Tcl_HashEntry *entry = Tcl_FirstHashEntry(&state->breakpoints, &search);
    while (entry != NULL) {
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
    if (!state) {
        return;
    }
    TdbBreakpointClearAll(state);
    Tcl_DeleteHashTable(&state->breakpoints);
    if (state->lastStopDict) {
        Tcl_DecrRefCount(state->lastStopDict);
    }
    ckfree(state);
}
            state->methodBreakpointCount += delta;
            if (state->methodBreakpointCount < 0) state->methodBreakpointCount = 0;
            break;
        default:
            break;
    }
}

static void
TdbBreakpointFree(TdbBreakpoint *bp)
{
    if (!bp) {
        return;
    }
    if (bp->filePath) {
        Tcl_DecrRefCount(bp->filePath);
    }
    if (bp->procName) {
        Tcl_DecrRefCount(bp->procName);
    }
    if (bp->methodPattern) {
        Tcl_DecrRefCount(bp->methodPattern);
    }
    if (bp->methodName) {
        Tcl_DecrRefCount(bp->methodName);
    }
    if (bp->condition) {
        Tcl_DecrRefCount(bp->condition);
    }
    if (bp->hitCountSpec) {
        Tcl_DecrRefCount(bp->hitCountSpec);
    }
    if (bp->logMessage) {
        Tcl_DecrRefCount(bp->logMessage);
    }
    ckfree(bp);
}

static void
TdbRemoveBreakpointEntry(TdbState *state, Tcl_HashEntry *entry)
{
    if (entry == NULL) {
        return;
    }
    TdbBreakpoint *bp = (TdbBreakpoint *)Tcl_GetHashValue(entry);
    if (bp) {
        TdbAdjustCounts(state, bp->type, -1);
    }
    TdbBreakpointFree(bp);
    Tcl_DeleteHashEntry(entry);
}

static void
TdbBreakpointClearAll(TdbState *state)
{
    Tcl_HashSearch search;
    Tcl_HashEntry *entry = Tcl_FirstHashEntry(&state->breakpoints, &search);
    while (entry != NULL) {
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
    if (!state) {
        return;
    }
    TdbBreakpointClearAll(state);
    Tcl_DeleteHashTable(&state->breakpoints);
    if (state->lastStopDict) {
        Tcl_DecrRefCount(state->lastStopDict);
    }
    ckfree(state);
}
static TdbState *
TdbGetState(Tcl_Interp *interp)
{
    TdbState *state = (TdbState *)Tcl_GetAssocData(interp, "tdb::state", NULL);
    if (state != NULL) {
        return state;
    }
    state = (TdbState *)ckalloc(sizeof(TdbState));
    memset(state, 0, sizeof(TdbState));
    state->interp = interp;
    state->perfAllowInline = 1;
    state->pathNormalize = 1;
    state->nextBreakpointId = 1;
    Tcl_InitHashTable(&state->breakpoints, TCL_ONE_WORD_KEYS);
    Tcl_SetAssocData(interp, "tdb::state", TdbStateCleanup, state);
    return state;
}
static Tcl_Obj *
TdbMaybeNormalizePath(TdbState *state, Tcl_Obj *pathObj)
{
    Tcl_Obj *result = pathObj;
    if (state->pathNormalize) {
        Tcl_Obj *norm = Tcl_FSGetNormalizedPath(state->interp, pathObj);
        if (norm != NULL) {
            result = norm;
        }
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
    if (bp->methodName) {
        Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("method", -1), bp->methodName);
        Tcl_IncrRefCount(bp->methodName);
        Tcl_DecrRefCount(bp->methodName);
    }
    if (bp->condition) {
        Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("condition", -1), bp->condition);
        Tcl_IncrRefCount(bp->condition);
        Tcl_DecrRefCount(bp->condition);
    }
    if (bp->hitCountSpec) {
        Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("hitCount", -1), bp->hitCountSpec);
        Tcl_IncrRefCount(bp->hitCountSpec);
        Tcl_DecrRefCount(bp->hitCountSpec);
    }
    if (bp->logMessage) {
        Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("log", -1), bp->logMessage);
        Tcl_IncrRefCount(bp->logMessage);
        Tcl_DecrRefCount(bp->logMessage);
    }

    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("oneshot", -1), Tcl_NewBooleanObj(bp->oneshot));
    return dict;
}
