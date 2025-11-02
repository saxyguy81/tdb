#!/usr/bin/env bash
set -euo pipefail

# Runs the test suite on Tcl 8.5 and 8.6 if available.
# Honors TCLSH85/TCLSH86 env override; otherwise uses scripts/find-tclsh.sh.

root="$(cd "$(dirname "$0")/.." && pwd)"
find_tcl="${root}/scripts/find-tclsh.sh"
sh() { bash "$@"; }

run_one() {
  local exe="$1"; local label="$2"
  echo "== Running on ${label}: ${exe}" >&2
  (cd "$root" && TCLLIBPATH=. "$exe" tests/all.tcl)
}

ok_any=0

# 8.5
if [[ -n "${TCLSH85:-}" ]]; then
  run_one "$TCLSH85" "Tcl 8.5" || true
  ok_any=1
else
  if sh "$find_tcl" 8.5 >/dev/null 2>&1; then
    exe="$(sh "$find_tcl" 8.5)"
    run_one "$exe" "Tcl 8.5" || true
    ok_any=1
  else
    echo "(skip) Tcl 8.5 not found" >&2
  fi
fi

# 8.6
if [[ -n "${TCLSH86:-}" ]]; then
  run_one "$TCLSH86" "Tcl 8.6" || true
  ok_any=1
else
  if sh "$find_tcl" 8.6 >/dev/null 2>&1; then
    exe="$(sh "$find_tcl" 8.6)"
    run_one "$exe" "Tcl 8.6" || true
    ok_any=1
  else
    echo "(skip) Tcl 8.6 not found" >&2
  fi
fi

if [[ "$ok_any" -eq 0 ]]; then
  echo "No Tcl interpreters found. Set TCLSH85/TCLSH86 or install Tcl (see README)." >&2
  exit 1
fi
