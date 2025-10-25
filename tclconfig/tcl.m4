# Minimal subset of TEA macros used by the tdb extension.

AC_DEFUN([SC_PATH_TCLCONFIG], [
    AC_MSG_CHECKING([for Tcl configuration])
    AC_ARG_WITH([tcl],
        [AS_HELP_STRING([--with-tcl=DIR],[directory containing tclConfig.sh])],
        [tcl_cv_tclconfig="$withval"],
        [tcl_cv_tclconfig=""])
    for dir in \
        "$tcl_cv_tclconfig" \
        "$exec_prefix/lib" \
        "$prefix/lib" \
        /usr/lib \
        /usr/local/lib \
        /opt/homebrew/opt/tcl-tk/lib \
        /opt/local/lib; do
        if test -n "$dir" && test -f "$dir/tclConfig.sh"; then
            tcl_cv_tclconfig="$dir"
            break
        fi
    done
    if test ! -f "$tcl_cv_tclconfig/tclConfig.sh"; then
        AC_MSG_ERROR([Unable to locate tclConfig.sh. Use --with-tcl=DIR.])
    fi
    AC_MSG_RESULT([$tcl_cv_tclconfig])
    TCL_CONFIG_DIR="$tcl_cv_tclconfig"
    AC_SUBST([TCL_CONFIG_DIR])
])

AC_DEFUN([SC_LOAD_TCLCONFIG], [
    AC_MSG_CHECKING([for Tcl configuration script])
    if test ! -f "$TCL_CONFIG_DIR/tclConfig.sh"; then
        AC_MSG_ERROR([tclConfig.sh not found in $TCL_CONFIG_DIR])
    fi
    . "$TCL_CONFIG_DIR/tclConfig.sh"
    AC_MSG_RESULT([found])
    AC_SUBST([TCL_VERSION])
    AC_SUBST([TCL_MAJOR_VERSION])
    AC_SUBST([TCL_MINOR_VERSION])
    AC_SUBST([TCL_PATCH_LEVEL])
    AC_SUBST([TCL_SRC_DIR])
    AC_SUBST([TCL_LIB_SPEC])
    AC_SUBST([TCL_INCLUDE_SPEC])
    AC_SUBST([TCL_STUB_LIB_SPEC])
])

AC_DEFUN([TEA_INIT], [
    TEA_PACKAGE_NAME="$1"
    TEA_PACKAGE_VERSION="$2"
    AC_SUBST([TEA_PACKAGE_NAME])
    AC_SUBST([TEA_PACKAGE_VERSION])
])
