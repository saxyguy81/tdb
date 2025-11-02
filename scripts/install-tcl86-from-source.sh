#!/usr/bin/env bash
set -euo pipefail

# Build and install Tcl 8.6 from source into a local prefix under ./.local/tcl8.6
# Dependencies: build tools (clang/gcc, make), curl or wget, tar.

ver="8.6.13"
url="https://downloads.tcl-lang.org/releases/${ver}/tcl${ver}-src.tar.gz"
prefix="$(pwd)/.local/tcl8.6"
builddir="$(pwd)/.build-tcl86"

mkdir -p "$builddir"
cd "$builddir"

echo "Fetching Tcl ${ver} from ${url}..."
if command -v curl >/dev/null 2>&1; then
  curl -L "$url" -o tcl.tar.gz
elif command -v wget >/dev/null 2>&1; then
  wget -O tcl.tar.gz "$url"
else
  echo "Need curl or wget to download sources." >&2
  exit 2
fi

tar -xzf tcl.tar.gz
cd tcl${ver}/unix

echo "Configuring..."
./configure --prefix="$prefix"
echo "Building..."
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc || echo 2)
echo "Installing to $prefix ..."
make install

echo "Done. Add to PATH:"
echo "  export PATH=\"$prefix/bin:\$PATH\""
echo "Then run:"
echo "  TCLLIBPATH=. $prefix/bin/tclsh8.6 tests/all.tcl"

