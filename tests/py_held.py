class Box:
    pass
keep = {"a": Box()}
keep["a"].data = [bytearray(1 << 20) for _ in range(4)]  # 4 MiB at line 4, held via keep['a'].data
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
