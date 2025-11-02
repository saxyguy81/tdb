# Ensure absolute path to satisfy hardened loaders (no relative dlopen).
# Embed the normalized directory of this pkgIndex.tcl at index time.
package ifneeded tdb 0.1 [list ::apply {{dir} {
    set d [file normalize $dir]
    load [file join $d libtdb[info sharedlibextension]]
    source [file join $d library tdb.tcl]
}} [file normalize [file dirname [info script]]]]
