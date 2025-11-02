#!/usr/bin/env bash
set -euo pipefail

# find-tclsh.sh <8.5|8.6>
# Emits an absolute path to a tclsh of the requested major.minor, if found.
# Returns non-zero if not found.

want="${1:-}"
if [[ -z "${want}" ]]; then
  echo "usage: $0 <8.5|8.6>" >&2
  exit 2
fi

found=""

# Helper: verify version matches
is_version() {
  local exe="$1"; local want="$2"
  [[ -x "$exe" ]] || return 1
  local v
  v="$("$exe" <<<'puts [info patchlevel]' 2>/dev/null | tr -d '\r' || true)"
  [[ "$v" == $want* ]]
}

# 1) PATH lookups (tclsh8.X)
if command -v "tclsh${want}" >/dev/null 2>&1; then
  cand="$(command -v "tclsh${want}")"
  if is_version "$cand" "$want"; then found="$cand"; fi
fi

# 2) Homebrew common prefixes (prefer tcl-tk@8 for 8.6)
if [[ -z "$found" ]]; then
  for pfx in /opt/homebrew /usr/local; do
    # keg-only @8 formula
    cand="$pfx/opt/tcl-tk@8/bin/tclsh${want}"
    if is_version "$cand" "$want"; then found="$cand"; break; fi
    # generic formula (may be 9.x)
    cand="$pfx/opt/tcl-tk/bin/tclsh${want}"
    if is_version "$cand" "$want"; then found="$cand"; break; fi
  done
fi

# 3) Homebrew formula prefix
if [[ -z "$found" ]] && command -v brew >/dev/null 2>&1; then
  if pfx="$(brew --prefix tcl-tk@8 2>/dev/null)"; then
    cand="$pfx/bin/tclsh${want}"
    if is_version "$cand" "$want"; then found="$cand"; fi
  fi
  if [[ -z "$found" ]] && pfx="$(brew --prefix tcl-tk 2>/dev/null)"; then
    cand="$pfx/bin/tclsh${want}"
    if is_version "$cand" "$want"; then found="$cand"; fi
  fi
fi

# 4) Fallback: plain tclsh
if [[ -z "$found" ]] && command -v tclsh >/dev/null 2>&1; then
  cand="$(command -v tclsh)"
  if is_version "$cand" "$want"; then found="$cand"; fi
fi

if [[ -z "$found" ]]; then
  echo "tclsh ${want} not found" >&2
  exit 1
fi

echo "$found"
