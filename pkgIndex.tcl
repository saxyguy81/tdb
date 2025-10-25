package ifneeded tdb 0.1 [list ::apply {{dir} {
    load [file join $dir libtdb[info sharedlibextension]]
    source [file join $dir library tdb.tcl]
}} $dir]
