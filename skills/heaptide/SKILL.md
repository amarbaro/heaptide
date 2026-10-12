---
name: heaptide
description: Find and fix memory leaks and memory growth in CPU, GPU (HIP, Mojo) and Python programs on Linux with heaptide, a preload profiler that needs no rebuild. Use when a process's memory grows, when a run must be proven leak-free, when VRAM is not returned, when asked who holds memory, when a running service must be profiled without a restart, or when a change must be gated against its base in CI. Covers running and attaching, reading the JSON report (leaks, holders, GPU timeline), diffing two runs, telling real leaks from runtime residue, and proving a fix.
---

# heaptide

heaptide is preloaded into an unmodified program. At exit it writes one file per process; `heaptide
report` turns that into LEAKS, GROWTH, PEAK, HOTSPOTS, TEMPORARIES (and PYTHON with `--py`). Read the
JSON, not the text: it is the stable interface.

## The loop

1. **Reproduce small.** The shortest run that still shows the symptom. GROWTH needs repetition:
   loop the suspect operation (requests, iterations, batches) many times, so setup is under half
   the run.
2. **Run.**
   ```
   heaptide run [--gpu] [--py] [--py-depth N] [--every MS] [--gpu-timeline all] --out .work/ht -- <cmd> <args>
   heaptide report .work/ht --json > .work/ht/report.json
   heaptide flame .work/ht --open      # for a person: every call path as an offline flame graph
   ```
   - Already running and must not restart: `sudo heaptide attach PID --out .work/ht` (gdb loads
     heaptide into it), then `heaptide snapshot .work/ht`. It sees only what is allocated after
     attach and can under-report (never over-report); Mojo's heap stays invisible when attached.
     Done: `heaptide detach .work/ht` (last snapshot, then untracked; no restart, no sudo).
     Chromium-based browsers: only shared libraries' allocations are seen (PartitionAlloc is
     called directly); a pre-v0.1.5 attach could hang them. Never attach to a browser in use.
   - `--gpu-timeline all` (or `sites,streams,frag`) when the question is which GPU site filled VRAM
     when, per HIP stream, or how much the device holds beyond live blocks.
   - Build C/C++ with `-g` and Mojo with `-g1` (full `-g` can fail on large kernels), or sites
     stop at the executable's symbol.
   - `--gpu` for any Mojo GPU program. Without it Mojo pools its buffers and you see one block.
   - `--py` for a Python entry point (the command must be `python...`; GIL builds only, about
     memray's speed). On Python 3.12+ native and
     GPU sites end in the Python function that made them (`<- py::load_weights train.py`); the
     report must run on the same machine (it reads `/tmp/perf-<pid>.map`). Add `--py-depth 4` when
     one helper is called from many places: PYTHON then shows each call chain, not one line.
   - Short runs need a faster sampler or GROWTH says `too short`: `--every 20` under a few
     seconds, `--every 1` under one second.
   - Servers and long jobs: stop them with SIGTERM or Ctrl-C (the report is still written), or
     leave them running and call `heaptide snapshot .work/ht`, then `heaptide report`. Two
     snapshots under the same load show what grew between them (a snapshot pauses the program
     for milliseconds). A crash writes the report too; SIGKILL and the OOM killer write
     nothing: add `--snapshot-every 60`.
3. **Read** `report.json`: one object per process. Children are traced too; the target is
   usually the object with the most leak sites.
   ```
   {"pid", "direct", "indirect", "growth": "PASS|FAIL|too short|attached", "growth_bytes_per_s",
    "growth_window_bytes", "window_from_s",
    "leaks": [{"bytes", "direct", "indirect", "blocks", "kind": "cpu|gpu|host|mmap", "site"}],
    "held": [{"site", "held_by"}],            // who holds a PEAK site now: "make_table x.c:7 << global g_cache"
    "gpu_timeline": [{"site", "points": [[t_ms, bytes]]}]}   // with --gpu-timeline
   ```
4. **Fix the top direct leak first.** `site` is `function file:line`, the user's own frame
   included. Indirect leaks (only reachable from another leak) usually disappear with their owner;
   fix direct ones and re-run before touching indirect ones.
5. **Prove it.** Re-run the same command and show the site's `direct` went to 0 (or GROWTH flipped
   to PASS) next to the before number. A fix without the second run is not a fix.

Exit codes: `report` returns 0 clean, 1 leak or growth (per `--fail-on any|leaks|growth|none`),
2 tool error. In CI use `--fail-on leaks` or `--fail-on growth` to gate on what you care about.

## Deciding what is real

| report says | it means | do |
|---|---|---|
| `direct` at a site in your code | unfreed and nothing points to it | fix: free it, or give it an owner |
| `indirect` only | held only by another leaked block | fix that block's owner |
| `kind: gpu` or `host` unfreed | device or pinned buffer never freed | always real: GPU blocks are never treated as reachable |
| `kind: mmap` | an anonymous mapping never unmapped, or a part of a `PROT_NONE` reservation that `mprotect` committed (site = the `mprotect` call) | real unless it is an arena the runtime keeps by design |
| a few hundred B in `hsa_amd_signal_create`, `amd::roc::Device`, `_dl_*` | HIP/Mojo runtime residue | suppress, do not "fix" |
| GROWTH FAIL, few direct leaks | memory held, not lost: caches, queues, pools that keep growing | read `held` (`held by:` under PEAK): it names the holder chain, from the site to a global, a thread's stack, or memory from before attach. Holders are recorded for memory live at the report: for a service, `heaptide snapshot` while it grows |
| GROWTH `too short` | fewer than 5 samples | run longer or lower `--every` |
| GROWTH `n/a` (JSON `attached`) | attached: live bytes count from 0 at attach, so their ramp is not growth | `heaptide snapshot` twice while it runs, then `heaptide diff A B` |

Suppress with `--supp FILE` (one substring of a site per line). Keep the file next to the test that
uses it and say in a comment why each line is residue.

## GROWTH, read it right

- The window starts after the first 20% of samples. If that fails, it moves later (up to half the
  run) to skip a long setup; `window_from_s` tells you where it landed. The rise is the slope
  projected over the full span, so a short late window cannot hide a steady leak.
- It fails on a rise over 64 KiB **and** over 5% of mean live memory. On a big process (GBs)
  that is lenient: always add `--growth-limit MIB` (e.g. 16) and report `growth_window_bytes`.
- A run that is mostly loading fails because loading looks like growth. Lengthen the steady
  part; do not raise the limit to make it pass.

## Gate a change in CI

```
heaptide run --out .work/base -- <cmd>    # on the base build
heaptide run --out .work/new  -- <cmd>    # on the change
heaptide diff .work/base .work/new [--fail-on new|grown|none] [--json]
```
NEW (a leak site only the change has: exit 1 by default), GROWN (direct or peak up by more than
64 KiB and 10%: exit 1 with `--fail-on grown`), GONE, SHRUNK. Sites are matched without line
numbers, so moved code is not a change. Only the sites a report shows are compared (every leak,
the top N peaks): add `--top 50` for more. It needs llvm-symbolizer: unnamed frames warn and only
match between identical binaries.

## Text report for humans

`heaptide report DIR --top 10` prints the same data plus PEAK (who held memory at the peak),
HOTSPOTS (most allocations) and TEMPORARIES (alloc then immediate free, a cheap win). Use it
when the question is "where does memory go", not "what leaked".

## Pitfalls

- Run GPU jobs the way the project schedules its GPU (a queue, a lock); heaptide does not.
- A forked child keeps the parent's tables until exec; trust exec'd children, not fork-only ones.
- No `llvm-symbolizer` on the machine: sites show `[module+0xoffset]` only and the report says so. Install
  LLVM or set `HEAPTIDE_SYMBOLIZER`.
- ASan builds work only with a shared ASan runtime (gcc's default, clang `-shared-libsan`); clang's
  default static ASan gets a warning and an empty report.
- Mojo programs: heaptide switches Mojo's allocator to libc at start so its heap is visible; that
  is skipped when attached.
- A snapshot never interrupts a thread asleep in a syscall; such a thread's saved registers are
  not scanned, so in a snapshot (never at exit) a block held only there can show as leaked.
- Overhead is about 6x on an allocation-bound loop (`bench/alloc_bench`: 0.02 s bare, 0.13 s) and
  near zero on code that mostly waits; time-sensitive bugs may still hide under it.
- Linux x86-64 only.

## Reporting back

State: the command, the before numbers (`direct`, `indirect`, `growth`, `growth_window_bytes`), the
site you fixed, and the after numbers from the re-run. Name anything you suppressed and why.
