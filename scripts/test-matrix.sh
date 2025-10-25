#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_SCRIPT="$ROOT_DIR/tests/all.tcl"

INTERP_85="${TCLSH85:-$HOME/.local/tcl8.5/bin/tclsh8.5}"
INTERP_86="${TCLSH86:-/opt/homebrew/opt/tcl-tk@8/bin/tclsh8.6}"

labels=("Tcl 8.5" "Tcl 8.6")
interpreters=("$INTERP_85" "$INTERP_86")

status=0

run_tests() {
  local label="$1"
  local interp="$2"

  if [ -z "$interp" ]; then
    printf '[SKIP] %-8s (interpreter not specified)\n' "$label"
    return
  fi

  if ! command -v "$interp" >/dev/null 2>&1; then
    printf '[SKIP] %-8s (%s not found)\n' "$label" "$interp"
    return
  fi

  printf '[RUN ] %-8s -> %s\n' "$label" "$interp"
  if "$interp" "$TEST_SCRIPT"; then
    printf '[PASS] %-8s\n' "$label"
  else
    printf '[FAIL] %-8s\n' "$label"
    status=1
  fi
}

for idx in "${!labels[@]}"; do
  run_tests "${labels[$idx]}" "${interpreters[$idx]}"
  echo
done

exit $status
