import time
kept = [bytearray(1 << 20) for _ in range(8)]  # 8 MiB held at line 2
print("held", flush=True)  # line 2 has run: the test waits on this, not on the SIGTERM handler
time.sleep(60)
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
