#!/usr/bin/env tclsh
# Minimal JSON-ish server for tdb (Prompt B)

if {[catch {package require tdb} err]} {
    puts stderr "tdb package not available: $err"
    exit 1
}

# Extremely small JSON encoder/decoder for simple dicts
proc toJson {d} {
    set parts {}
    foreach {k v} $d {
        lappend parts \"$k\":\"[string map {" \" \\ \\\n+} $v]\"
    }
    return "{[join $parts ,]}"
}

proc fromJson {s} {
    # expects flat object {"k":"v",...}
    set s [string trim $s]
    if {![string match \{*\} $s]} { return {} }
    set s [string range $s 1 end-1]
    set out {}
    foreach pair [split $s ,] {
        if {[regexp {^\s*\"([^\"]+)\"\s*:\s*\"([^\"]*)\"\s*$} $pair -> k v]} {
            lappend out $k $v
        }
    }
    return $out
}

proc send {chan d} {
    puts $chan [toJson $d]
    flush $chan
}

proc handle {chan} {
    set line [gets $chan]
    if {$line < 0} { close $chan; return }
    set req [fromJson $line]
    set cmd [dict get $req command]
    set res {}
    if {$cmd eq "initialize"} {
        tdb::start
        set res {ok ok}
    } elseif {$cmd eq "setBreakpoints"} {
        # params: file, lines (comma-separated)
        set file [dict get $req file]
        set lines [split [dict get $req lines] ,]
        foreach l $lines { if {$l ne ""} { tdb::break add -file $file -line $l } }
        set res {ok ok}
    } elseif {$cmd eq "continue"} {
        set ev [tdb::continue -wait]
        set res [list event [dict get $ev reason]]
    } elseif {$cmd eq "next"} {
        set ev [tdb::step over -wait]
        set res [list event [dict get $ev reason]]
    } elseif {$cmd eq "stepIn"} {
        set ev [tdb::step in -wait]
        set res [list event [dict get $ev reason]]
    } elseif {$cmd eq "stepOut"} {
        set ev [tdb::step out -wait]
        set res [list event [dict get $ev reason]]
    } elseif {$cmd eq "stackTrace"} {
        set fr [tdb::frames]
        set res [list frames [llength $fr]]
    } elseif {$cmd eq "scopes"} {
        set res {scopes locals,globals}
    } elseif {$cmd eq "variables"} {
        set scope [dict get $req scope]
        if {$scope eq "locals"} {
            set vars [tdb::locals]
        } else {
            set vars [tdb::globals]
        }
        set res [list vars [string length $vars]]
    } elseif {$cmd eq "evaluate"} {
        set expr [dict get $req expr]
        set res [list result [tdb::eval -1 $expr]]
    } elseif {$cmd eq "wait"} {
        set ev [tdb::wait -timeout 5000]
        set res [list event [dict get $ev reason]]
    } else {
        set res [list error "unknown command $cmd"]
    }
    send $chan $res
}

proc accept {chan addr port} {
    fconfigure $chan -translation lf -buffering line
    fileevent $chan readable [list handle $chan]
}

set port [expr {[info exists ::env(TDB_DAP_PORT)] ? $::env(TDB_DAP_PORT) : 4711}]
set srv [socket -server accept $port]
puts "tdb-dap listening on $port"
vwait forever

