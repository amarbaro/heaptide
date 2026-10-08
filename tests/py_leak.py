# Planted: line 6 keeps 1000 x 10 KiB bytearrays alive (Python objects);
# line 7 keeps 50 numpy arrays of 80 KiB alive (native buffers, seen by the C shim).
import numpy as np
kept, arrs = [], []
for i in range(1000):
    kept.append(bytearray(10240))
    if i % 20 == 0: arrs.append(np.zeros(10240))
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
