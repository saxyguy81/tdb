#!/usr/bin/env tclsh
# Tiny REPL for tdb (Prompt A)

if {[catch {package require tdb} err]} {
    puts stderr "tdb package not available: $err"
    exit 1
}

proc usage {} {
    puts "Commands:\n  start|stop\n  break file:/abs/path:line\n  break proc ::qualified\n  break method ::glob method\n  ls|clear|rm <id>\n  c (continue)\n  s (step in)\n  n (step over)\n  fin (step out)\n  frames\n  locals ?level?\n  globals\n  eval ?level? {script}\n  wait ?ms?\n  last\n  help|quit|exit"
}

tdb::start
puts "tdb REPL. Type 'help' for commands."
while {1} {
    puts -nonewline "tdb> "
    flush stdout
    if {[gets stdin line] < 0} break
    set line [string trim $line]
    if {$line eq ""} continue
    if {$line in {exit quit}} break
    if {$line eq "help"} { usage; continue }
    set cmd [lindex $line 0]
    catch { set args [lrange $line 1 end] }
    set out ""
    set rc [catch {
        switch -exact -- $cmd {
            start { tdb::start; set out ok }
            stop { tdb::stop; set out ok }
            break {
                set sub [lindex $args 0]
                if {[regexp {^file:(.*):(\d+)$} $sub -> f l]} {
                    set out [tdb::break add -file [file normalize $f] -line $l]
                } elseif {[string match proc* $sub]} {
                    set name [lindex $args 1]
                    set out [tdb::break add -proc $name]
                } elseif {[string match method* $sub]} {
                    set pat [lindex $args 1]; set m [lindex $args 2]
                    set out [tdb::break add -method $pat $m]
                } elseif {$sub eq "ls"} {
                    set out [tdb::break ls]
                } elseif {$sub eq "clear"} {
                    tdb::break clear; set out ok
                } elseif {$sub eq "rm"} {
                    tdb::break rm [lindex $args 1]; set out ok
                } else {
                    error "unknown break form"
                }
            }
            ls { set out [tdb::break ls] }
            clear { tdb::break clear; set out ok }
            rm { tdb::break rm [lindex $args 0]; set out ok }
            c { set out [tdb::continue -wait] }
            s { set out [tdb::step in -wait] }
            n { set out [tdb::step over -wait] }
            fin { set out [tdb::step out -wait] }
            frames { set out [tdb::frames] }
            locals { set out [eval [list tdb::locals] $args] }
            globals { set out [tdb::globals] }
            eval { set out [eval [list tdb::eval] $args] }
            last { set out [tdb::last-stop] }
            wait {
                set ms [expr {[llength $args] ? [lindex $args 0] : 2000}]
                set out [tdb::wait -timeout $ms]
            }
            default { error "unknown command: $cmd" }
        }
    } out opts]
    if {$rc} { puts stderr "ERROR: $out" } else { puts $out }
}

