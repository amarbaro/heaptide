# One stack across Python and GPU: load_weights calls hipMalloc (the mock libamdhip64 built by the
# suite) through ctypes and never frees; the GPU site must name the Python function.
import ctypes
import sys

hip = ctypes.CDLL(sys.argv[1])


def load_weights():
    p = ctypes.c_void_p()
    hip.hipMalloc(ctypes.byref(p), 1 << 20)


load_weights()
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
