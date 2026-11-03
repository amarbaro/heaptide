# Changelog

Full notes in `docs/releases/`. Newest first.

- [v0.1.10](docs/releases/v0.1.10.md) (2026-11-03): correction, the GitHub runner's +10 MB was the host mapping 8 MiB of the shm file, not heaptide; README badges
- [v0.1.9](docs/releases/v0.1.9.md) (2026-11-03): THP "always" hosts: the shim no longer adds about 10 MB per traced process
- [v0.1.8](docs/releases/v0.1.8.md) (2026-11-03): held GPU memory not a leak (ComfyUI 10.88 GiB false leaks to 0), a report after SIGKILL/OOM kill/_exit, --py OOM cap and half the overhead, readable busy snapshots, three CLI fixes (--out reuse, python -u, diff options)
- [v0.1.7](docs/releases/v0.1.7.md) (2026-10-30): runs on glibc before 2.34, default ROCm suppression list and 200x faster report symbolization, stack table clamp with warning, `py::` frames under Mojo 1.2.0
- [v0.1.6](docs/releases/v0.1.6.md) (2026-10-26): Python holder chains, per-process reports with --no-children/--only, snapshot --label, attach mmap crash and fork timeline fixed, --py sys.exit() and -c
- [v0.1.5](docs/releases/v0.1.5.md) (2026-10-12): interactive flame graph (`heaptide flame`), Python mode ~10x faster (memray-style hooks), detach without restart, attach safe on own allocators (Chromium hang and crash fixed), Qt/QML heaps
- [v0.1.4](docs/releases/v0.1.4.md) (2026-10-12): who holds memory, attach to a running PID, diff as a CI gate, GPU timeline, Python frames in native stacks, Mojo heap visible
- [v0.1.3](docs/releases/v0.1.3.md) (2026-10-11): `--py` runs `python -m`, 3.9x faster Python snapshots, maps inside the `.bin`, EINTR-safe report writes, test race fix
- [v0.1.2](docs/releases/v0.1.2.md) (2026-10-09): report on stop signals and crashes, millisecond live snapshots, 2-3x faster exit scan, collision-proof sites, madvise reuse, mremap
- [v0.1.1](docs/releases/v0.1.1.md) (2026-10-09): two external reviews: interior pointers, debuginfod, fork, mprotect commits, `--py-depth`, 9 bug fixes
- [v0.1.0](docs/releases/v0.1.0.md) (2026-10-09): first public release
