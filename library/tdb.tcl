namespace eval ::tdb {
    namespace export start stop config break
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

foreach cmd {start stop config break} {
    ::tdb::_install_stub $cmd
}
