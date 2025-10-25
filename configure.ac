AC_PREREQ([2.69])
AC_INIT([tdb],[0.1],[support@example.com])
AC_CONFIG_SRCDIR([generic/tdb_engine.c])
AC_CONFIG_AUX_DIR([.])
AC_CANONICAL_HOST

m4_include([tclconfig/tcl.m4])

TEA_INIT([tdb],[0.1])

AC_PROG_CC
AC_PROG_INSTALL

SC_PATH_TCLCONFIG
SC_LOAD_TCLCONFIG

# Determine how to build shared libraries on this platform.
case $host_os in
  darwin*)
    SHLIB_SUFFIX=".dylib"
    SHLIB_LD="${CC}"
    SHLIB_LDFLAGS="-dynamiclib -Wl,-undefined,dynamic_lookup"
    SHLIB_CFLAGS="-fno-common"
    ;;
  mingw*|msys*)
    SHLIB_SUFFIX=".dll"
    SHLIB_LD="${CC}"
    SHLIB_LDFLAGS="-shared"
    SHLIB_CFLAGS=""
    ;;
  *)
    SHLIB_SUFFIX=".so"
    SHLIB_LD="${CC}"
    SHLIB_LDFLAGS="-shared"
    SHLIB_CFLAGS="-fPIC"
    ;;
esac

PKG_LIB_FILE="libtdb${SHLIB_SUFFIX}"

AC_SUBST([SHLIB_SUFFIX])
AC_SUBST([SHLIB_LD])
AC_SUBST([SHLIB_LDFLAGS])
AC_SUBST([SHLIB_CFLAGS])
AC_SUBST([PKG_LIB_FILE])

# Ensure we compile against the Tcl stubs library
AC_DEFINE([USE_TCL_STUBS],[1],[Build against the Tcl stubs library])

AC_SUBST([TDB_VERSION],[0.1])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
