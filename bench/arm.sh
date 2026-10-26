#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# bench/arm.sh SHIM.so CMD...: one run of CMD under that shim, prints "wall S s" and "rss KB kB"
# (max RSS). An arm for bench-ab: bench-ab -- bench/arm.sh old.so CMD ::: bench/arm.sh new.so CMD
set -euo pipefail
so=$(readlink -f "$1"); shift
o=$(mktemp -d "${TMPDIR:-/tmp}/heaptide-arm.XXXXXX"); trap 'rm -rf "$o"' EXIT
HEAPTIDE_OUT=$o LD_PRELOAD=$so /usr/bin/time -f "wall %e s
rss %M kB" "$@" 2>&1 >/dev/null | grep -E '^(wall|rss) '
