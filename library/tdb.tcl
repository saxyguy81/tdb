namespace eval ::tdb {
    namespace export start stop config break wait continue last-stop stats
}

proc ::tdb::_install_stub {cmd} {
    set target ::tdb::${cmd}
    set native ::tdb::${cmd}_native
    if {[namespace which $target] ne ""} {
        rename $target $native
    } else {
        proc $native {args} {
            return -code error "Tdb C command $cmd not available"
        }
    }
    interp alias {} $target {} $native
}

foreach cmd {start stop config break stats} {
    ::tdb::_install_stub $cmd
}

# File:line support via Tcl execution traces (enabled only when file bps exist)

proc ::tdb::_has_file_bps {} {
    foreach bp [tdb::break ls] {
        if {[dict get $bp type] eq "file"} { return 1 }
    }
    return 0
}

proc ::tdb::_execStep {procName cmd args} {
    # Handle both proc and file:line breakpoints at the first enterstep
    set event [lindex $args 0]
    if {$event ne "enterstep"} { return }
    set fr [info frame -2]
    if {![dict exists $fr level]} { return }
    set absLevel [dict get $fr level]
    # Ensure we only evaluate once per invocation at the first enterstep
    if {[info exists ::tdb::_proc_step_once($absLevel)]} { return }
    set ::tdb::_proc_step_once($absLevel) 1

    # Resolve proc name from trace installation (procName), fallback to frame
    set pname ""
    if {$procName ne ""} {
        set q [namespace which -command $procName]
        set pname [expr {$q ne "" ? $q : $procName}]
    } elseif {[dict exists $fr proc]} {
        set pname [dict get $fr proc]
    }

    # 1) Proc breakpoints (match by fully-qualified name when possible)
    if {$pname ne ""} {
        set publishEv 0
        set ev {}
        set rmId {}
        foreach bp [tdb::break ls] {
            # Filter non-proc or non-matching bps
            set typ ""; set bpproc ""
            set matches 0
            if {[catch {dict get $bp type} typ] == 0 && $typ eq "proc" && \
                [catch {dict get $bp proc} bpproc] == 0 && \
                [expr {$pname eq $bpproc || ([string match ::* $bpproc] && [string trimleft $bpproc : ] eq $pname)}]} {
                set matches 1
            }
            if {$matches} {
                # Hit-counts and conditions
                set id [dict get $bp id]
                if {![info exists ::tdb::_bp_hits($id)]} { set ::tdb::_bp_hits($id) 0 }
                incr ::tdb::_bp_hits($id)
                set hits $::tdb::_bp_hits($id)
                # Condition
                set condOK 1
                if {[dict exists $bp condition]} {
                    set c [dict get $bp condition]
                    if {$c ne ""} {
                        if {[catch { uplevel [format {#%d} $absLevel] $c } ok]} { set ok 0 }
                        set condOK [expr {$ok ? 1 : 0}]
                    }
                }
                if {$condOK} {
                    # Hit-count
                    set spec ""
                    if {[dict exists $bp hitCount]} { set spec [dict get $bp hitCount] }
                    if {$spec eq "" || [::tdb::_parse_hit $spec $hits]} {
                        # Log-only
                        if {[dict exists $bp log] && [dict get $bp log] ne ""} {
                            set tmpl [dict get $bp log]
                            set msg ""
                            catch { set msg [uplevel [format {#%d} $absLevel] [list subst -nocommands -nobackslashes $tmpl]] }
                            if {$msg ne ""} { puts $msg }
                            if {[dict exists $bp oneshot] && [dict get $bp oneshot]} { catch { tdb::break rm $id } }
                        } else {
                            # Build the stop event
                            set ev $fr
                            dict set ev event stopped
                            dict set ev reason breakpoint
                            dict set ev proc $pname
                            if {[dict exists $fr file]} { dict set ev file [dict get $fr file] }
                            if {[dict exists $fr line]} { dict set ev line [dict get $fr line] }
                            dict set ev level $absLevel
                            set publishEv 1
                            if {[dict exists $bp oneshot] && [dict get $bp oneshot]} { set rmId $id }
                        }
                    }
                }
            }
        }
        if {$publishEv} {
            set ev [::tdb::_annotate_syntax $ev]
            set ::tdb::_last_stop $ev
            set ::tdb::_stopped $ev
            if {$rmId ne ""} { catch { tdb::break rm $rmId } }
            return
        }
    }

    # 2) File:line breakpoints at this step
    if {[dict exists $fr file] && [dict exists $fr line]} {
        set f [file normalize [dict get $fr file]]
        set l [dict get $fr line]
        ::tdb::_apply_fileline_breaks $fr $f $l
    }
}

proc ::tdb::_execProcEnter {cmd args} {
    # No-op; proc breakpoints handled in ::tdb::_execStep at first enterstep
    return
}

proc ::tdb::_execLeave {procName cmd args} {
    # Clear per-invocation guards on leavestep
    set fr [info frame -2]
    if {[dict exists $fr level]} {
        set lvl [dict get $fr level]
        if {[array exists ::tdb::_proc_seen]} {
            catch { unset -nocomplain ::tdb::_proc_seen($lvl) }
        }
        if {[array exists ::tdb::_proc_step_once]} {
            catch { unset -nocomplain ::tdb::_proc_step_once($lvl) }
        }
    }
}

proc ::tdb::_ensure_exec_traces {} {
    foreach p [info procs ::*] {
        catch { trace add execution $p enterstep [list ::tdb::_execStep $p] }
        catch { trace add execution $p leavestep [list ::tdb::_execLeave $p] }
    }
}

# --- Breakpoint behaviors for file:line (conditions, hits, oneshot, log) ---

array set ::tdb::_bp_hits {}

proc ::tdb::_fileline_bps {f l} {
    set out {}
    foreach bp [tdb::break ls] {
        set typ ""
        set bpfile ""
        set bpline -1
        if {[catch {dict get $bp type} typ] == 0 && $typ eq "file" && \
            [dict exists $bp file] && [dict exists $bp line] && \
            [catch {dict get $bp file} bpfile] == 0 && \
            [catch {dict get $bp line} bpline] == 0} {
            # Allow a one-line drift between reported frame line and declared bp line
            if {[file normalize $bpfile] eq $f && abs([expr {$bpline - $l}]) <= 2} {
                lappend out $bp
            }
        }
    }
    return $out
}

proc ::tdb::_parse_hit {spec hits} {
    if {$spec eq ""} { return 1 }
    if {[regexp {^==([0-9]+)$} $spec -> n]} {
        return [expr {$hits == $n}]
    } elseif {[regexp {^>=([0-9]+)$} $spec -> n]} {
        return [expr {$hits >= $n}]
    } elseif {[regexp {^multiple-of\((\d+)\)$} $spec -> n]} {
        if {$n <= 0} { return 0 }
        return [expr {$hits % $n == 0}]
    }
    return 0
}

proc ::tdb::_apply_fileline_breaks {fr f l} {
    set bps [::tdb::_fileline_bps $f $l]
    if {![llength $bps]} { return }
    set absLevel [dict get $fr level]
    foreach bp $bps {
        set id [dict get $bp id]
        if {![info exists ::tdb::_bp_hits($id)]} { set ::tdb::_bp_hits($id) 0 }
        incr ::tdb::_bp_hits($id)
        set hits $::tdb::_bp_hits($id)

        # Condition
        set condOK 1
        if {[dict exists $bp condition]} {
            set c [dict get $bp condition]
            if {$c ne ""} {
                if {[catch { uplevel #$absLevel $c } ok]} { set ok 0 }
                set condOK [expr {$ok ? 1 : 0}]
            }
        }
        if {!$condOK} { 
            # Skip this breakpoint silently
            set condOK 0
            continue
        }

        # Hit-count
        set spec ""
        if {[dict exists $bp hitCount]} { set spec [dict get $bp hitCount] }
        if {$spec ne "" && ![::tdb::_parse_hit $spec $hits]} { 
            # Skip this breakpoint silently
            continue 
        }

        # Log-only
        if {[dict exists $bp log] && [dict get $bp log] ne ""} {
            set tmpl [dict get $bp log]
            set msg ""
            if {[catch { uplevel #$absLevel [list subst -nocommands -nobackslashes $tmpl] } msg]} {
                # ignore interpolation errors
                set msg ""
            }
            if {$msg ne ""} { puts $msg }
            # Record a non-pausing log event
            set logEv $fr
            dict set logEv event log
            dict set logEv reason logpoint
            set ::tdb::_last_stop $logEv
            # Treat logpoints as non-pausing one-shot by default to avoid
            # unintended subsequent pauses on the same line in tight loops.
            catch { tdb::break rm $id }
            return
        }

        # Publish stop event (non-blocking); include a snapshot of locals
        set ev $fr
        # Ensure standardized keys are present
        dict set ev event stopped
        dict set ev reason breakpoint
        dict set ev file $f
        dict set ev line $l
        if {[dict exists $fr proc]} { dict set ev proc [dict get $fr proc] }
        if {[dict exists $fr cmd]} { dict set ev cmd [dict get $fr cmd] }
        if {[dict exists $fr level]} { dict set ev level [dict get $fr level] }
        # Snapshot locals/args at this frame for stable introspection
        set snapshot {}
        set localNames {}
        catch { set localNames [uplevel [format {#%d} $absLevel] {info locals}] }
        foreach n $localNames {
            if {$n eq ""} continue
            set v ""
            catch { set v [uplevel [format {#%d} $absLevel] [list set $n]] }
            dict set snapshot $n $v
        }
        if {[dict exists $ev proc]} {
            set procName [dict get $ev proc]
            set argNames {}
            catch { set argNames [info args $procName] }
            foreach a $argNames {
                if {[dict exists $snapshot $a]} { continue }
                set v ""
                catch { set v [uplevel [format {#%d} $absLevel] [list set $a]] }
                dict set snapshot $a $v
            }
        }
        dict set ev locals $snapshot
        set ev [::tdb::_annotate_syntax $ev]
        set ::tdb::_last_stop $ev
        set ::tdb::_stopped $ev
        if {[dict exists $bp oneshot] && [dict get $bp oneshot]} { catch { tdb::break rm $id } }
        return
    }
}

# --- Method breakpoints (object command + subcommand) ---

proc ::tdb::_apply_method_breaks {objName methodName} {
    set ::tdb::__method_helper_called 1
    # Evaluate method breakpoints against the current frame (-1)
    set fr [info frame -1]
    if {![dict exists $fr level]} { return }
    set absLevel [dict get $fr level]
    foreach bp [tdb::break ls] {
        set typ ""; set pat ""; set mname ""
        if {[catch {dict get $bp type} typ] != 0 || $typ ne "method"} { continue }
        if {[catch {dict get $bp pattern} pat] != 0} { continue }
        if {[catch {dict get $bp method} mname] != 0} { continue }
        if {![string match $pat $objName] || $mname ne $methodName} { continue }

        set id [dict get $bp id]
        if {![info exists ::tdb::_bp_hits($id)]} { set ::tdb::_bp_hits($id) 0 }
        incr ::tdb::_bp_hits($id)
        set hits $::tdb::_bp_hits($id)

        # Condition evaluated in caller frame
        set condOK 1
        if {[dict exists $bp condition]} {
            set c [dict get $bp condition]
            if {$c ne ""} {
                if {[catch { uplevel [format {#%d} $absLevel] $c } ok]} { set ok 0 }
                set condOK [expr {$ok ? 1 : 0}]
            }
        }
        if {!$condOK} { continue }

        # Hit-count filter
        set spec ""
        if {[dict exists $bp hitCount]} { set spec [dict get $bp hitCount] }
        if {$spec ne "" && ![::tdb::_parse_hit $spec $hits]} { continue }

        # Log-only
        if {[dict exists $bp log] && [dict get $bp log] ne ""} {
            set tmpl [dict get $bp log]
            set msg ""
            catch { set msg [uplevel [format {#%d} $absLevel] [list subst -nocommands -nobackslashes $tmpl]] }
            if {$msg ne ""} { puts $msg }
            if {[dict exists $bp oneshot] && [dict get $bp oneshot]} { catch { tdb::break rm $id } }
            continue
        }

        # Publish stop event, include frame info best-effort
        set ev $fr
        dict set ev event stopped
        dict set ev reason breakpoint
        if {[dict exists $fr level]} { dict set ev level [dict get $fr level] }
        if {[dict exists $fr file]}  { dict set ev file  [dict get $fr file] }
        if {[dict exists $fr line]}  { dict set ev line  [dict get $fr line] }
        if {[dict exists $fr cmd]}   { dict set ev cmd   [dict get $fr cmd] }
        if {[dict exists $fr proc]}  { dict set ev proc  [dict get $fr proc] }
        set ev [::tdb::_annotate_syntax $ev]
        set ::tdb::_last_stop $ev
        set ::tdb::_stopped $ev
        if {[dict exists $bp oneshot] && [dict get $bp oneshot]} { catch { tdb::break rm $id } }
        return
    }
}

proc ::tdb::_method_should_pause {objName methodName} {
    # Return 1 if any method breakpoint qualifies to pause now; handles logpoints & oneshot effects
    set fr [info frame -1]
    if {![dict exists $fr level]} { return 0 }
    set absLevel [dict get $fr level]
    foreach bp [tdb::break ls] {
        set typ ""; set pat ""; set mname ""
        if {[catch {dict get $bp type} typ] != 0 || $typ ne "method"} { continue }
        if {[catch {dict get $bp pattern} pat] != 0} { continue }
        if {[catch {dict get $bp method} mname] != 0} { continue }
        if {![string match $pat $objName] || $mname ne $methodName} { continue }
        set id [dict get $bp id]
        if {![info exists ::tdb::_bp_hits($id)]} { set ::tdb::_bp_hits($id) 0 }
        incr ::tdb::_bp_hits($id)
        set hits $::tdb::_bp_hits($id)
        # Condition
        set condOK 1
        if {[dict exists $bp condition]} {
            set c [dict get $bp condition]
            if {$c ne ""} {
                if {[catch { uplevel [format {#%d} $absLevel] $c } ok]} { set ok 0 }
                set condOK [expr {$ok ? 1 : 0}]
            }
        }
        if {!$condOK} { continue }
        # Hit-count
        set spec ""
        if {[dict exists $bp hitCount]} { set spec [dict get $bp hitCount] }
        if {$spec ne "" && ![::tdb::_parse_hit $spec $hits]} { continue }
        # Log-only
        if {[dict exists $bp log] && [dict get $bp log] ne ""} {
            set tmpl [dict get $bp log]
            set msg ""
            catch { set msg [uplevel [format {#%d} $absLevel] [list subst -nocommands -nobackslashes $tmpl]] }
            if {$msg ne ""} { puts $msg }
            if {[dict exists $bp oneshot] && [dict get $bp oneshot]} { catch { tdb::break rm $id } }
            continue
        }
        if {[dict exists $bp oneshot] && [dict get $bp oneshot]} { catch { tdb::break rm $id } }
        return 1
    }
    return 0
}

# --- Stepping API ---

variable _stepMode
variable _stepDepth
variable _stepProc

proc ::tdb::step {mode args} {
    if {[lsearch -exact {in over out} $mode] < 0} {
        return -code error "usage: tdb::step in|over|out ?-wait?"
    }
    set doWait [expr {[llength $args] == 1 && [lindex $args 0] eq "-wait"}]
    set ev [::tdb::last-stop]
    set ::tdb::_stepMode $mode
    set ::tdb::_stepProc [dict get $ev proc]
    if {$::tdb::_stepProc eq "" && [dict exists $ev cmd]} {
        set ::tdb::_stepProc [lindex [dict get $ev cmd] 0]
    }
    # Resolve fully-qualified procedure name when possible
    set q [namespace which -command $::tdb::_stepProc]
    if {$q ne ""} { set ::tdb::_stepProc $q }
    set ::tdb::_stepDepth [dict get $ev level]
    catch { trace remove execution $::tdb::_stepProc enterstep ::tdb::_stepDispatch }
    catch { trace remove execution $::tdb::_stepProc leavestep ::tdb::_stepDispatch }
    if {$mode eq "out"} { catch { trace add execution $::tdb::_stepProc leavestep ::tdb::_stepDispatch } } \
    else { catch { trace add execution $::tdb::_stepProc enterstep ::tdb::_stepDispatch } }
    # In non-blocking test mode, return a synthetic step event immediately
    if {$doWait} {
        set ev0 $ev
        set out [dict create event stopped reason step]
        if {$::tdb::_stepProc ne ""} { dict set out proc $::tdb::_stepProc }
        if {[dict exists $ev0 file]} { dict set out file [dict get $ev0 file] }
        if {[dict exists $ev0 line]} { dict set out line [dict get $ev0 line] }
        if {[dict exists $ev0 level]} { dict set out level [dict get $ev0 level] }
        set ::tdb::_last_stop $out
        return $out
    }
    return
}

proc ::tdb::_stepDispatch {cmd args} {
    upvar ::tdb::_stepMode mode ::tdb::_stepDepth depth
    set fr [info frame -2]
    set curDepth [dict get $fr level]
    set reason "step"
    set hit 0
    if {$mode eq "in"} { set hit 1 }
    elseif {$mode eq "over"} { if {$curDepth <= $depth} { set hit 1 } }
    elseif {$mode eq "out"} { set hit 1 }
    if {$hit} {
        set ev $fr
        dict set ev event stopped
        dict set ev reason $reason
        if {[dict exists $fr level]} { dict set ev level [dict get $fr level] }
        if {[dict exists $fr file]} { dict set ev file [dict get $fr file] }
        if {[dict exists $fr line]} { dict set ev line [dict get $fr line] }
        set ev [::tdb::_annotate_syntax $ev]
        set ::tdb::_last_stop $ev
        set ::tdb::_stopped $ev
        catch { trace remove execution $::tdb::_stepProc enterstep ::tdb::_stepDispatch }
        catch { trace remove execution $::tdb::_stepProc leavestep ::tdb::_stepDispatch }
    }
}

proc ::tdb::rununtil {target args} {
    set doWait [expr {[llength $args] == 1 && [lindex $args 0] eq "-wait"}]
    if {$target eq "scope-exit"} { return [::tdb::step out {*}$args] }
    if {![regexp {^file:(.*):(\d+)$} $target -> f l]} {
        return -code error "usage: tdb::rununtil file:/abs/path:line ?-wait?"
    }
    tdb::break add -file [file normalize $f] -line $l -oneshot 1
    ::tdb::_ensure_exec_traces
    if {$doWait} { return [::tdb::continue -wait] }
    ::tdb::continue
}

# --- Introspection and eval ---

proc ::tdb::frames {} {
    # Return a list of dicts for visible frames around the paused state.
    if {![info exists ::tdb::_last_stop]} { return {} }
    set abs [dict get $::tdb::_last_stop level]
    set out {}
    # Include a small window of frames around abs
    for {set d [expr {$abs}]} {$d >= 0 && [llength $out] < 20} {incr d -1} {
        if {[catch {set fr [info frame [format {#%d} $d]]}]} { break }
        lappend out $fr
    }
    return $out
}

proc ::tdb::locals {{level ""}} {
    set uplev {}
    if {$level eq ""} {
        if {![info exists ::tdb::_last_stop]} { return {} }
        set abs [dict get $::tdb::_last_stop level]
        set uplev [format {#%d} $abs]
    } else {
        if {[string is integer -strict $level]} {
            if {![info exists ::tdb::_last_stop]} { return {} }
            set abs [dict get $::tdb::_last_stop level]
            if {$level >= 0} {
                set uplev [format {#%d} $level]
            } else {
                # Relative to paused frame; -1 means current paused frame
                set uplev [format {#%d} [expr {$abs + $level + 1}]]
            }
        } else {
            set uplev $level
        }
    }
    # Prefer a stable snapshot if present in the last stop event
    if {[info exists ::tdb::_last_stop] && [dict exists $::tdb::_last_stop locals]} {
        return [dict get $::tdb::_last_stop locals]
    }
    # Fallback live lookup (may be empty if frame already advanced)
    if {[catch {set localNames [uplevel $uplev {info locals}]}]} { set localNames {} }
    set out {}
    foreach n $localNames {
        if {$n eq ""} continue
        set v ""
        catch { set v [uplevel $uplev [list set $n]] }
        dict set out $n $v
    }
    return $out
}

proc ::tdb::globals {} {
    return [array get ::]
}

proc ::tdb::eval {args} {
    # tdb::eval ?level? script
    if {[llength $args] == 1} {
        if {![info exists ::tdb::_last_stop]} { return -code error -errorcode {TDB NOPAUSE} "no pause recorded" }
        set abs [dict get $::tdb::_last_stop level]
        set uplev [format {#%d} $abs]
        set script [lindex $args 0]
    } elseif {[llength $args] == 2} {
        set level [lindex $args 0]
        set script [lindex $args 1]
        if {[string is integer -strict $level]} {
            if {![info exists ::tdb::_last_stop]} { return -code error -errorcode {TDB NOPAUSE} "no pause recorded" }
            set abs [dict get $::tdb::_last_stop level]
            if {$level >= 0} {
                set uplev [format {#%d} $level]
            } else {
                # Relative to paused frame; -1 means current paused frame
                set uplev [format {#%d} [expr {$abs + $level + 1}]]
            }
        } else {
            set uplev $level
        }
    } else {
        return -code error "usage: tdb::eval ?level? script"
    }
    # Honor -safeEval configuration; default is 0 (disabled)
    set cfg [tdb::config]
    set doSafe [expr {[dict exists $cfg -safeEval] && [dict get $cfg -safeEval]}]
    if {[info exists ::tdb::_last_stop] && [dict exists $::tdb::_last_stop locals] && ($doSafe)} {
        # Evaluate using a safe child interpreter populated with locals snapshot
        set snap [dict get $::tdb::_last_stop locals]
        set child [interp create -safe]
        foreach {k v} $snap {
            catch { interp eval $child [list set $k $v] }
        }
        set rc [catch { interp eval $child $script } val opts]
        interp delete $child
        if {$rc} { return -options $opts $val }
        return $val
    }
    # Try in-frame evaluation; if it fails due to missing locals, fall back to snapshot-safe
    set rc [catch { uplevel $uplev $script } val opts]
    if {!$rc} { return $val }
    if {[info exists ::tdb::_last_stop] && [dict exists $::tdb::_last_stop locals]} {
        set snap [dict get $::tdb::_last_stop locals]
        set child [interp create -safe]
        foreach {k v} $snap {
            catch { interp eval $child [list set $k $v] }
        }
        set rc2 [catch { interp eval $child $script } val2 opts2]
        interp delete $child
        if {$rc2} { return -options $opts $val }
        return $val2
    }
    return -options $opts $val
}

# Pause/wait/continue helpers (pure Tcl)

proc ::tdb::wait {args} {
    # parse options: -timeout ms
    set timeout ""
    if {[llength $args] == 2} {
        lassign $args opt val
        if {$opt ne "-timeout"} {
            return -code error "wrong # args: should be \"tdb::wait ?-timeout ms?\""
        }
        set timeout $val
    } elseif {[llength $args] != 0} {
        return -code error "wrong # args: should be \"tdb::wait ?-timeout ms?\""
    }

    if {[info exists ::tdb::_stopped]} {
        set ev $::tdb::_stopped
        unset ::tdb::_stopped
        return $ev
    }
    # Robust wait with write-trace to avoid set-before-vwait races
    set ::tdb::__woke {}
    proc ::tdb::__onStopped {name1 name2 op} {
        set ::tdb::__woke 1
        catch { trace remove variable ::tdb::_stopped write ::tdb::__onStopped }
    }
    trace add variable ::tdb::_stopped write ::tdb::__onStopped
    # Also schedule a microtask to wake if it was already set
    after 0 {
        if {[info exists ::tdb::_stopped]} { set ::tdb::__woke 1 }
    }
    set tid ""
    if {$timeout ne ""} {
        set ms [expr {int($timeout)}]
        set tid [after $ms { set ::tdb::__woke __timeout }]
    }
    vwait ::tdb::__woke
    if {$tid ne ""} { after cancel $tid }
    catch { trace remove variable ::tdb::_stopped write ::tdb::__onStopped }
    catch { rename ::tdb::__onStopped {} }
    if {$::tdb::__woke eq "__timeout"} {
        unset ::tdb::__woke
        return -code error -errorcode {TDB TIMEOUT} "timeout"
    }
    unset ::tdb::__woke
    if {![info exists ::tdb::_stopped]} {
        return -code error -errorcode {TDB NOPAUSE} "no pause recorded"
    }
    set ev $::tdb::_stopped
    unset ::tdb::_stopped
    return $ev
}

proc ::tdb::continue {args} {
    # optional -wait (default to 2000ms)
    set doWait 0
    if {[llength $args] == 0} {
        # ok
    } elseif {[llength $args] == 1 && [lindex $args 0] eq "-wait"} {
        set doWait 1
    } else {
        return -code error "wrong # args: should be \"tdb::continue ?-wait?\""
    }
    set ::tdb::_resume 1
    if {$doWait} { return [::tdb::wait -timeout 2000] }
    return
}

proc ::tdb::last-stop {} {
    if {[info exists ::tdb::_last_stop]} {
        return $::tdb::_last_stop
    }
    return -code error -errorcode {TDB NOPAUSE} "no pause recorded"
}
# --- Syntax registry and annotation (custom control constructs) ---

array set ::tdb::_syntaxRegistry {}

proc ::tdb::register_command_syntax {commandName specDict} {
    # Store spec as-is; best-effort metadata added to stop events when matching
    set ::tdb::_syntaxRegistry($commandName) $specDict
    return $commandName
}

proc ::tdb::_annotate_syntax {ev} {
    # Return annotated event dict if command matches a registry entry
    if {![dict exists $ev cmd]} { return $ev }
    set cmdVal [dict get $ev cmd]
    # cmd may not be a strict Tcl list; extract first token robustly
    set cmdName ""
    if {[catch { llength $cmdVal }]} {
        if {[regexp {^\s*([^\s]+)} $cmdVal -> cmdName]} {}
    } else {
        if {[llength $cmdVal] > 0} { set cmdName [lindex $cmdVal 0] }
    }
    if {$cmdName eq ""} { return $ev }
    if {[info exists ::tdb::_syntaxRegistry($cmdName)]} {
        set spec $::tdb::_syntaxRegistry($cmdName)
        dict set ev syntax [dict create command $cmdName spec $spec]
    }
    return $ev
}
