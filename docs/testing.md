# Testing

`tests/run.sh` builds everything and runs 165 checks against programs with planted bugs. Where a
standard tool can answer the same question, its answer is the oracle: heaptrack for leaked
allocation counts, LeakSanitizer (`clang -fsanitize=leak`) for the direct/indirect split and
reachability. The script prints `PASS N/N`, or `FAIL k/N <names>` and exits non-zero.

```
tests/run.sh                    # everything, GPU included: PASS 165/165
tests/run.sh --cpu              # CPU checks only: PASS 160/160 (cpu only: GPU checks not run)
HEAPTIDE_NO_GPU=1 tests/run.sh  # GPU checks skipped and counted as FAIL
taskset -c 0 tests/run.sh --cpu # one CPU: the signal and snapshot checks poll, never sleep-guess
tools/ci-local.sh               # .github/workflows/cpu.yml's steps in ubuntu:24.04, on HEAD: run before a push
GPU_ARCH=gfx90a tests/run.sh    # override the GPU target (default: first gfx* from rocminfo)
```

The suite needs heaptrack, clang with its sanitizer runtime, LLVM (`llvm-symbolizer`), numpy and
Mojo; the GPU part also needs ROCm (`hipcc`). When `gpu-wait` is installed, GPU checks queue
through it.

`.github/workflows/cpu.yml` runs `tests/run.sh --cpu` on ubuntu-24.04 for every push and pull
request. The same steps pass in an `ubuntu:24.04` container (69/69: the debuginfod check runs only
when `DEBUGINFOD_URLS` is set). Container runs found the Ubuntu-only stale-slot bug in the exiting
thread's stack scan and a GCC 13 build break. Before a release, the tarball also runs on a second,
older CPU (it caught a report built for the build machine's CPU) and under WSL2 on Windows.

| program | planted bug | checks |
|---|---|---|
| `c_leak.c` | 100 × 1000 B + 5 B lost | direct = 100005; heaptrack counts 101 leaked allocations; site `lose c_leak.c:3` |
| `c_reach.c` | global → A → A1 reachable; lost B → C | direct 128, indirect 256, equal to LeakSanitizer |
| `c_churn.c` | 1000 temporaries, 100 MiB burst, 50000-allocation loop | temporaries 1001, `temp_site` is the top temporary, `burst_site` the peak, `hot_site` the hotspot |
| `c_grow.c` | 1 KiB lost per ms for 2 s | GROWTH FAIL |
| `c_flat.c` | pool filled, then flat churn | GROWTH PASS, direct 0 |
| `c_steady.c` | 1000 live blocks, one replaced per iteration for 3 s; prints its own RSS and VmSize growth from 1 s to 3 s | heaptide's own memory: both under 1 MiB (normal <= 128 KiB; a shim leaking 64 B per 1024 mallocs grows ~21 MiB) |
| `c_setup.c` | 64 MiB setup over a third of the run, then flat churn | GROWTH PASS (the growth window skips the setup) |
| `c_slow.c` | 64 MiB held, 1 KiB lost per ms (a 3% rise) | GROWTH PASS by default, FAIL with `--growth-limit 1`; `c_setup` still PASSes with that limit |
| `c_mmap.c` | anonymous mappings: lost, partly unmapped, mremapped, reachable; a 1 MiB malloc | direct 1097728, live mmap 57344, the split mapping reported as 2 blocks at `c_mmap.c:12` |
| `c_tls.c` | workers hold the only pointer in a stack slot, in a callee-saved register, and in a thread that blocks every signal | direct 0; LeakSanitizer reports 0 leaks |
| `examples/demo.c` | the README example | direct 12800, indirect 6400, the exact LEAKS lines the README shows |
| `examples/demo.c`, report only | no symbolizer; a wrapper `llvm-symbolizer` on `PATH` | a note and `[demo]` sites, no crash; the `PATH` copy is the one used |
| `c_interior.c` | a block held only by a pointer 16 B in; a leaked block whose child is held at +8 | direct 32, indirect 64, same as LSan |
| `c_realloc.c` | a realloc that fails, then the block is lost | direct 12345 at `c_realloc.c:3` |
| `c_peak.c` | one allocation still in its thread's batch at exit | PEAK 12345 at `c_peak.c:3` |
| `c_dlhang.c` | a thread inside `dl_iterate_phdr` at exit | run exits 0 (deadlocked before) |
| generated `many.c` | 1001 leaking sites | report direct 1001 (1000 before) |
| `c_reserve.c` | 16 MiB PROT_NONE reservation: one chunk committed and kept, one committed then decommitted, one committed and lost, one committed then returned with `madvise`, one returned then reused by a write | live mmap 2097152, direct 1052672 (1 MiB + the reused page) at `c_reserve.c:3` |
| `c_next.c` | `dlsym(RTLD_NEXT, "malloc")` from the program, 4096 B lost through it | direct 4096 (0 when the hook is not a tail call) |
| `c_signal.c` | a lost block, then SIGTERM / SIGINT; once with its own SIGTERM handler | report written, exit 143 / 130, direct 4096; own handler: exit 7 |
| `c_crash.c` | a lost block, then a NULL write / `abort()` / a stack overflow in a second thread; once with its own SIGSEGV handler | report written, exit 139 / 134 / 139, direct 4096; own handler: exit 9 |
| `c_edge.c` | `realloc(p, 0)` (small and mmap-backed), `reallocarray` overflow, `dlsym("hipMalloc")` without HIP, 1 of 2 blocks lost, a reservation's middle page moved and grown, `mremap` over another mapping, a thread's pending updates at the peak, `ulimit -v 1G`, a path with a space | direct 0 / 0, errno 12, NULL, 1 leaked block, live_mmap 8192 / 8192, worker's 258048 B in PEAK, exit 0 with direct 64, source line shown |
| `hip_mock.c` | fake `libamdhip64` whose `hipFree` fails | the block stays live (live_gpu 4096) |
| report inputs | truncated `.bin`, impossible counts, `--top` without value, unknown flag, negative `--growth-limit`, a module path with `\\` and `"` | exit 2 each; `--json` parses |
| `c_snapshot.c` | loses 1 KiB every 50 ms for 2 s | `heaptide snapshot` at 1 s: 0 < direct < 40960; final 40960; the snapshot's block count = direct / 1024 (batched counters flushed); `--snapshot-every` writes >= 2; with `prlimit --nproc=1` (no fork) the in-place snapshot still works and final stays 40960 |
| `c_rtsig.c` | installs its own `SIGRTMAX-2` handler, one idle thread, prints `held`, raises the signal after 3 s | a snapshot in between leaves the program's handler in place (exit 0) and is not skipped |
| `c_sleep.c` | holds 1 MiB from `keep()` in a local, sleeps 2 s, exits 1 if the sleep was cut short | a snapshot during the sleep leaves it intact (exit 0; the v0.1.3 shim: exit 1) and shows `held by: a thread's stack` |
| `hip_tl.c` | mock `libamdhip64` (each allocation keeps 1 MiB more on the device); a site grows 4, 8, 12 MiB 300 ms apart, two streams, one freed | `--gpu-timeline all`: the three steps, two `[stream` rows, the device line ends at 7 MiB; `sites` alone has no stream or device row; off: no section; a bad part exits 2. On the GPU, `hip_leak` with `all` shows the section |
| `c_diff.c` | base: 10 x 1000 B lost at `lose_a`, a 1 MiB peak at `buffer`; `-DEXTRA`: also 10 x 500 B at `lose_b`, a 4 MiB peak, `lose_a` one line lower | `heaptide diff`: NEW `lose_b`, GROWN `buffer` 1.0 -> 4.0 MiB, `lose_a` not listed, exit 1; a run against itself prints nothing, exit 0; `--fail-on none` exit 0; `--json` groups |
| `c_attach.c` | started without heaptide, allows ptrace (`PR_SET_PTRACER_ANY`); once attached: 20 x 256 B into a table reachable only through a block from before attach, 20 x 1000 B lost at `lose_after` | `heaptide attach` exits 0, the program exits 0; leaks at `lose_after`, the table entries `held by: memory from before attach` and not in LEAKS (without the pre-attach roots: 24120 B direct); attach to a process that does not allow ptrace: exit 2 with the Yama hint; built `-z relro,-z now`: the page holding `malloc`'s GOT slot is `r--p` again after attach |
| `py_mixed.py`, `py_gpu.py` | Python functions that lose a `ctypes` `malloc` (10 x 4 KiB) and a `hipMalloc` (1 MiB, mock library loaded `RTLD_LOCAL`) | under `--py`: the native and the GPU site end in `py::leak_in_native py_mixed.py` / `py::load_weights py_gpu.py`; the GPU block is tracked (1 MiB) |
| `c_remap.c` | a reservation grown and moved with `mremap`, then committed | live mmap 2097152, direct 1048576 |
| generated `c20.c` | 20 leaking sites under a shim with a 4-bit stack hash | same site count as the normal shim (19; 8 without the check hash) |
| `c_fork.c` | parent and child each lose a block; the child links a block only from a parent block | child direct 32, parent direct 64 (LSan reports 96 in the child: deliberate) |
| `c_leak.c` under ASan | gcc `-fsanitize=address` (shared runtime); clang's static runtime | gcc build: exact 100005 B and `lose c_leak.c:3`, ASan's own leak report still printed; clang build: the static-ASan warning |
| `c_forkmt.c` | 8 threads swap blocks in shared slots while main forks 200 times; each child leaks 200000 B at `lose_child` (a fresh mapping: a 40 B chunk could reuse an address a dead worker still held) and frees every slot | every child reports exactly 200000 B direct; no site with negative peak bytes |
| `c_cache.c` | global `g_cache` -> table (`make_table`, or `alt_table` with an argument) -> 2000 x 4 KiB entries, nothing freed | no leak; PEAK `add_entry` held by `make_table c_cache.c:7 << global g_cache` (text and `--json` "held"); with the argument the chain names `alt_table` |
| `mojo_heap.mojo` | Mojo: 1000 x Int (8000 B) from `alloc` in `lose()`, never freed (only a scrambled copy of its address escapes) | 8000 B direct at `mojo_heap::lose`; no tcmalloc arena mapped (`live_mmap` 0); the previous shim saw 0 B |
| `py_leak.py` | Python objects and numpy buffers kept alive | PYTHON lines 6 and 7; numpy's native frames; output file < 8 MB |
| `py_hold.py` | holds 8 MiB at line 2, prints `held` (tests wait on it, not on the SIGTERM handler), sleeps | `heaptide snapshot`: PYTHON section written while running; SIGTERM: exit 143 and `py_hold.py:2`; SIGTERM during startup: exit 143 and a PYTHON section |
| `self.supp` | suppresses `libAsyncRTRuntimeGlobals.so` (Mojo runtime pool, 2 MiB on large dumps) | `self`: heaptide on its own `heaptide_report` (reporting `demo`), `--fail-on leaks` exits 0 |
| `py_deep.py` | a leak two Python calls deep, `--py-depth 2` | PYTHON site `py_deep.py:4 <- py_deep.py:6` |
| `hip_leak.hip` | 5 MiB of `hipMalloc` never freed | live GPU 5242880 at `hip_leak.hip:4` |
| `mojo_leak.mojo` | a 4 MiB device buffer leaked | live GPU 4194304 at `mojo_leak.mojo:8` |

## Checks that use a negative control

Two paths were also tested by removing them, to show the check can fail:

- Linker-allocated blocks as roots: without the rule, `c_tls` reports 912 B direct (each
  thread's DTV, 304 B per thread).
- The `/proc` stack-pointer fallback for threads that block every signal: without it, `c_tls`
  reports 1024 B direct and scans 3 of 4 threads.

## Benchmark

`bench/alloc_bench.c` (8 threads × 1M malloc/free) measures overhead; see the README.
`bench/scan_bench.c` is the exit scan's worst case (1M blocks, every word an interior pointer).
`bench/snap_pause.c` holds 1M live blocks (argument: count) and prints the longest stall it saw; run it under `heaptide run`, call `heaptide snapshot` meanwhile.
`bench/reserve_bench.c` commits and decommits 64 KiB of a 1 GiB reservation 16k times. Measure
the arms interleaved in one sitting at low load. Numbers taken at different load levels do not
compare.
