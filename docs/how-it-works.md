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
  `mmap`)
- `hipMalloc`, `hipMallocAsync`, `hipHostMalloc`, `hipFree`, `hipFreeAsync`, `hipHostFree`
- `dlsym`, because the Mojo runtime resolves HIP functions through `dlsym` after `dlopen`, which
  plain symbol interposition never sees

The real allocators are resolved lazily on first use. Another library's constructor can
allocate before the shim's constructor runs. The few allocations `dlsym` makes while resolving
come from a static 16 KiB buffer.

A thread-local counter marks calls into the real allocators. A mapping made while it is set
(jemalloc behind `malloc`, the HSA runtime behind `hipMalloc`) belongs to that allocator's block
and is not counted again. glibc's own malloc calls mmap internally and is never seen. The shim's
own memory comes from raw `syscall(SYS_mmap)`.

## Per allocation

1. `unw_backtrace` (libunwind, per-thread cache) collects up to 64 return addresses. The shim's
   own frames are dropped. The stack ends before the first address it repeats: CPython re-enters
   its eval loop through C on every import and callback, and recursion adds no new call site.
2. The frames are hashed (64-bit) into a stack id. A new stack copies its frames into one mmap'd
   arena and gets a 56 B counter row.
3. The block goes into a 64-shard open-addressing table, 16 B per entry: address, then size,
   flags and stack id packed into one word.
4. The row's counters (allocations, bytes, temporaries, live blocks, live bytes) are batched per
   thread: each thread holds pending changes for 16 call sites in its own slot and adds them to
   the shared row every 64 updates, on eviction, before a peak check, and at exit. Live bytes per
   kind are counted per thread, one cache line each, and summed by readers. With 8 threads at one
   call site, the shared atomic counters were 77% of the time (perf), not the unwinding (0.4%).
5. Every 256th allocation of a thread, and on any block of 64 KiB or more, the summed live total
   is compared with the last peak. When it is 1% higher, each row's live bytes are copied into
   its peak field. No event log is kept.

A free that hits the block allocated last on the same thread counts as a temporary (heaptrack's
definition). Anonymous mappings live in a separate locked array: a partial `munmap` trims or
splits an entry, `mremap` re-homes it at the call site the way `realloc` does, and `MAP_FIXED`
replaces whatever was mapped there.

A sampler thread records time, live host bytes and live GPU bytes every `--every` ms.

## At exit (LeakSanitizer's design)

The shim registers its exit handler first, so it runs after the program's own `atexit`
handlers and static destructors.

1. **Park the other threads.** Each thread listed in `/proc/self/task` gets `SIGRTMAX-2`. The
   handler saves the thread's registers from its `ucontext` and waits until classification ends.
   A thread that blocks the signal is not signalled; its stack pointer comes from
   `/proc/self/task/<tid>/syscall`, with no registers. LeakSanitizer stops threads with ptrace
   instead.
2. **Roots.** These are scanned for 8-byte words equal to the start of a live CPU or mmap block:
   - the writable segments of every loaded object
   - every other thread's registers, and its stack from the stack pointer up. The thread calling
     exit is not scanned: its returned frames leave stale pointers (on Ubuntu 24.04 one hid a real
     indirect leak that LeakSanitizer reports)
   - every block with a dynamic-linker frame in its stack. This is LeakSanitizer's
     `use_ld_allocations`: a thread's DTV is held as `dtv + 1` in its TCB, an interior pointer.
3. **Flood.** Each reachable block's contents are scanned the same way.
4. **Classify.** For every block still unreached, its contents are scanned with the INDIRECT
   mark. Blocks found only that way are indirect leaks; the rest are direct.
5. **Write** the `.bin` and `.maps` files, then release the parked threads.

Stacks and mappings are copied 64 pages per `process_vm_readv` call instead of read in place.
Recent glibc installs thread-stack guard pages with `MADV_GUARD_INSTALL`, so neighbouring stacks
appear as one VMA in `/proc/self/maps`. Scanning to the end of that VMA faults on the next
stack's guard page; a copy stops at the first page that fails to read. Mappings skip unreadable
pages and continue.

GPU and pinned host blocks are never scanned, and nothing scanned can reach them: the HIP
runtime's own tables point at every one of them. One left unfreed at exit is a direct leak.

## The report

For each process the report reads the `.bin` file, picks the stacks each section needs (all
leaking stacks and the top N by peak, allocations and temporaries), and sends their addresses to
`llvm-symbolizer --no-inlines` in one batch. A site is the first two frames plus the first frame
in the user's own source: a path outside `/usr`, `/opt` and `.venv`, or, without debug info, a
frame in the executable.

GROWTH is a least-squares fit of live bytes over time. The rise is the slope times the span after
the first 20% of samples. It fails when that rise exceeds 64 KiB and 5% of the mean, or exceeds
`--growth-limit`. If the first window fails, the start moves later in 2.5% steps up to half the
run, and the earliest window that passes wins. Each later window's slope is projected over the
same full span, so a shorter window cannot shrink a steady leak under the threshold. Only a slope
that has really flattened (setup over) passes.
