# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
# A script that ends with sys.exit(3) while a global still holds 5 MB: the PYTHON section shows it.
import sys

keep = [bytearray(100000) for _ in range(50)]
sys.exit(3)
