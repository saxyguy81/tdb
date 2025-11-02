package require tcltest 2

namespace import ::tcltest::*

set scriptDir [file normalize [file dirname [info script]]]
set pkgDir [file dirname $scriptDir]

if {[info exists ::env(TCLLIBPATH)]} {
    set ::env(TCLLIBPATH) "$pkgDir $::env(TCLLIBPATH)"
} else {
    set ::env(TCLLIBPATH) $pkgDir
}

# Ensure package path for singleproc mode
if {[lsearch -exact $::auto_path $pkgDir] < 0} {
    lappend ::auto_path $pkgDir
}

# Set sane defaults to avoid hangs in CI: per-test timeout 10s, single process
tcltest::configure -testdir $scriptDir -singleproc 1
runAllTests
