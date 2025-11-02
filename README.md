# tdb — Tcl Debugger Extension

tdb is a Tcl debugger implemented as a TEA C extension with a small Tcl shim. It targets Tcl 8.5 and 8.6 via stubs, installs one global object trace only when needed, and keeps the hot path fast.

Key features
- Start/stop engine; robust pause/wait/continue plumbing (non‑blocking).
- Breakpoints
  - File:line, Proc, and Method (object command + subcommand)
  - Options: `-condition`, `-hitCount`, `-oneshot`, `-log` (logpoint)
  - Method conditions are evaluated using a `$cmd` list (see below).
- Stepping: step in/over/out, run‑to‑cursor, run‑until scope exit.
- Frames/locals/globals/eval; `-safeEval` configuration for sandboxed eval.
- Custom control constructs: register command syntax; best‑effort annotations on stop events.
- Performance: opt‑in smoke test validates low overhead when tracer is idle.

## Prerequisites

- Autoconf (for `configure` already provided in repo)
- A C compiler toolchain (Xcode CLT on macOS; build‑essential on Linux)
- Tcl 8.6 (and optionally 8.5) interpreters for tests

## Tcl Toolchains

### Tcl 8.6 (macOS, Homebrew)

If you are on macOS with Homebrew:

```sh
./scripts/install-tcl86-macos.sh
# then add to PATH if suggested by the script, e.g.
export PATH="$(brew --prefix tcl-tk)/bin:$PATH"
```

Common locations for `tclsh8.6`:

- Apple Silicon: `/opt/homebrew/opt/tcl-tk/bin/tclsh8.6`
- Intel macOS: `/usr/local/opt/tcl-tk/bin/tclsh8.6`

You can override the interpreter path by exporting `TCLSH86=/path/to/tclsh8.6`.

### Tcl 8.6 (Linux)

```sh
sudo apt-get update && sudo apt-get install -y tcl8.6
TCLLIBPATH=. tclsh8.6 tests/all.tcl
```

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
./configure --with-tcl=$(brew --prefix tcl-tk)/lib

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

On any platform, you can run the suite against whatever `tclsh` is in PATH:

```sh
TCLLIBPATH=. tclsh tests/all.tcl
```

Make targets are also available:

```sh
make test85   # runs with detected Tcl 8.5
make test86   # runs with detected Tcl 8.6
make test-matrix
```

CI
- GitHub Actions run the suite on Ubuntu (8.6 and 8.5 from source) and macOS (8.6 via Homebrew). See `.github/workflows/ci.yml`.
- The perf smoke job is separate and opt‑in (constraints `perf`).

## API Summary

- `tdb::start` / `tdb::stop` — enable/disable the engine (idempotent)
- `tdb::config ?-option value ...?` — config keys:
  - `-perf.allowInline` (1|0) — inline compilation flag on object trace
  - `-path.normalize` (1|0) — normalize file paths
  - `-safeEval` (1|0) — safe child interp for `tdb::eval` (default 0; falls back automatically when needed)
- `tdb::break add|rm|clear|ls` — breakpoints:
  - File:Line: `-file /abs/path -line N`
  - Proc: `-proc ::qualified`
  - Method (object command + subcommand): `-method ::globPattern methodName`
  - Options: `-condition {expr}`, `-hitCount ==N|>=N|multiple-of(N)`, `-oneshot 1`, `-log {template}`
- Pause control:
  - `tdb::wait ?-timeout ms?`, `tdb::continue ?-wait?`, `tdb::last-stop`
- Stepping:
  - `tdb::step in|over|out ?-wait?`
  - `tdb::rununtil file:/abs:line ?-wait?`
  - `tdb::rununtil scope-exit ?-wait?`
- Introspection and eval:
  - `tdb::frames`, `tdb::locals ?level?`, `tdb::globals`, `tdb::eval ?level? script`
- Custom constructs:
  - `tdb::register_command_syntax <command> <specDict>` — best‑effort metadata on stop events

See `docs/usage.md` for examples.

### Notes about method breakpoints

- At the moment of object dispatch, method local variables are not yet in scope. To make conditions useful, tdb injects a `$cmd` list into the evaluation frame containing the full command words: `{object method arg1 arg2 ...}`. Example condition for an even argument:

```tcl
tdb::break add -method ::* bark -condition {expr {[lindex $cmd 2] % 2 == 0}}
```

Hit‑counts (`-hitCount`) and logpoints (`-log`) work with method breakpoints. Logpoints print and do not pause; oneshot is honored.

## CLI REPL (Prompt A)

A tiny REPL is provided to drive the debugger interactively:

```sh
./scripts/tdb-repl.tcl
```

Examples:

```
tdb> start
tdb> break file:/abs/path/to/app.tcl:42
tdb> c
tdb> wait 2000
tdb> frames
tdb> locals
tdb> eval -1 {expr {$a+$b}}
tdb> fin
```

## JSON/DAP Shim (Prompt B)

A minimal JSON-over-socket shim is included for editor integration experiments:

```sh
TDB_DAP_PORT=4711 ./scripts/tdb-dap.tcl
```

It accepts newline-delimited JSON objects with a `command` field, e.g.:

```json
{"command":"initialize"}
{"command":"setBreakpoints","file":"/abs/file.tcl","lines":"10,20"}
{"command":"continue"}
{"command":"stackTrace"}
```

Responses are minimal JSON objects. This is not a full DAP implementation, but a small bridge to the core Tcl API.

### DAP endpoints (overview)
The Debug Adapter Protocol (DAP) is used by IDEs to drive debuggers. A full DAP includes endpoints like initialize, launch/attach, setBreakpoints, configurationDone, threads, stackTrace, scopes, variables, continue, next, stepIn, stepOut, pause, evaluate, disconnect, etc. The included shim supports a small subset for experiments (see `scripts/tdb-dap.tcl`).

## Troubleshooting

- Ensure `TCLLIBPATH=.` is set when running tests or `tclsh` directly from the repo.
- macOS: `pkgIndex.tcl` uses an absolute normalized path to satisfy hardened loaders.
- Tcl 8.5: OO may be unavailable; OO tests are skipped automatically.
- Performance: run `-constraints perf` to opt into the perf smoke test.

## Further reading

- See `docs/usage.md` for a practical guide with examples covering breakpoints, stepping, event flow, evaluation, and custom constructs.
