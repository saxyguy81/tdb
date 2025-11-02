#!/usr/bin/env bash
set -euo pipefail

# Installs Tcl/Tk 8.6 via Homebrew on macOS and prints the discovered tclsh8.6.
# Usage: scripts/install-tcl86-macos.sh

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This helper targets macOS; on Linux use your package manager or source build." >&2
  exit 2
fi

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew not found. Install from https://brew.sh/ then re-run." >&2
  exit 2
fi

echo "Installing tcl-tk@8 (Tcl/Tk 8.6, may take a few minutes)..."
brew install tcl-tk@8 || true

# Brew installs outside the default PATH; suggest adding to PATH
pfx="$(brew --prefix tcl-tk@8)"
echo "Installed Tcl/Tk 8.6 under: $pfx"
echo "tclsh8.6: $pfx/bin/tclsh8.6"
if [[ ":$PATH:" != *":$pfx/bin:"* ]]; then
  echo "Note: Add to PATH for convenience:"
  echo "  export PATH=\"$pfx/bin:\$PATH\""
fi

echo "Verifying version..."
"$pfx/bin/tclsh8.6" <<<'puts [info patchlevel]'
