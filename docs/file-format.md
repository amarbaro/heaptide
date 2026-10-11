# File format

The shim writes one file per process into `HEAPTIDE_OUT` (default `.work/heaptide`) at exit, on a stop signal, and at each snapshot (latest wins; written as `.tmp` and renamed, so a reader never sees half a file or maps from another snapshot):

- `heaptide.<pid>.bin`: the data below, little-endian, ending with a copy of `/proc/self/maps` used to symbolize addresses

Files from before v0.1.3 keep the maps in a separate `heaptide.<pid>.maps`; the report still reads them.

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
| ... | 8 × `n` | leaked blocks per stack (only when header field 6 has bit 0 set) |
| ... | 8 × `n` | retention, per stack: its first holder, a stack id (1-based), `1<<63 \| address` of the global word that reached it, `1<<62` a thread's stack or registers, 0 none (only when header field 6 has bit 2 set) |
| ... | 8 + 16 × `count` | GPU timeline (only when header field 6 has bit 3 set): u64 `count`, then per change `u64 sample << 32 \| id`, `u64 live bytes`; id `0xFFFFFFFF` = device bytes in use beyond the live blocks, `0xFFFFFFFE` = async pool reserved minus used |
| ... | field 7 | `/proc/self/maps` text (only when header field 6 has bit 1 set) |

Header (u64 each):

| field | value |
|---|---|
| 0 | magic `"HEAPTIDE"` (0x4544495450414548) |
| 1 | pid |
| 2 | `n`, number of stacks |
| 3 | `nips`, number of instruction pointers |
| 4 | `nsamples` |
| 5 | sampling period in ms |
| 6 | flags: bit 0 = leaked block counts follow the samples (since v0.1.2; older files: the report shows live blocks); bit 1 = maps text ends the file (since v0.1.3); bit 2 = the retention array follows the leaked block counts (since v0.1.4); bit 3 = the GPU timeline follows it (`--gpu-timeline`, since v0.1.4) |
| 7 | length of the maps text in bytes (0 without bit 1) |

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

IPs are return addresses. To symbolize one, find its mapping in the maps text and subtract
the mapping's lowest start minus one (the address of the call instruction). The report sends all
of them to `llvm-symbolizer` in one batch per process.

The report checks a file before reading it: counts below the shim's limits (2^25 stacks, 2^31
frames, 2^21 samples), a file long enough for every section they promise, and every stack's
frames inside the frame array. Anything else is exit 2.
