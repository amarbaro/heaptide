# Changelog

Full notes in `docs/releases/`. Newest first.

- Unreleased: `report/default.supp` drops the ROCm runtime's one-time allocations by default (`--no-default-supp`, one `suppressed N B in K blocks` line, JSON `suppressed_bytes`/`suppressed_blocks`); `report` no longer waits on debuginfod inside llvm-symbolizer (a ROCm run: 90 s to 0.4 s). Shim: `dlsym` found on glibc before 2.34; the stack counter stops at the table limit (it could wrap onto ids in use) and a full table prints a WARN line; the spinlock pauses and yields.
- [v0.1.6](docs/releases/v0.1.6.md) (2026-10-26): Python holder chains, per-process reports with --no-children/--only, snapshot --label, attach mmap crash and fork timeline fixed, --py sys.exit() and -c
- [v0.1.5](docs/releases/v0.1.5.md) (2026-10-12): interactive flame graph (`heaptide flame`), Python mode ~10x faster (memray-style hooks), detach without restart, attach safe on own allocators (Chromium hang and crash fixed), Qt/QML heaps
- [v0.1.4](docs/releases/v0.1.4.md) (2026-10-12): who holds memory, attach to a running PID, diff as a CI gate, GPU timeline, Python frames in native stacks, Mojo heap visible
- [v0.1.3](docs/releases/v0.1.3.md) (2026-10-11): `--py` runs `python -m`, 3.9x faster Python snapshots, maps inside the `.bin`, EINTR-safe report writes, test race fix
- [v0.1.2](docs/releases/v0.1.2.md) (2026-10-09): report on stop signals and crashes, millisecond live snapshots, 2-3x faster exit scan, collision-proof sites, madvise reuse, mremap
- [v0.1.1](docs/releases/v0.1.1.md) (2026-10-09): two external reviews: interior pointers, debuginfod, fork, mprotect commits, `--py-depth`, 9 bug fixes
- [v0.1.0](docs/releases/v0.1.0.md) (2026-10-09): first public release
