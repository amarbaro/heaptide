# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 AMARBARO · amarbaro.org labs
"""heaptide Python mode: run a script (or `python -m module`) with heaptide's Python allocator hooks on; the
shim writes still-allocated memory by line at exit, on SIGTERM/SIGHUP, and next to every native snapshot.
Then names, in Python, the object that holds each top site's memory and the biggest GPU tensors.

Runs inside the target interpreter (that is why it is Python). Started by `heaptide run --py`.
"""
import atexit
import ctypes
import gc
import os
import runpy
import signal
import sys
import types

lib = ctypes.PyDLL(None)  # PyDLL keeps the GIL: the hooks are installed with it held
for f in (lib.heaptide_py_site_of, lib.heaptide_gpu_site_of):
    f.argtypes, f.restype = [ctypes.c_uint64], ctypes.c_long
lib.heaptide_py_name.argtypes, lib.heaptide_py_name.restype = [ctypes.c_uint32], ctypes.c_char_p
# an object's block starts at its GC header (16 B) and, for managed dicts/weakrefs (3.11+), a 16 B pre-header
GC, PRE = 1 << 14, (0x18 if sys.version_info >= (3, 11) else 0)
SELF = __file__  # runpy's __main__ swap leaves this module's globals without it by exit
# objects walked at most, and taken from one container (past the roots): one object per site is enough, so a
# list of a million is not walked through (bench/py_bench: +0.1 s and +20 MB at exit, not +1.8 s and +570 MB)
BUDGET, FAN = 200_000, 256


def site_of(o):
    f = type(o).__flags__
    return lib.heaptide_py_site_of(id(o) - ((32 if f & PRE else 16) if f & GC else 0))


def key(h, c):
    return next((k for k, v in h.items() if v is c), "?")


def label(path, names):  # the target, each holder, the root: "bytearray << Box.data[0] << __main__.keep['a']"
    out, sub = [type(path[0]).__name__], ""
    for j in range(1, len(path) - 1):
        h, c = path[j], path[j - 1]
        name = h.__name__ if isinstance(h, type) else type(h).__name__  # a class by its own name
        if isinstance(h, dict):  # an instance's or a class's __dict__ reads as .attr
            k, o = key(h, c), path[j + 1]
            own = getattr(o, "__dict__", None) is h or (isinstance(o, type) and vars(o).get(k) is c)
            sub = (f".{k}" if own else f"[{k!r}]") + sub
        elif isinstance(h, (list, tuple)):
            sub = f"[{next((i for i, v in enumerate(h) if v is c), '?')}]" + sub
        elif getattr(h, "__dict__", None) is c or (isinstance(h, type) and isinstance(c, dict)):
            out.append(name + sub)
            sub = ""
        else:
            d = getattr(h, "__dict__", None)
            a = key(d, c) if isinstance(d, dict) else next((s for s in getattr(type(h), "__slots__", ()) if getattr(h, s, None) is c), "?")
            out.append(name + (f".{a}" if a != "?" else "") + sub)
            sub = ""
    r, c = path[-1], path[-2]
    if isinstance(r, types.FrameType):
        return " << ".join(out + [f"frame {r.f_code.co_name} local {key(r.f_locals, c)}{sub}"])
    return " << ".join(out + [f"{names[id(r)]}.{key(r, c)}{sub}"])


def holders():  # pyheld.<pid>.txt. One breadth-first walk from the roots (module globals, the script's,
    # every thread's frames) finds each target on the way, so the first path is the shortest: an object of
    # a top Python site (by its block, past the GC header) or a tensor of a top GPU site.
    top, rows = (ctypes.c_uint32 * 20)(), (ctypes.c_uint32 * 5)()
    want, gpu, found = set(top[:lib.heaptide_py_top(top)]), set(rows[:lib.heaptide_gpu_top(rows)]), {}
    tensor = getattr(sys.modules.get("torch"), "Tensor", None)
    names, roots, parent = {}, [], {id(globals()): None}  # this module and its frames are never walked
    for m in list(sys.modules.values()):
        d = getattr(m, "__dict__", None)
        if isinstance(d, dict) and d is not globals():
            names[id(d)] = getattr(m, "__name__", "?")
            roots.append(d)
    if isinstance(globals().get("script_globals"), dict):
        names[id(script_globals)] = "__main__"
        roots.append(script_globals)
    for f in sys._current_frames().values():
        while f:
            if f.f_code.co_filename == SELF:
                parent[id(f)] = None
            else:
                roots.append(f)
            f = f.f_back
    for r in roots:
        parent[id(r)] = None
    frontier, walked = roots, 0
    while frontier and (want or gpu) and walked < BUDGET:
        nxt = []
        for h in frontier:
            for x in gc.get_referents(h)[:None if parent[id(h)] is None else FAN]:
                if id(x) in parent:
                    continue
                parent[id(x)] = h
                walked += 1
                s, row = site_of(x), -1
                if tensor is not None and gpu and isinstance(x, tensor):
                    try:
                        row = lib.heaptide_gpu_site_of(x.untyped_storage().data_ptr())
                    except Exception:
                        pass
                if (s in want and site_of(x) == s) or row in gpu:
                    path = [x]
                    while parent[id(path[-1])] is not None:
                        path.append(parent[id(path[-1])])
                    found["py" if s in want else "gpu", s if s in want else row] = label(path, names)
                    want.discard(s)
                    gpu.discard(row)
                if type(x).__flags__ & GC:
                    nxt.append(x)
            if not (want or gpu) or walked >= BUDGET:
                break
        frontier = nxt
    for s in want:
        found["py", s] = "(no module or frame reaches it: held from C, or past the walk's budget)"
    for row in gpu:
        found["gpu", row] = "(no module or frame reaches it: held from C, or past the walk's budget)"
    out = f"{os.environ.get('HEAPTIDE_OUT', '.')}/pyheld.{os.getpid()}.txt"
    with open(out + ".tmp", "w") as f:
        for (k, s), c in found.items():
            f.write(f"{k}\t{lib.heaptide_py_name(s).decode(errors='replace') if k == 'py' else s}\t{c}\n")
    os.replace(out + ".tmp", out)


def walk():  # also run by a snapshot, on the main thread; no thread switch while it walks
    old = sys.getswitchinterval()
    sys.setswitchinterval(1e6)
    try:
        holders()
    except Exception as e:  # the program's exit status and its report come first
        print(f"heaptide: no Python holder chains: {e!r}", file=sys.stderr)
    finally:
        sys.setswitchinterval(old)


def dump():
    lib.heaptide_py_dump()
    walk()


def stop(sig, frame):  # write both sections with the heap as it is, then die of the signal
    dump()
    lib.heaptide_final()
    signal.signal(sig, signal.SIG_DFL)
    os.kill(os.getpid(), sig)


if not getattr(sys, "_is_gil_enabled", lambda: True)():
    print("heaptide: --py needs a GIL build of Python (this one is free-threaded): no PYTHON section", file=sys.stderr)
elif lib.heaptide_py_start(os.path.abspath(__file__).encode()):
    print("heaptide: --py could not hook this Python's allocators: no PYTHON section", file=sys.stderr)
else:
    lib.heaptide_py_walker(ctypes.py_object(walk))
atexit.register(dump)
for s in (signal.SIGTERM, signal.SIGHUP):  # the script may still install its own later
    signal.signal(s, stop)
held = lib.heaptide_py_ready()  # a stop signal that came while Python was starting was held for us: take it now
if held:
    os.kill(os.getpid(), held)
sys.argv = sys.argv[1:]
try:  # the script's globals are kept alive: freeing them would hide leaks
    if sys.argv[0] == "-m" and len(sys.argv) > 1:  # python -m pkg.module args: as Python does, the cwd leads sys.path
        module = sys.argv[1]
        sys.argv = [module] + sys.argv[2:]
        sys.path[0] = os.getcwd()
        script_globals = runpy.run_module(module, run_name="__main__", alter_sys=True)
    elif sys.argv[0] == "-c" and len(sys.argv) > 1:  # python -c CODE args: argv[0] "-c", the cwd as ""
        code, sys.argv = sys.argv[1], ["-c"] + sys.argv[2:]
        sys.path[0] = ""
        script_globals = {"__name__": "__main__", "__builtins__": __builtins__}
        exec(compile(code, "<string>", "exec"), script_globals)
    else:
        sys.path[0] = os.path.dirname(os.path.abspath(sys.argv[0]))
        script_globals = runpy.run_path(sys.argv[0], run_name="__main__")
except BaseException as e:  # sys.exit() or an error: the script's module frame in the traceback still holds them
    tb = e.__traceback__
    while tb and (tb.tb_frame.f_globals is globals() or tb.tb_frame.f_globals.get("__name__") != "__main__"):
        tb = tb.tb_next
    if tb:
        script_globals = tb.tb_frame.f_globals
    raise
