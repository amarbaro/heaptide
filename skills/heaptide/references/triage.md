# Triage: what is real

| report says | it means | do |
|---|---|---|
| `direct` at a site in your code | unfreed, nothing points to it | free it, or give it an owner |
| `indirect` only | held only by another leaked block | fix that block's owner |
| `kind: gpu` or `host` unfreed | device or pinned buffer never freed | always real: GPU blocks are never treated as reachable |
| `kind: mmap` | an anonymous mapping never unmapped, or the part of a `PROT_NONE` reservation that `mprotect` committed (site = the `mprotect` call) | real unless the runtime keeps that arena by design |
| a few hundred B in `hsa_amd_signal_create`, `amd::roc::Device`, `_dl_*` | HIP/Mojo runtime residue | suppress, do not "fix" |
| `suppressed N B in K blocks by the default runtime list` | the ROCm runtime's one-time blocks, dropped by `report/default.supp` | nothing; `--no-default-supp` lists them |
| `WARN stack table full: N allocations` (JSON `dropped_allocations`) | more distinct stacks than the table holds; those allocations are in no site | totals are a lower bound: lower `HEAPTIDE_DEPTH` or shorten the run |
| `WARN untracked: N blocks` in PYTHON (`--py`) | the live Python table reached its cap (1/8 of free memory, `HEAPTIDE_PY_MEM` MiB) | PYTHON is a lower bound; raise the cap only with memory to spare |
| GROWTH FAIL, few direct leaks | memory held, not lost: caches, queues, pools | read `held`: the chain from the site to a global, a thread's stack, or memory from before attach |
| `ENDED WITHOUT A REPORT` (JSON `ended_without_report`) | killed (SIGKILL, OOM killer) or `_exit`; written afterwards from its tables | no leak scan: read PEAK, the site on top is what filled memory |
| `before attach: N` | the glibc heap held N at attach, no site can show it | leaks and sites are a lower bound by up to N |
| GROWTH `too short` | fewer than 5 samples | run longer or lower `--every` |
| GROWTH `attached` (text `n/a`) | live bytes count from 0 at attach | snapshot twice under load, then `heaptide diff A B` |

## Suppressions

The ROCm runtime's own one-time blocks are suppressed by default. For anything else: `--supp FILE`,
one substring of a site per line, kept next to the test that uses it with a comment per line saying
why it is residue. Never suppress a site your own code can also produce (`setenv`, `strdup`) by its
two-frame site alone.

## Growth

- The window starts after the first 20% of samples and moves later (up to half the run) to skip a
  long setup; `window_from_s` says where. The rise is the slope over the full span, so a short late
  window cannot hide a steady leak.
- FAIL needs a rise over 64 KiB **and** over 5% of mean live memory. On a GB-sized process that is
  lenient: add `--growth-limit MIB` (e.g. 16) and quote `growth_window_bytes`.
- A run that is mostly loading fails because loading looks like growth. Lengthen the steady part;
  never raise the limit to make it pass.
- Holders are recorded for memory live at the report. For a service: `heaptide snapshot DIR` while
  it grows, twice under the same load, then `heaptide diff`.
