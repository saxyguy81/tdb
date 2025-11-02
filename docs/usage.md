Tdb (Tcl Debugger) — Usage Guide

This guide shows how to start the debugger, set breakpoints, step, and inspect state.

Prerequisites
- Build/install the extension (see README). Make sure `pkgIndex.tcl` is on `auto_path`.

Quick Start
```tcl
package require tdb

# Start engine
tdb::start

# Set a breakpoint at a file and line
tdb::break add -file /abs/path/to/app.tcl -line 42

# Run code from the event loop and wait for pause
after 0 { my::entry }
set ev [tdb::wait -timeout 2000]

# Continue execution
tdb::continue

# Remove all breakpoints and stop
tdb::break clear
tdb::stop
```

Configuration
- `-perf.allowInline` (default 1): enable `TCL_ALLOW_INLINE_COMPILATION` on the global object trace.
- `-path.normalize` (default 1): normalize paths for file:line breakpoints.
- `-safeEval` (default 0): when 1, `tdb::eval` uses a safe child interpreter seeded with a snapshot of locals/args. When 0, it evaluates in-frame; if that fails (e.g., vars out of scope), it falls back to snapshot-eval.

Breakpoints
```tcl
# File:Line
tdb::break add -file /abs/path/foo.tcl -line 100

# Procedure
tdb::break add -proc ::ns::procname

# Object method (command + subcommand style)
tdb::break add -method ::* bark

# Conditions, hit-counts, oneshot, and logpoints
tdb::break add -file $f -line $l -condition {expr {$i % 2 == 0}}
tdb::break add -proc ::foo -hitCount ==3
tdb::break add -file $f -line $l -oneshot 1
tdb::break add -file $f -line $l -log {i=$i}

# Method breakpoint conditions: use $cmd (full command words)
# Example: pause only when first argument to bark is even
tdb::break add -method ::* bark -condition {expr {[lindex $cmd 2] % 2 == 0}}

# List/remove/clear
tdb::break ls
tdb::break rm 3
tdb::break clear
```

Pause/Continue
- `tdb::wait ?-timeout ms?` returns a stop event dict (keys: event, reason, file, line, proc, cmd, level, locals…)
- `tdb::continue ?-wait?` resumes execution; when `-wait`, returns the next stop.
- `tdb::last-stop` returns the last stop event dict.

Stepping
```tcl
# step into/over/out from the current pause
tdb::step in  -wait
tdb::step over -wait
tdb::step out -wait

# Run to cursor (file:line one-shot)
tdb::rununtil file:/abs/path:123 -wait

# Run until scope exit (leave current proc)
tdb::rununtil scope-exit -wait
```

Frames and Eval
```tcl
# Frames from the paused state (top N frames)
set frames [tdb::frames]

# Locals in the paused frame or a specific level
set locals [tdb::locals]
set up1    [tdb::locals #1]

# Eval in-frame; see -safeEval config for safety/isolation
tdb::eval -1 {expr {$a + $b}}

# Safe eval
# Enable sandboxed evaluation using a snapshot of locals/args
tdb::config -safeEval 1
tdb::eval -1 {expr {$a + $b}}
```

Custom Control Constructs
Register command syntax to add best-effort metadata to stop events (useful for DSLs):
```tcl
tdb::register_command_syntax xif {arms {expr script script}}
```

Performance
An opt-in perf smoke test is provided; run with constraints:
```sh
TCLLIBPATH=. tclsh tests/all.tcl -constraints perf
```

Tips
- When debugging object‑method calls, inspect `$cmd` to see the full dispatched command (object, method, and arguments).
- Logpoints (`-log`) do not pause. They print a message and continue.
- `tdb::wait` always takes a timeout; tests use 2s by default to avoid hangs.
- Use `tdb::last-stop` to inspect the most recent stop event without waiting.
