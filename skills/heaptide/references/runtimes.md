# Runtimes and modes

## Python (`--py`)

- The command must start with `python...`; GIL builds only; about memray's speed.
- Interpreter options before the script, `-m` or `-c` (`python3 -u -X dev -c ...`) stay the interpreter's.
- Every Python allocation gets its line, 1 frame deep; `--py-depth N` (max 8) shows call chains,
  for one helper called from many places.
- Python 3.12+: native and GPU sites end in the Python function (`<- py::load_weights train.py`).
  The report must run on the same machine (it reads `/tmp/perf-<pid>.map`).
- `py_held` names the Python object that holds each top site and the biggest GPU tensors.
- Memory: about 37 B per live Python block (30M blocks: 2.48 GB bare, 3.60 GB traced). The table
  is capped at 1/8 of free memory at start (`HEAPTIDE_PY_MEM` MiB); past it, PYTHON starts with
  `WARN untracked`. Still run big jobs under a memory cap (`ht.sh run` does).
- Native stacks are capped at 8 frames under `--py` (`HEAPTIDE_DEPTH`): deep C++ (torch's ATen
  dispatch) can use them all before the Python frame.

## GPU (HIP, Mojo)

- `--gpu` always for Mojo GPU programs; it also turns PyTorch's caching allocator off, so each
  tensor is its own block.
- `--gpu-timeline all` (or `sites,streams,frag`): which site filled VRAM when, per HIP stream, and
  what the device holds beyond live blocks.
- Device and pinned blocks are never treated as reachable: unfreed means leaked.

## Mojo

- Build with `-g1` (full `-g` can fail on large kernels).
- heaptide switches Mojo's allocator to libc at start so its heap is visible; not when attached.
- The report itself is built with Mojo 1.2 or newer; 1.2.0 needs the source as shipped (it
  avoids a `for ... else` that 1.2.0 miscompiles at -O3).

## Attach, snapshot, detach

```
sudo heaptide attach PID --out .work/ht      # gdb loads heaptide; sudo under Yama ptrace_scope 1
heaptide snapshot .work/ht [--label NAME]    # every traced process writes its report now
heaptide detach .work/ht                     # last snapshot, then the process runs on untracked
```

- Sees only what is allocated after attach: can under-report, never over-report. The report says by
  how much at most: `before attach: N in use in the glibc heap` (JSON `before_attach_bytes`).
- GROWTH reads `attached`: compare two snapshots with `heaptide diff` instead.
- A snapshot pauses the program for milliseconds and never interrupts a thread asleep in a
  syscall; a block held only in such a thread's registers can show as leaked (never at exit).
- Chromium-based browsers: only shared libraries' allocations are seen. Never attach to one in use.

## Children, signals, crashes

- Children are traced, one report section each, named by command; `--no-children` or
  `--only NAME[,NAME]` narrow it. A forked child keeps the parent's tables until exec.
- SIGTERM / Ctrl-C and crashes write the full report. SIGKILL, the OOM killer and `_exit()` leave
  the tables in `/dev/shm`; the wrapper (or `heaptide report DIR`) writes a report from them:
  `ENDED WITHOUT A REPORT`, JSON `ended_without_report`, no leak scan. `--snapshot-every S` still
  gives full snapshots before the end.

## Builds and limits

- ASan works only with a shared ASan runtime (gcc's default, clang `-shared-libsan`).
- No `llvm-symbolizer`: sites show modules only (install LLVM or set `HEAPTIDE_SYMBOLIZER`).
- Overhead about 6x on an allocation-bound loop, near zero on code that mostly waits.
- Linux x86-64, glibc. Checked: glibc 2.31 (shim run only) and current Arch (full suite).
