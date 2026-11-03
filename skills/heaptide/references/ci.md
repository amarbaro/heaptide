# Gate a change

```
heaptide run --out .work/base -- CMD      # base build
heaptide run --out .work/new  -- CMD      # the change
heaptide diff .work/base .work/new [--fail-on new|grown|none] [--json] [--top N]
```

- NEW: a leak site only the change has (exit 1 by default). GROWN: direct or peak up by more
  than 64 KiB and 10% (exit 1 with `--fail-on grown`). GONE, SHRUNK: reported, exit 0.
- A missing value (`--fail-on` with nothing after it) or an unknown option is exit 2, never a silent default.
- Sites match without line numbers, so moved code is not a change.
- Only sites a report shows are compared (every leak, the top N peaks): `--top 50` for more.
- Needs `llvm-symbolizer`: unnamed frames warn and match only between identical binaries.
- One run per side is noisy for GROWN on a busy machine: gate on `new`, read `grown`.

Single-run gates: `heaptide report DIR --fail-on leaks` (or `growth`, with `--growth-limit MIB`).

## A fix's receipt

`scripts/ht.sh prove BEFORE AFTER [SITE]` prints each leaking site's direct bytes before -> after,
then `heaptide diff`. Quote both lines in the report or commit message.
