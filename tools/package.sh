#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# Packs a prebuilt release: dist/heaptide-<version>-linux-x86_64.tar.gz, runnable without Mojo.
# The report links Mojo's runtime libraries; they are copied next to it and found via $ORIGIN.
# Run after build.sh, on the oldest glibc you want to support (CI: ubuntu-24.04).
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); ver=${1:?usage: tools/package.sh VERSION}
name=heaptide-$ver-linux-x86_64; stage=$here/dist/$name
rm -rf "$stage"; mkdir -p "$stage/build/lib"
cp "$here/heaptide" "$here/LICENSE" "$here/NOTICE" "$here/README.md" "$stage/"
cp "$here/build/libheaptide.so" "$here/build/heaptide_report" "$stage/build/"
cp -r "$here/py" "$stage/"  # --py runs py/run.py
ldd "$here/build/heaptide_report" | awk '/=> \// {print $3}' | grep -E '/modular/lib/' | while read -r lib; do
  cp -L "$lib" "$stage/build/lib/"
done
for lib in "$stage"/build/lib/*.so; do patchelf --set-rpath '$ORIGIN' "$lib"; done
patchelf --set-rpath '$ORIGIN/lib' "$stage/build/heaptide_report"
[[ -n $(ls "$stage/build/lib") ]] || { echo "FAIL package: no Mojo runtime libraries found" >&2; exit 1; }
cat >> "$stage/NOTICE" <<'N'

build/lib/ holds Mojo runtime libraries from the mojo package (Modular Inc.), redistributed
unmodified under their license: https://www.modular.com/legal/max-mojo-license
N
ldd "$stage/build/heaptide_report" | grep -q "not found" && { echo "FAIL package: unresolved library" >&2; exit 1; }
tar -C "$here/dist" -czf "$here/dist/$name.tar.gz" "$name"
echo "packed $here/dist/$name.tar.gz"
