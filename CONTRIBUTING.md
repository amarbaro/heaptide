# Contributing

Bug reports, fixes and new checks are welcome.

## Report a bug

Open an issue with the command you ran, the report output (`heaptide report DIR --json` is best),
your distribution, glibc version (`ldd --version`) and, for GPU bugs, ROCm version and GPU. A small
program that reproduces it helps most.

Security problems go through private reporting instead, see [SECURITY.md](SECURITY.md).

## Send a change

1. Build: `./build.sh` (needs a C compiler, `libunwind`, Mojo 1.1; see the README).
2. Test: `tests/run.sh --cpu` must print `PASS N/N`. If you touch GPU paths and have ROCm, run
   `tests/run.sh` too and say so in the pull request.
3. A fix comes with a check in `tests/run.sh` that fails without it. A new behaviour comes with a
   planted-bug program in `tests/` and its check, ideally with heaptrack or LeakSanitizer as the
   oracle (see [docs/testing.md](docs/testing.md)).
4. A speed or memory claim comes with before and after numbers on the same input
   (`bench/alloc_bench.c`, arms interleaved).
5. Update the docs the change touches: README, `docs/`, `skills/heaptide/SKILL.md` for anything
   in `report --json`.

Keep pull requests to one concern. Match the style of the file you edit; new files carry the
SPDX header.

Sign off every commit (`git commit -s`): the `Signed-off-by` line certifies the
[Developer Certificate of Origin](https://developercertificate.org/), and your work is licensed
under Apache-2.0, the project's license. Run `tools/publish-check.sh` before opening the pull
request. Participation follows the [Code of Conduct](CODE_OF_CONDUCT.md).
