# One stack across Python and native: leak_in_native loses a 4096 B malloc through ctypes, called
# from outer; the native site must name both Python functions.
import ctypes

libc = ctypes.CDLL(None)
libc.malloc.restype = ctypes.c_void_p


def leak_in_native():
    for _ in range(10):  # ten blocks: one stale pointer cannot hide the site
        libc.malloc(4096)


def outer():
    leak_in_native()


outer()
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
