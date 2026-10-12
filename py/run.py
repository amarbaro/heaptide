# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
"""heaptide Python mode: run a script (or `python -m module`) with heaptide's Python allocator hooks on; the
shim writes still-allocated memory by line at exit, on SIGTERM/SIGHUP, and next to every native snapshot.

Runs inside the target interpreter (that is why it is Python). Started by `heaptide run --py`.
"""
import atexit
import ctypes
import os
import runpy
import signal
import sys

lib = ctypes.PyDLL(None)  # PyDLL keeps the GIL: the hooks are installed with it held


def dump():
    lib.heaptide_py_dump()


def stop(sig, frame):  # write both sections with the heap as it is, then die of the signal
    dump()
    lib.heaptide_final()
    signal.signal(sig, signal.SIG_DFL)
    os.kill(os.getpid(), sig)


if not getattr(sys, "_is_gil_enabled", lambda: True)():
    print("heaptide: --py needs a GIL build of Python (this one is free-threaded): no PYTHON section", file=sys.stderr)
elif lib.heaptide_py_start(os.path.abspath(__file__).encode()):
    print("heaptide: --py could not hook this Python's allocators: no PYTHON section", file=sys.stderr)
atexit.register(dump)
for s in (signal.SIGTERM, signal.SIGHUP):  # the script may still install its own later
    signal.signal(s, stop)
held = lib.heaptide_py_ready()  # a stop signal that came while Python was starting was held for us: take it now
if held:
    os.kill(os.getpid(), held)
sys.argv = sys.argv[1:]
if sys.argv[0] == "-m" and len(sys.argv) > 1:  # python -m pkg.module args: as Python does, the cwd leads sys.path
    module = sys.argv[1]
    sys.argv = [module] + sys.argv[2:]
    sys.path[0] = os.getcwd()
    script_globals = runpy.run_module(module, run_name="__main__", alter_sys=True)  # keep alive: freeing them would hide leaks
else:
    sys.path[0] = os.path.dirname(os.path.abspath(sys.argv[0]))
    script_globals = runpy.run_path(sys.argv[0], run_name="__main__")
