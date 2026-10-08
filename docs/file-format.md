# File format

The shim writes two files per process into `HEAPTIDE_OUT` (default `.work/heaptide`) at exit:

- `heaptide.<pid>.bin`: the data below, little-endian
- `heaptide.<pid>.maps`: a copy of `/proc/self/maps`, used to symbolize addresses

With `--py`, `py.<pid>.txt` adds the PYTHON section as text.

## `heaptide.<pid>.bin`

| offset | size | content |
|---|---|---|
| 0 | 64 | header: 8 × u64 |
| 64 | 8 × `nips` | instruction pointers of every stack, concatenated |
| ... | 56 × `n` | one row per call stack (ids 1..n) |
| ... | 8 × `n` | direct leaked bytes per stack |
| ... | 8 × `n` | indirect leaked bytes per stack |
| ... | 24 × `nsamples` | live-byte samples |

Header (u64 each):

| field | value |
|---|---|
| 0 | magic `"HEAPTIDE"` (0x4544495450414548) |
| 1 | pid |
| 2 | `n`, number of stacks |
| 3 | `nips`, number of instruction pointers |
| 4 | `nsamples` |
| 5 | sampling period in ms |
| 6, 7 | 0 |

Row (56 B, `row_t` in `shim/heaptide.c`, size fixed by a static assert):

| offset | type | field |
|---|---|---|
| 0 | u64 | allocations |
| 8 | u64 | bytes allocated |
| 16 | u64 | temporaries (freed with no allocation in between, same thread) |
| 24 | u64 | live blocks at exit |
| 32 | u64 | live bytes at exit |
| 40 | u64 | live bytes at the last recorded peak |
| 48 | u32 | offset of the stack's first IP in the IP array |
| 52 | u8 | kind: 0 cpu, 1 gpu, 2 host (pinned), 3 mmap |
| 53 | u8 | depth (number of IPs) |
| 54 | 2 | padding |

Sample (24 B): u64 monotonic time in ns, u64 live bytes of cpu + host + mmap, u64 live gpu bytes.

IPs are return addresses. To symbolize one, find its mapping in the `.maps` file and subtract
the mapping's lowest start minus one (the address of the call instruction). The report sends all
of them to `llvm-symbolizer` in one batch per process.
