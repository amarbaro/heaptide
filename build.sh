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
"$mojo" build -O3 "$here/report/heaptide_report.mojo" -o "$here/build/heaptide_report" 2>&1 | grep -vE "posix_spawn|Crashpad|spawn_subprocess|Please submit|Stack dump|Program arguments|^ *#|^\s*$" >&2 || true
[[ -x "$here/build/heaptide_report" ]] || { echo "FAIL build: heaptide_report" >&2; exit 1; }
echo "built $here/build/heaptide_report"
