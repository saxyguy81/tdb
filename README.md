# tdb — Tcl Debugger Extension

This project provides the starting point for a Tcl extension named **tdb** (Tcl Debugger). It follows the Tcl Extension Architecture (TEA) and targets Tcl 8.5 and 8.6 via the stubs interface.

## Prerequisites

- Autoconf (for generating `configure`)
- A C compiler toolchain (Xcode Command Line Tools on macOS)
- Tcl interpreters for 8.5 **and** 8.6 (instructions below)

## Tcl Toolchains

### Tcl 8.6 (Homebrew)

If you are on macOS with Homebrew:

```sh
brew install tcl-tk@8
```

`/opt/homebrew/opt/tcl-tk@8/bin/tclsh8.6` will be used by default. Override it by exporting `TCLSH86=/path/to/tclsh8.6`.

### Tcl 8.5 (local build)

Tcl 8.5 is no longer packaged by Homebrew, so we build it from source into `~/.local/tcl8.5`.

```sh
cd ~/CascadeProjects
curl -LO https://prdownloads.sourceforge.net/tcl/tcl8.5.19-src.tar.gz
tar xf tcl8.5.19-src.tar.gz
cd tcl8.5.19
patch -p1 < /Users/smhanan/CascadeProjects/tdb/docs/tcl85_ptrdiff.patch
cd unix
ac_cv_func_strtod=yes tcl_cv_strtod_unbroken=ok tcl_cv_strtod_buggy=ok \
  ./configure --prefix="$HOME/.local/tcl8.5"
make -j8
make install
```

Add the newly installed interpreter to your `PATH`:

```sh
echo 'export PATH="$HOME/.local/tcl8.5/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

You can change the interpreter path later via `TCLSH85=/path/to/tclsh8.5`.

## Building the Extension

Configure against the Tcl version you want to build with:

```sh
# Build against Tcl 8.6 (Homebrew default)
./configure --with-tcl=/opt/homebrew/opt/tcl-tk@8/lib

# Build against local Tcl 8.5
./configure --with-tcl=$HOME/.local/tcl8.5/lib

make
make install   # optional
```

Re-run `./configure` whenever you switch Tcl versions so stubs/headers are updated.

## Test Matrix

Use the helper script to run the smoke suite across the available interpreters:

```sh
./scripts/test-matrix.sh
```

The script respects `TCLSH85` and `TCLSH86`. If either interpreter is missing it is reported as skipped.

You can still invoke interpreters directly if desired:

```sh
tclsh8.6 tests/all.tcl
tclsh8.5 tests/all.tcl
```

These tests currently ensure `package require tdb` works and that the placeholder commands return success.

## Runtime Commands

- `tdb::config ?-option value ...?` — view or set `-perf.allowInline` and `-path.normalize`.
- `tdb::start` / `tdb::stop` — enable or disable the engine (idempotent).
- `tdb::break add|rm|clear|ls` — manage in-memory breakpoints for files, procs, or methods.

See `tests/01_config.test` and `tests/10_break_api.test` for sample interactions.
