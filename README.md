# heaptide

[![cpu](https://github.com/amarbaro/heaptide/actions/workflows/cpu.yml/badge.svg)](https://github.com/amarbaro/heaptide/actions/workflows/cpu.yml)

heaptide finds leaks in CPU, GPU (HIP and Mojo) and Python programs in one pass, without
rebuilding them. On an 8-thread malloc benchmark it runs 12x faster than heaptrack with 6.7x less
added memory. On every planted leak in its test suite, its direct/indirect split matches
LeakSanitizer's. It also sees HIP device and pinned host memory, Mojo's GPU buffers included,
which no other leak tool we know of covers on ROCm.

```
heaptide run [--gpu] [--py] [--py-depth N] [--every MS] [--snapshot-every S] [--out DIR] -- CMD...
heaptide snapshot DIR                                  a running process writes its report now
heaptide report DIR [--json] [--top N] [--supp FILE] [--fail-on any|leaks|growth|none]
                    [--growth-limit MIB]
```

## One report answers five questions

[`examples/demo.c`](examples/demo.c) loses one 128 B node, and the 64 B child it points to, on
every 10th of 1000 requests:

```
$ cc -O0 -g -o demo examples/demo.c
$ ./heaptide run --every 50 -- ./demo
heaptide: pid=1377460 direct=12800 indirect=6400 live_cpu=19328 ... threads=1/1
$ ./heaptide report .work/heaptide --top 3
== heaptide pid 1377460  exe .../demo  stacks 3  samples 42 every 50 ms
LEAKS  direct 12.5 KiB  indirect 6.25 KiB
  12.5 KiB (direct 12800, indirect 0, 100 blocks) cpu  handle_request demo.c:9:13
  6.25 KiB (direct 0, indirect 6400, 100 blocks) cpu  handle_request demo.c:10:13
GROWTH  PASS  slope 9.1171 KiB /s  over window 15.063 KiB
PEAK (live at peak)
   12800   cpu    handle_request demo.c:9:13
   6400   cpu    handle_request demo.c:10:13
   128   cpu    main demo.c:15:11
HOTSPOTS (allocations)
   1000   cpu    handle_request demo.c:9:13
   1000   cpu    handle_request demo.c:10:13
   1   cpu    main demo.c:15:11
TEMPORARIES (alloc then free)
   900   cpu    handle_request demo.c:10:13
```

- **LEAKS**: what was never freed, split the way LeakSanitizer splits it. The node held by the
  global `cache` is reachable and not listed. Each lost child is *indirect*: only a lost node
  points to it.
- **GROWTH**: whether live memory keeps rising after setup. It passes here because 19 KiB over
  two seconds is under the 64 KiB threshold.
- **PEAK**: which sites held memory at the moment of the peak.
- **HOTSPOTS**: which sites allocate most often.
- **TEMPORARIES**: which allocations are freed straight after, with nothing in between.

A fifth section, **PYTHON**, appears with `--py`: still-allocated memory by Python source line.
The report exits 1 because there are leaks (`--fail-on` picks what counts).

## Install

The release tarball needs no Mojo: Linux x86-64, glibc 2.34 or newer, and `libunwind`
(`libunwind8` on Debian/Ubuntu, `libunwind` on Arch/Fedora).

```
curl -LO https://github.com/amarbaro/heaptide/releases/download/v0.1.1/heaptide-v0.1.1-linux-x86_64.tar.gz
tar xzf heaptide-v0.1.1-linux-x86_64.tar.gz && cd heaptide-v0.1.1-linux-x86_64 && ./heaptide run -- CMD...
```

## Build from source

You need Linux x86-64, a C compiler, `libunwind` and Mojo 1.1 for the report (on `PATH`, in
`./.venv`, or `MOJO=`). For file:line the report needs `llvm-symbolizer`: it uses
`$HEAPTIDE_SYMBOLIZER`, then the one on `PATH`, then ROCm's. Without any, sites show module names
only.

```
./build.sh          # -> build/libheaptide.so, build/heaptide_report (report built for x86-64-v2, not the build CPU)
```

GPU targets need ROCm/HIP. Python mode uses the standard library's `tracemalloc`.

## GPU and Python need one flag each

- `--gpu` sets `MODULAR_DEVICE_CONTEXT_MEMORY_MANAGER_SIZE=0` and
  `MODULAR_DEVICE_CONTEXT_HOST_MEMORY_MANAGER_SIZE=0`. Mojo normally sub-allocates from a
  ~490 MiB device pool and a pinned host pool it keeps until exit. Without the flag heaptide sees
  one big block, and the host pool shows up as a leak. Leak findings are the same either way;
  PEAK and peak memory under `--gpu` are those of the unpooled allocator, not of production.
- `--py CMD` takes a python command and runs the script under `tracemalloc` with 1 frame;
  `--py-depth N` keeps N frames and shows each site as its call chain (on `tests/py_leak.py`:
  1.14 s at 1 frame, 1.32 s at 2, 2.24 s at 8). Native
  stacks are capped at 8 frames (`HEAPTIDE_DEPTH`, default 64), because most Python-mode stacks
  are tracemalloc's own and deeper frames only split them.
- `--every MS` sets the GROWTH sampling period (default 100).
- Long-running processes: SIGTERM, SIGINT and SIGHUP write the report before the process dies of
  the signal (exit status unchanged), unless the program installed its own handler, which is left
  alone (with `--py`, the PYTHON section is written too). `heaptide snapshot DIR` makes a running
  process write its report now, and
  `--snapshot-every S` does it on a timer; `heaptide report DIR` then shows the latest one. A
  snapshot pauses the program only to fork it (10M live blocks: 15 ms) and scans the copy, which
  can cost up to the heap's size in memory while the program rewrites its pages; it is skipped if
  a thread cannot be parked.
- A crash (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT) writes the report, then the process dies of
  the signal; best effort, since the heap may be broken (it gives up after 10 s).
- `report --growth-limit MIB` fails GROWTH on a projected rise above MIB, whatever the mean.
- `report --supp FILE` drops leaks whose site contains any line of FILE (built in: glibc's
  `_dlerror_run` buffer, which libc releases at exit behind malloc's back). `--json` prints one
  object per process. A damaged `.bin`, a missing option value or an unknown option is exit 2.
- Exit code: 0 clean, 1 leak or growth (per `--fail-on`), 2 tool error.

Child processes are traced too. A helper with a peak under 1 MiB, leaks under 64 KiB and no
growth folds into one QUIET line. The main process never folds.

GROWTH fits live bytes after the first 20% of the run and projects the slope over that span. It
fails when the projected rise is over 64 KiB and over 5% of the mean, or over `--growth-limit MIB`
when you set one. If the first window fails, its start moves later, up to half the run, and the
report says `window from X s`; every window is judged by its slope over the same span, so only a
slope that really flattened passes. A run that is mostly loading still fails: run more iterations.

The 5% rule alone is lenient on big processes. A 20-request run of a 9B-parameter Mojo inference
engine (12 GB live) passes with a 527 MiB projected rise, which is 4% of the mean. With
`--growth-limit 16` the window moves to 2.0 s and the rise there is 0: setup ends at 2 s and the
engine is flat after that. For large processes, set a limit.

Build targets with `-g` (Mojo: `-g1`, since full `-g` can fail on large parameterized kernels)
to get file:line in the report.

## Aggregating in process is what makes it fast

```
heaptide run ── LD_PRELOAD ──► libheaptide.so (C, one file, inside the target)
                                │ hooks: malloc family · anonymous mmap/munmap/mremap
                                │        hipMalloc/HostMalloc/frees · dlsym (Mojo's HIP path)
                                │ per allocation: unwind → stack id → 16 B live-table entry
                                │ sampler thread: live bytes every --every ms
                                ▼ at exit
                       reachability scan (LeakSanitizer design)
                         roots: globals · other threads' stacks + registers · linker blocks
                         → each unfreed block is direct, indirect or reachable
                                ▼
                       heaptide.<pid>.bin + .maps
                                ▼
heaptide report (Mojo) ── one llvm-symbolizer batch ──► LEAKS · GROWTH · PEAK · HOTSPOTS
                                                        · TEMPORARIES · PYTHON
```

heaptrack streams every event to a second process; heaptide keeps one counter row per call stack
and one 16 B entry per live block, and writes a single file at exit. Threads never share a hot
counter or lock: live totals are per thread, each thread batches its updates to a call site's row,
and each thread remembers its last 64 call stacks' ids, so a known site skips the shared table.
`bench/alloc_bench.c` (8 threads × 1M malloc/free, 16..4096 B), `/usr/bin/time -f "%e s %M KB"`,
Ryzen 7 7800X3D, arms interleaved over 8 rounds:

| arm | wall | peak RSS | RSS over bare |
|---|---|---|---|
| bare | 0.02 s | 3.2 MB | |
| heaptrack 1.5.0 `--record-only` | 1.60-1.82 s (median 1.65) | 65.3 MB | +62.1 MB |
| heaptide | 0.12-0.14 s (median 0.13) | 12.5 MB | +9.3 MB |

That is still about 6x slower than no tool on an allocation-bound loop (load average 3 during the
runs). Python mode costs more: 1.1 s
for a script that runs in 0.06 s bare, nearly all of it in `tracemalloc`.

Internals: [docs/how-it-works.md](docs/how-it-works.md). File format:
[docs/file-format.md](docs/file-format.md).

## Every claim above has a check

`tests/run.sh` runs 112 checks against planted bugs, with heaptrack and LeakSanitizer as oracles:
exact leak bytes, file:line, the direct/indirect split, mmap regions, blocks held only by a
thread's stack or register, interior pointers, fork, reserve-then-commit mappings, growth after a
long setup, Python lines, and HIP and Mojo GPU leaks. `tests/run.sh --cpu` runs the 108 that need
no GPU; CI runs those on Ubuntu 24.04 for every push.
[docs/testing.md](docs/testing.md) lists them.

## For coding agents

[`skills/heaptide/SKILL.md`](skills/heaptide/SKILL.md) teaches an agent the loop: reproduce small,
run, read `report --json`, fix the top direct leak, re-run to prove it. It also tells the agent
which findings are runtime residue to suppress. Copy or symlink the folder into the agent's skills
directory (for Claude Code: `~/.claude/skills/heaptide` or a project's `.claude/skills/heaptide`).

## Where it is wrong

- A pointer into a block's middle keeps it alive, as in LeakSanitizer, so a stray word that
  happens to fall inside a leaked block hides it. Runtime residue left: 216 B direct and 48 B
  indirect from the HIP runtime, 1.2 KiB indirect from Mojo's. The ~170 KiB CPython used to show
  at exit was reached through interior pointers (an object pointer sits past its GC header) and
  is gone.
- GPU and pinned host blocks are never scanned for pointers, because the HIP runtime's own
  tables point at all of them. Any GPU or host block unfreed at exit counts as a direct leak.
- A `PROT_NONE` reservation counts only the parts `mprotect` commits, each as a block at the
  `mprotect` call; `mprotect(PROT_NONE)` decommits. After `madvise(MADV_DONTNEED|MADV_FREE)`
  only the pages resident at report time count. A reservation moved with `mremap` is followed.
- SIGKILL (and the OOM killer) writes no report: use `--snapshot-every` on anything that may die
  that way. A stack overflow writes a report only in the main thread or a thread that has
  allocated (each gets a 64 KiB signal stack, mapped but untouched until a crash).
- A forked child treats the parent's blocks as reachable roots, so it reports only its own leaks
  (LeakSanitizer reports the parent's again in the child). Stack ids are a 64-bit hash plus a
  32-bit check: two sites merge only if all 96 bits collide.
- Stripped system libraries show as `[module]` frames unless their debug file is in elfutils'
  cache or `DEBUGINFOD_URLS` is set (then fetched once by build-id; libpython is 33 MB).
- Linux x86-64 only. GPU paths tested on one machine (Arch Linux, kernel 7.2, glibc 2.44,
  ROCm 7.2, RX 7900 XTX); CPU paths also on Ubuntu 24.04 (glibc 2.39), an older Intel laptop
  (Skylake, Arch) and Ubuntu 24.04 under WSL2 on Windows 11. Other GPUs untried.
- Windows: no native support (no `LD_PRELOAD`; the report is Mojo, which runs on Linux and
  macOS only). Linux programs under WSL2 work with the Linux release; GPU under WSL is untried.

## License

Apache-2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE). Copyright 2026 AMARBARO · amarbaro.org
labs.
