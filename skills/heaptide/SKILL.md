---
name: heaptide
description: Find, explain and prove fixes for memory leaks and memory growth in CPU, GPU (HIP, Mojo) and Python programs on Linux with heaptide, a preload profiler that needs no rebuild. Use when a process's memory or VRAM keeps growing, when a run must be shown leak-free, when asked who holds memory, when a running service must be profiled without a restart, when a Python job's RSS balloons, or when a change must be gated against its base in CI.
---

# heaptide

Preloaded into an unmodified program; one file per process at exit; `heaptide report --json` is
the stable interface. `scripts/ht.sh` wraps the loop below and prints one line per finding.

## Route by the question

| The user asks | Do | Read |
|---|---|---|
| "does it leak?" / "is it clean?" | `ht.sh run .work/ht -- CMD`, fix the top direct leak, `ht.sh prove` | [triage](references/triage.md) |
| "memory keeps growing" (no leak shown) | a longer steady run, then `held` chains; for a service, two snapshots + `diff` | [triage](references/triage.md) § growth |
| "who holds this memory?" | `held` (native) and `py_held` (`--py`, Python names) | [json](references/json.md) |
| "VRAM is not given back" | `--gpu --gpu-timeline all` | [runtimes](references/runtimes.md) § GPU |
| a Python job | `--py`, add `--py-depth 4` for shared helpers | [runtimes](references/runtimes.md) § Python |
| a running process, no restart | `attach` / `snapshot` / `detach` | [runtimes](references/runtimes.md) § attach |
| "gate this PR on leaks" | two runs + `heaptide diff --fail-on new` | [ci](references/ci.md) |

## The loop

1. **Reproduce small.** The shortest run that shows the symptom; for growth, loop the suspect
   operation so setup is under half the run.
2. **Run capped.** `scripts/ht.sh run .work/ht [--gpu] [--py] -- CMD ARGS`. It runs under a systemd
   memory cap (`HT_MEM`, default half of free memory): a blowup kills the job, not the session.
   Without the script: `systemd-run --user --scope -p MemoryMax=8G heaptide run --out .work/ht -- CMD`.
3. **Read** the summary (or `report.json`). Check the warnings first: `WARN stack table full`
   and `--py WARN untracked` mean the numbers are a lower bound.
4. **Fix the top direct leak** (`site` = `function file:line`, the user's frame included).
   Indirect leaks usually go with their owner: re-run before touching them.
5. **Prove it.** Same command again, then `scripts/ht.sh prove .work/ht .work/ht-fixed`: each
   site's direct bytes before -> after, plus `heaptide diff`. No second run, no fix.

## Rules that cost a turn when skipped

- Build C/C++ with `-g`, Mojo with `-g1`; without symbols, sites stop at `[module+0xoffset]`.
- `--gpu` for every Mojo GPU program (Mojo pools buffers otherwise: one block).
- Short runs: `--every 20` under a few seconds, or GROWTH says `too short`.
- SIGKILL and the OOM killer write nothing: add `--snapshot-every 60` to long jobs.
- A process that leaves through `_exit()` (dash, Ubuntu's `sh`) writes no report: use `bash -c`.
- Run GPU jobs through the project's GPU scheduler; heaptide does not queue.
- Never attach to a browser in use; attached runs can under-report, never over-report.

## Reporting back

The command, the before numbers (`direct`, `indirect`, `growth`, `growth_window_bytes`), the
warnings, the site fixed, the after numbers from the re-run, and every suppression with its reason.
