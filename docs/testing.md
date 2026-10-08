# Testing

`tests/run.sh` builds everything and runs 39 checks against programs with planted bugs. Where a
standard tool can answer the same question, its answer is the oracle: heaptrack for leaked
allocation counts, LeakSanitizer (`clang -fsanitize=leak`) for the direct/indirect split and
reachability. The script prints `PASS N/N`, or `FAIL k/N <names>` and exits non-zero.

```
tests/run.sh                    # everything, GPU included: PASS 39/39
tests/run.sh --cpu              # CPU checks only: PASS 35/35 (cpu only: GPU checks not run)
HEAPTIDE_NO_GPU=1 tests/run.sh  # GPU checks skipped and counted as FAIL
GPU_ARCH=gfx90a tests/run.sh    # override the GPU target (default: first gfx* from rocminfo)
```

The suite needs heaptrack, clang with its sanitizer runtime, LLVM (`llvm-symbolizer`), numpy and
Mojo; the GPU part also needs ROCm (`hipcc`). When `gpu-wait` is installed, GPU checks queue
through it.

`.github/workflows/cpu.yml` runs `tests/run.sh --cpu` on ubuntu-24.04 for every push and pull
request. The same steps pass in an `ubuntu:24.04` container (35/35). That container run is also
how the Ubuntu-only stale-slot bug in the exiting thread's stack scan was found.

| program | planted bug | checks |
|---|---|---|
| `c_leak.c` | 100 × 1000 B + 5 B lost | direct = 100005; heaptrack counts 101 leaked allocations; site `lose c_leak.c:3` |
| `c_reach.c` | global → A → A1 reachable; lost B → C | direct 128, indirect 256, equal to LeakSanitizer |
| `c_churn.c` | 1000 temporaries, 100 MiB burst, 50000-allocation loop | temporaries 1001, `temp_site` is the top temporary, `burst_site` the peak, `hot_site` the hotspot |
| `c_grow.c` | 1 KiB lost per ms for 2 s | GROWTH FAIL |
| `c_flat.c` | pool filled, then flat churn | GROWTH PASS, direct 0 |
| `c_setup.c` | 64 MiB setup over a third of the run, then flat churn | GROWTH PASS (the growth window skips the setup) |
| `c_slow.c` | 64 MiB held, 1 KiB lost per ms (a 3% rise) | GROWTH PASS by default, FAIL with `--growth-limit 1`; `c_setup` still PASSes with that limit |
| `c_mmap.c` | anonymous mappings: lost, partly unmapped, mremapped, reachable; a 1 MiB malloc | direct 1097728, live mmap 57344, the split mapping reported as 2 blocks at `c_mmap.c:12` |
| `c_tls.c` | workers hold the only pointer in a stack slot, in a callee-saved register, and in a thread that blocks every signal | direct 0; LeakSanitizer reports 0 leaks |
| `examples/demo.c` | the README example | direct 12800, indirect 6400, the exact LEAKS lines the README shows |
| `examples/demo.c`, report only | no symbolizer; a wrapper `llvm-symbolizer` on `PATH` | a note and `[demo]` sites, no crash; the `PATH` copy is the one used |
| `py_leak.py` | Python objects and numpy buffers kept alive | PYTHON lines 6 and 7; numpy's native frames; output file < 8 MB |
| `hip_leak.hip` | 5 MiB of `hipMalloc` never freed | live GPU 5242880 at `hip_leak.hip:4` |
| `mojo_leak.mojo` | a 4 MiB device buffer leaked | live GPU 4194304 at `mojo_leak.mojo:8` |

## Checks that use a negative control

Two paths were also tested by removing them, to show the check can fail:

- Linker-allocated blocks as roots: without the rule, `c_tls` reports 912 B direct (each
  thread's DTV, 304 B per thread).
- The `/proc` stack-pointer fallback for threads that block every signal: without it, `c_tls`
  reports 1024 B direct and scans 3 of 4 threads.

## Benchmark

`bench/alloc_bench.c` (8 threads × 1M malloc/free) measures overhead; see the README. Measure
the arms interleaved in one sitting at low load. Numbers taken at different load levels do not
compare.
