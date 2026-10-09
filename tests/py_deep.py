# Planted: line 4 allocates, called from line 6; --py-depth 2 shows the chain, the default does not.
kept = []
def inner():
    return bytearray(1 << 20)
def outer():
    kept.append(inner())
for _ in range(8): outer()
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
