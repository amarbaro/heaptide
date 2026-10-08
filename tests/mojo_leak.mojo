# Planted: a 4 MiB device buffer whose pointer is taken out of its owner and never freed (leak);
# a 1 MiB buffer dropped normally (no leak). Run with MODULAR_DEVICE_CONTEXT_MEMORY_MANAGER_SIZE=0.
from max.gpu.host import DeviceContext


def main() raises:
    var ctx = DeviceContext()
    var lost = ctx.enqueue_create_buffer[DType.float32](1 << 20).take_ptr()  # 4 MiB, never freed
    var tmp = ctx.enqueue_create_buffer[DType.float32](1 << 18)  # 1 MiB, freed
    _ = tmp^
    ctx.synchronize()
    _ = lost
    print("mojo-gpu planted")
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
