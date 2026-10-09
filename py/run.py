# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
"""heaptide Python mode: run a script under tracemalloc; write still-allocated memory by line at exit,
on SIGTERM/SIGHUP, and next to every native snapshot.

Runs inside the target interpreter (that is why it is Python). Started by `heaptide run --py`.
"""
import atexit
import ctypes
import os
import runpy
import signal
import sys
import threading
import time
import tracemalloc


def dump(me=os.path.abspath(__file__), out=os.environ.get("HEAPTIDE_OUT", ".")):
    skip = [tracemalloc.Filter(False, f) for f in (tracemalloc.__file__, me, "<frozen *>", runpy.__file__)]
    deep = tracemalloc.get_traceback_limit() > 1  # --py-depth: group by call chain, not line
    stats = tracemalloc.take_snapshot().filter_traces(skip).statistics("traceback" if deep else "lineno")
    path = os.path.join(out, f"py.{os.getpid()}.txt")
    with open(path + ".tmp", "w") as f:
        for s in stats[:20]:
            at = " <- ".join(f"{fr.filename}:{fr.lineno}" for fr in reversed(s.traceback))
            f.write(f"  {s.size} B in {s.count} blocks  {at}\n")
    os.replace(path + ".tmp", path)


def stop(sig, frame):  # write both sections with the heap as it is, then die of the signal
    dump()
    try:
        ctypes.CDLL(None).heaptide_final()
    except (OSError, AttributeError):
        pass
    signal.signal(sig, signal.SIG_DFL)
    os.kill(os.getpid(), sig)


def follow(out=os.environ.get("HEAPTIDE_OUT", ".")):  # a native snapshot gets a Python section too
    path, last = os.path.join(out, f"heaptide.{os.getpid()}.bin"), None
    while True:
        time.sleep(0.2)
        try:
            m = os.stat(path).st_mtime_ns
        except OSError:
            continue
        if m != last:
            last = m
            dump()


atexit.register(dump)
for s in (signal.SIGTERM, signal.SIGHUP):  # the script may still install its own later
    signal.signal(s, stop)
try:  # a stop signal that came while Python was starting was held for us: take it now
    held = ctypes.CDLL(None).heaptide_py_ready()
except (OSError, AttributeError):
    held = 0
if held:
    os.kill(os.getpid(), held)
threading.Thread(target=follow, daemon=True).start()
sys.argv = sys.argv[1:]
sys.path[0] = os.path.dirname(os.path.abspath(sys.argv[0]))
script_globals = runpy.run_path(sys.argv[0], run_name="__main__")  # keep alive: freeing them would hide leaks
