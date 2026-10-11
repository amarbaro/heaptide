# How it works

heaptide has three parts: a C shim preloaded into the target (`shim/heaptide.c`), a Mojo report
(`report/heaptide_report.mojo`), and a bash launcher (`heaptide`). The shim does all the
recording and the reachability scan inside the target. The report only reads the file and
symbolizes the stacks.

## Hooks

`LD_PRELOAD` puts the shim ahead of libc. It hooks:

- `malloc`, `calloc`, `realloc`, `reallocarray`, `free`, `free_sized`, `free_aligned_sized`,
  `posix_memalign`, `aligned_alloc`, `memalign`, `valloc`
- `mmap`, `mmap64`, `munmap`, `mremap`, for anonymous mappings that are not `PROT_NONE` (kind
  `mmap`). An anonymous `PROT_NONE` mapping is kept as a reservation: `mprotect` that commits
  part of it adds that range as an `mmap` block at the `mprotect` call, and `mprotect(PROT_NONE)`
  removes it again. `madvise(MADV_DONTNEED|MADV_FREE)` moves the range to a returned list that
  keeps its site; at report time `mincore` finds its resident pages (reused by plain writes, as
  Go's allocator does) and they count again. `mremap` of a reservation moves its entries.
- `hipMalloc`, `hipMallocAsync`, `hipHostMalloc`, `hipFree`, `hipFreeAsync`, `hipHostFree`
- `dlsym`, because the Mojo runtime resolves HIP functions through `dlsym` after `dlopen`, which
  plain symbol interposition never sees. Other names are passed on with a forced tail call
  (`musttail`): glibc resolves `RTLD_NEXT` from the return address, which must stay the caller's

The real allocators are resolved lazily on first use. Another library's constructor can
allocate before the shim's constructor runs. The few allocations `dlsym` makes while resolving
come from a static 16 KiB buffer.

A thread-local counter marks calls into the real allocators. A mapping made while it is set
(jemalloc behind `malloc`, the HSA runtime behind `hipMalloc`) belongs to that allocator's block
and is not counted again. glibc's own malloc calls mmap internally and is never seen. The shim's
own memory comes from raw `syscall(SYS_mmap)`.

**Python in native stacks.** `--py` on Python 3.12+ adds `-X perf`: CPython then runs every Python
function through a small generated stub, one per code object, and lists the stubs in
`/tmp/perf-<pid>.map`. The stubs are native frames, so the stacks heaptide already records contain
them with no extra work per allocation; the report names any frame that falls in no file mapping
but in a stub range as `py::function file.py`, and counts it as the program's own frame.

**Diff.** `heaptide diff BASE NEW` runs `report --sites` on each dir: one tab-separated line per
site the report shows (direct, indirect, peak, live, kind, site), then `report --diff` keys both
on kind plus the site with every `:line:col` removed and compares. Nothing is re-analysed: it
costs two reports (two 65 536-site reports: 2.7 s).

**Attach.** `heaptide attach PID` has gdb call `setenv` (output dir, attach flag) and `dlopen` in
the target. The constructor then cannot rely on interposition: the program's GOT slots were bound
before the library existed. It walks every loaded object's `DT_JMPREL` and `DT_RELA` relocations
(`JUMP_SLOT`, `GLOB_DAT`) and writes its own address into each slot that names a hooked function
(its own definitions come from its own `dlopen` handle, since a plain `&malloc` would bind to
libc's). It also records the writable anonymous mappings and `[heap]` present at that moment: at
report time they are scanned as roots, skipping the blocks tracked since attach, so a block that
is reachable only through older memory is not reported as lost.

**GPU timeline.** With `--gpu-timeline`, GPU and pinned-host rows skip the per-thread batch (so
their counts are current) and each new such row is added to a watch list. Every sample, the
sampler appends `{sample, row, bytes}` for each watched row whose live bytes moved: 16 B per
change, nothing for a quiet row. `streams` interns `hipMallocAsync`'s stream as one more frame
(bit 63 set), so each (site, stream) is its own row. `frag` adds two pseudo rows: device bytes in
use (`hipMemGetInfo`) minus the live GPU blocks, against the value read just before the program's
first GPU allocation, and the default pool's reserved minus used bytes. The sampler only calls
HIP once the program has allocated on the GPU, so it never initialises HIP for a program.

**Mojo's heap.** Mojo code allocates through `KGEN_CompilerRT_AlignedAlloc`, which jumps through a
function pointer to a tcmalloc bundled in the runtime; malloc hooks never see those blocks, only
tcmalloc's 2 MiB arena mapping. The runtime exports `KGEN_CompilerRT_SetAsanAllocators()`, a
sanitizer switch that points that function pointer at libc `malloc`/`aligned_alloc`/`free`.
heaptide's constructor calls it before Mojo allocates (a block from tcmalloc must never reach
libc `free`), so every Mojo allocation passes through the hooks above.

## Per allocation

1. `unw_backtrace` (libunwind, per-thread cache) collects up to 64 return addresses. The shim's
   own frames are dropped. The stack ends before the first address it repeats: CPython re-enters
   its eval loop through C on every import and callback, and recursion adds no new call site.
2. The frames are hashed (64-bit) into a stack id. The table entry also keeps a second,
   independent 32-bit hash; a hit whose check differs is another stack and takes the next key. A new stack copies its frames into one mmap'd
   arena and gets a 56 B counter row.
3. The block goes into a 64-shard open-addressing table, 16 B per entry: address, then size,
   flags and stack id packed into one word.
4. The row's counters (allocations, bytes, temporaries, live blocks, live bytes) are batched per
   thread: each thread holds pending changes for 16 call sites in its own slot and adds them to
   the shared row every 64 updates, on eviction, before a peak check, and at exit (a snapshot's
   fork flushes every thread's slot in its own copy). Live bytes per
   kind are counted per thread, one cache line each, and summed by readers. With 8 threads at one
   call site, the shared atomic counters were 77% of the time (perf), not the unwinding (0.4%).
5. Every 256th allocation of a thread, and on any block of 64 KiB or more, the summed live total
   is compared with the last peak. When it is 1% higher, each row's live bytes are copied into
   its peak field. No event log is kept.

A call stack's id comes from a sharded hash table under a spinlock; each thread also keeps a
64-entry cache of stack hash -> id (ids never change), so the common case, a site seen before,
takes no lock. Before the cache, 8 threads allocating at one site spent 80% of their time queued
on one shard's lock (perf) and the benchmark moved +-30% with code layout alone.

`realloc(p, 0)` frees `p` (glibc), so only a failed non-zero `realloc` restores the old block; a
failed `hipFree` restores its block too. `errno` is saved around the bookkeeping. The `dlsym` hook
hands out its HIP wrappers only when the real symbol exists. `mremap` moves the part of every
range inside the source (a reservation cut in the middle included) and, with `MREMAP_FIXED`,
forgets what the destination replaced. Under an address-space limit (`ulimit -v`) the row and
frame arenas shrink to a quarter of it; if even that cannot be mapped, the shim records nothing
and says so, and a report that cannot get its memory says so instead of writing a partial file.

A free that hits the block allocated last on the same thread counts as a temporary (heaptrack's
definition). Anonymous mappings live in a separate locked array: a partial `munmap` trims or
splits an entry, `mremap` re-homes it at the call site the way `realloc` does, and `MAP_FIXED`
replaces whatever was mapped there.

A sampler thread records time, live host bytes and live GPU bytes every `--every` ms.

## At exit, on a signal, or as a snapshot (LeakSanitizer's design)

The shim registers its exit handler first, so it runs after the program's own `atexit`
handlers and static destructors. SIGTERM, SIGINT and SIGHUP get a handler that only sets a flag,
where the program kept the default; the sampler thread then writes the report and re-raises the
signal, so the process still dies of it. The sampler also writes snapshots: every
`--snapshot-every`, or when `OUT/snapshot` appears (`heaptide snapshot`). A snapshot is the same
report of a copy: with the threads parked and their stack pointers read, the sampler forks with a
raw `clone` (no atfork handlers, no malloc locks a parked thread may hold, no exit signal so the
program's `wait` never sees it) and releases the threads; the child scans its copy-on-write image,
writes the files under the parent's pid and exits, and the sampler reaps it before the next one.
The pause is the fork: 1M live blocks 147 -> 3-7 ms, 10M 1851 -> 15 ms (`bench/snap_pause.c`).
If the fork fails (no memory, process limit) the snapshot scans in place and restores the reach
marks afterwards. When a thread cannot be parked the snapshot is skipped, since that thread could
be growing the table being read. Files are written as `.tmp` and renamed.

A crash (SEGV, BUS, FPE, ILL, ABRT, where the program kept the default) sets the same flag and the
crashing thread waits, on a signal stack so a stack overflow works too (the main thread's is
static, other threads get one at their first allocation); its stack is
scanned from the interrupted stack pointer. The sampler writes the report and kills the process
with the signal. After 10 s (a crash inside the shim, a heap too broken to scan) the thread gives up
and dies as it would have.

With `--py`, the Python runner installs its own SIGTERM/SIGHUP handler: it writes the PYTHON
section, calls the shim's exported `heaptide_final()` (the native report, heap untouched), then
re-raises the signal. A stop signal that arrives before that handler exists (interpreter startup)
is held by the shim and handed over by `heaptide_py_ready()`, which the runner calls once its
handler is in; if Python exits first, the shim re-raises it after the report. Ctrl-C needs nothing extra: `KeyboardInterrupt` exits through `atexit`. A
watcher thread rewrites the PYTHON section whenever the native `.bin` changes, so every snapshot
has one.

0. **List the loaded objects' writable segments** (`dl_iterate_phdr`) while the other threads
   still run: a thread parked inside the loader would hold the lock that call needs.
1. **Park the other threads.** (For a snapshot, a thread asleep in a system call is left asleep:
   the park signal would end its `nanosleep`, `poll` or `epoll_wait` with EINTR, and a program
   that does not retry, like Mojo's `sleep`, then runs on. Its stack pointer comes from
   `/proc/self/task/<tid>/syscall`; its saved registers are not scanned.)
    Each thread listed in `/proc/self/task` gets the park signal: the highest real-time signal from `SIGRTMAX-2` down that the program left at its default (a program's own handler is never replaced). The
   handler saves the thread's registers from its `ucontext` and waits until classification ends.
   A thread that blocks the signal is not signalled; its stack pointer comes from
   `/proc/self/task/<tid>/syscall`, with no registers. LeakSanitizer stops threads with ptrace
   instead.
2. **Roots.** These are scanned for 8-byte words pointing at or into a live CPU or mmap block
   (LeakSanitizer does the same). A word below the lowest block or past the highest is dropped
   at once; any other word goes to a branchless binary search over the blocks sorted by start,
   in three levels (every 256th start, every 16th, then a 16-entry run of 4 cache lines). Words
   go through 16 at a time, level by level, with each word's next run prefetched before any is
   read, so the misses of a large heap overlap. Worst case, every word an interior pointer
   (`bench/scan_bench.c`): 1M blocks 1.05 s, 10M blocks 16.6 s (bare program 0.05 / 0.79 s).
   - the writable segments of every loaded object
   - every other thread's registers, and its stack from the stack pointer up. The thread calling
     exit is not scanned: its returned frames leave stale pointers (on Ubuntu 24.04 one hid a real
     indirect leak that LeakSanitizer reports)
   - every block with a dynamic-linker frame in its stack. This is LeakSanitizer's
     `use_ld_allocations`: a thread's DTV is held as `dtv + 1` in its TCB, an interior pointer.
   - in a forked child, every block inherited from the parent (a `pthread_atfork` handler marks
     them). They are the parent's leaks, and may hold the only pointer to a child block.
3. **Flood.** Each reachable block's contents are scanned the same way.
4. **Classify.** For every block still unreached, its contents are scanned with the INDIRECT
   mark. Blocks found only that way are indirect leaks; the rest are direct.
5. **Write** the `.bin` file (maps text at its end), then release the parked threads.

Stacks and mappings are copied 64 pages per `process_vm_readv` call instead of read in place.
Recent glibc installs thread-stack guard pages with `MADV_GUARD_INSTALL`, so neighbouring stacks
appear as one VMA in `/proc/self/maps`. Scanning to the end of that VMA faults on the next
stack's guard page; a copy stops at the first page that fails to read. Mappings skip unreadable
pages and continue.

GPU and pinned host blocks are never scanned, and nothing scanned can reach them: the HIP
runtime's own tables point at every one of them. One left unfreed at exit is a direct leak.

**Who holds it.** While the scan floods from the roots, the first time a block of a site is
reached it records what reached it: the site of the block being scanned, or the root (the address
of a global's word, or "a thread"). One u64 per site, not per block: 65 536 sites cost 512 KiB, and
10M blocks would have cost 40 MB for a per-block parent. The report follows these links from each
PEAK site up to a root (at most 6 hops) and names the global with llvm-symbolizer's `DATA` lookup,
in the same batch as the frames. It is the first path found, not every path: a block reachable two
ways shows one of them.

## The report

For each process the report reads the `.bin` file, picks the stacks each section needs (all
leaking stacks, all of them so totals and `--supp` cover every site, and the top N by peak, allocations and temporaries), and sends their addresses to
`llvm-symbolizer --no-inlines` in one batch. A module without `.debug_info` is swapped for its
debug file by build-id: from elfutils' cache (`~/.cache/debuginfod_client/<id>/debuginfo`, shared
with gdb), or fetched with curl from the first `DEBUGINFOD_URLS` server and cached there. ROCm's
llvm-symbolizer is built without debuginfod, so the report does this itself. A site is the first
two frames plus the first frame in the user's own source: a path outside `/usr`, `/opt` and
`.venv`, or, without debug info, a frame in the executable. `tracemalloc_*` frames are skipped
like libheaptide's: in `--py` mode they are heaptide's own instrumentation.

GROWTH is a least-squares fit of live bytes over time. The rise is the slope times the span after
the first 20% of samples. It fails when that rise exceeds 64 KiB and 5% of the mean, or exceeds
`--growth-limit`. If the first window fails, the start moves later in 2.5% steps up to half the
run, and the earliest window that passes wins. Each later window's slope is projected over the
same full span, so a shorter window cannot shrink a steady leak under the threshold. Only a slope
that has really flattened (setup over) passes.
