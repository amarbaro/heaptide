# Planted: 1000 x Int (8000 B) allocated in lose() and never freed; a 1024 x Int List that is
# (CPU only: heaptide routes Mojo's allocator to libc, so its blocks are seen at all).
from std.memory import alloc


@no_inline
def lose() -> Int:
    var p = alloc[Int](1000)  # 8000 B, never freed; only a scrambled copy of its address escapes
    p.store(0, 7)
    return Int(p) ^ 0x5A5A5A5A


def main():
    var tag = lose()
    var tmp = List[Int](length=1024, fill=tag)
    print("mojo-heap planted", tmp[1023] != 0)
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
