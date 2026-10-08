# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
"""heaptide Python mode: run a script under tracemalloc; at exit write still-allocated memory by line.

Runs inside the target interpreter (that is why it is Python). Started by `heaptide run --py`.
"""
import atexit
import os
import runpy
import sys
import tracemalloc


def dump(me=os.path.abspath(__file__), out=os.environ.get("HEAPTIDE_OUT", ".")):
    skip = [tracemalloc.Filter(False, f) for f in (tracemalloc.__file__, me, "<frozen *>", runpy.__file__)]
    stats = tracemalloc.take_snapshot().filter_traces(skip).statistics("lineno")
    path = os.path.join(out, f"py.{os.getpid()}.txt")
    with open(path, "w") as f:
        for s in stats[:20]:
            fr = s.traceback[0]
            f.write(f"  {s.size} B in {s.count} blocks  {fr.filename}:{fr.lineno}\n")


atexit.register(dump)
sys.argv = sys.argv[1:]
sys.path[0] = os.path.dirname(os.path.abspath(sys.argv[0]))
script_globals = runpy.run_path(sys.argv[0], run_name="__main__")  # keep alive: freeing them would hide leaks
