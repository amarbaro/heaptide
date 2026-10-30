# report.json

`heaptide report DIR --json`: an array, one object per traced process. The target is usually the
object with `"root": true` (the first traced process) or the one with the most leak sites.

```
{"pid", "exe", "cmdline", "root",
 "direct", "indirect",                         // leaked bytes, whole process
 "suppressed_bytes", "suppressed_blocks",      // dropped by the default runtime list
 "dropped_allocations",                        // > 0: the stack table was full, totals are a lower bound
 "ended_without_report",                       // true: killed or _exit; written afterwards, no leak scan
 "before_attach_bytes",                        // attached: glibc heap in use at attach, the most it can miss
 "growth": "PASS|FAIL|too short|attached", "growth_bytes_per_s", "growth_window_bytes", "window_from_s",
 "leaks":  [{"bytes", "direct", "indirect", "blocks", "kind": "cpu|gpu|host|mmap", "site"}],
 "held":   [{"site", "held_by"}],              // "make_table x.c:7 << global g_cache"
 "py_held":[{"kind", "site", "held_by"}],      // --py: "bytearray << Box.data[0] << __main__.keep['a']"
 "gpu_timeline": [{"site", "points": [[t_ms, bytes]]}]}   // --gpu-timeline
```

The `--py` PYTHON section (sites by line, and its `WARN untracked` line) is text only: `DIR/py.<pid>.txt`.

## jq recipes

```
jq '[.[] | {pid, exe, direct, growth}]' report.json                          # one line per process
jq '.[] | select(.root) | .leaks | sort_by(-.direct) | .[:10]' report.json  # top direct leaks
jq '.[] | .leaks[] | select(.kind == "gpu")' report.json                     # device memory left
jq '.[] | select(.growth == "FAIL") | .held' report.json                     # who holds the growth
jq '[.[] | .dropped_allocations] | add' report.json                          # anything lost?
```

Exit codes: `report` 0 clean, 1 leak or growth (per `--fail-on any|leaks|growth|none`), 2 tool error.
