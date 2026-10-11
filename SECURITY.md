# Security

heaptide is preloaded into other programs and reads their memory, so security bugs in it matter.

## Report a vulnerability

Do not open a public issue. Use GitHub's private reporting: the **Report a vulnerability** button
on the [Security advisories](https://github.com/amarbaro/heaptide/security/advisories/new) page.
Include the version, the command, and how to reproduce it.

You will get a reply within 7 days. A fix ships as a patch release with an advisory that credits
you, unless you ask not to be named.

## What heaptide does to a process

- `heaptide run` preloads `libheaptide.so` into the program it starts: it reads that program's
  memory (the reachability scan) and writes report files to `--out`.
- `heaptide attach PID` uses gdb (ptrace) to load the same library into a running process. That
  needs the right to ptrace it: under Yama `ptrace_scope` 1 root, or a process that allowed it with
  `prctl(PR_SET_PTRACER, ...)`. Report files are then written by the target with the target's
  rights, to the directory the attaching user named. To route allocations through its hooks, the
  library rewrites the process's GOT slots for the allocation functions; read-only (RELRO) pages
  are made writable for that write and read-only again right after (tested in `tests/run.sh`).
- The report reads the `.bin` files and, under `--py`, CPython's `/tmp/perf-<pid>.map`; it treats
  both as untrusted input (bounded counts, rejected files exit 2).

## Supported versions

Only the latest release gets fixes.
