#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# heaptide against Valgrind (massif, memcheck) on bench/alloc_bench and examples/demo: N runs each,
# wall seconds and peak RSS of the profiled process. heaptide is preloaded directly (the launcher's
# own helpers, nm among them, would be measured instead). Memcheck on a stripped glibc needs its
# debug info: DEBUGINFOD_URLS lets valgrind fetch it once. Usage: bench/vs_valgrind.sh [N]
set -euo pipefail
cd "$(dirname "$0")/.."; n=${1:-3}; o=.work/vs_valgrind; mkdir -p "$o"
command -v valgrind >/dev/null || { echo "FAIL vs_valgrind: valgrind not installed"; exit 1; }
cc -O2 -pthread -o build/alloc_bench bench/alloc_bench.c && cc -O0 -g -o build/demo examples/demo.c
for prog in build/alloc_bench build/demo; do
  for v in bare heaptide massif memcheck; do
    for _ in $(seq "$n"); do
      case $v in
        bare) cmd=("$prog") ;;
        heaptide) cmd=(env LD_PRELOAD="$PWD/build/libheaptide.so" HEAPTIDE_OUT="$PWD/$o/ht" "$prog") ;;
        massif) cmd=(valgrind -q --tool=massif --massif-out-file="$o/massif.out" "$prog") ;;
        memcheck) cmd=(valgrind -q --leak-check=full --log-file="$o/memcheck.log" "$prog") ;;
      esac
      /usr/bin/time -f "$(basename "$prog") $v %e s %M KB" -o "$o/t" "${cmd[@]}" >/dev/null 2>&1 || { echo "FAIL vs_valgrind: $v on $prog (log $o)"; exit 1; }
      cat "$o/t"
    done
  done
done
