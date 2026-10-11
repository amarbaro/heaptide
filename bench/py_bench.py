# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# --py cost: a large live Python heap built and churned for a few seconds (python3 bench/py_bench.py [rounds]).
import sys
keep = [{"i": i, "s": str(i)} for i in range(1_000_000)]
for r in range(int(sys.argv[1]) if len(sys.argv) > 1 else 20):
    tmp = [str(i) * 3 for i in range(200_000)]
    keep[r] = tmp
