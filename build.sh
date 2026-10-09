#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# Builds build/libheaptide.so (C shim) and build/heaptide_report (Mojo).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); mkdir -p "$here/build"
cc -O2 -g -Wall -Wextra -fPIC -shared -fvisibility=hidden -o "$here/build/libheaptide.so" "$here/shim/heaptide.c" -lunwind -ldl -lpthread \
  || { echo "FAIL build: libheaptide.so" >&2; exit 1; }
echo "built $here/build/libheaptide.so"
mojo=${MOJO:-$(command -v mojo || ls "$here/.venv/bin/mojo" 2>/dev/null || true)}
[[ -n $mojo ]] || { echo "FAIL build: no mojo toolchain (put mojo on PATH, in ./.venv, or set MOJO=)" >&2; exit 1; }
# into a temp file: a failed build must not leave the old binary looking fresh. Mojo's crashpad
# trace is noise, the compiler's exit status is not.
tmp=$here/build/.heaptide_report.new; rm -f "$tmp"; rc=0
log=$("$mojo" build -O3 --target-cpu x86-64-v2 "$here/report/heaptide_report.mojo" -o "$tmp" 2>&1) || rc=$?
grep -vE "posix_spawn|Crashpad|spawn_subprocess|Please submit|Stack dump|Program arguments|^ *#|^[0-9]+ +[^ ]+ +0x|^\s*$" <<<"$log" >&2 || true
(( rc == 0 )) && [[ -x $tmp ]] || { echo "FAIL build: heaptide_report (mojo exit $rc)" >&2; exit 1; }
mv "$tmp" "$here/build/heaptide_report"
echo "built $here/build/heaptide_report"
