package require tcltest 2

namespace import ::tcltest::*

set scriptDir [file normalize [file dirname [info script]]]
set pkgDir [file dirname $scriptDir]

if {[info exists ::env(TCLLIBPATH)]} {
    set ::env(TCLLIBPATH) "$pkgDir $::env(TCLLIBPATH)"
} else {
    set ::env(TCLLIBPATH) $pkgDir
}

tcltest::configure -testdir $scriptDir
runAllTests
