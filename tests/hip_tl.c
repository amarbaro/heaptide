// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// --gpu-timeline on the CPU: a fake libamdhip64 (MOCK_LIB) whose device "uses" every live block
// plus 1 MiB the runtime keeps per allocation (the fragmentation line), and a program that grows
// one site in 3 steps 300 ms apart, allocates on two streams, then frees one stream's block. TL_FORK: then forks a child.
#include <stddef.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef MOCK_LIB
static size_t used;
static int torn;  // like HIP: teardown registered at first use, any call after it aborts
static void down(void) { torn = 1; }
static void up(void) { static int u; if (torn) abort(); if (!u) u = 1, atexit(down); }
static int get(void **p, size_t n) { up(); *p = malloc(n); used += n + (1 << 20); return !*p; }
int hipMalloc(void **p, size_t n) { return get(p, n); }
int hipMallocAsync(void **p, size_t n, void *s) { (void)s; return get(p, n); }  // not via the hooked hipMalloc
int hipFree(void *p) { free(p); return 0; }
int hipFreeAsync(void *p, void *s) { (void)s; free(p); return 0; }
int hipMemGetInfo(size_t *fr, size_t *to) { up(); *to = (size_t)1 << 34; *fr = *to - used; return 0; }
#else
int hipMalloc(void **p, size_t n);
int hipMallocAsync(void **p, size_t n, void *s);
int hipFreeAsync(void *p, void *s);
static void nap(void) { struct timespec d = {0, 300000000}; nanosleep(&d, 0); }
__attribute__((noinline)) static void *grow(void) { void *p; hipMalloc(&p, 4 << 20); return p; }
int main(void) {
  void *keep[3], *a, *b;
  for (int i = 0; i < 3; i++) keep[i] = grow(), nap();  // 4, 8, 12 MiB
  hipMallocAsync(&a, 2 << 20, (void *)0x1000);
  hipMallocAsync(&b, 2 << 20, (void *)0x2000);
  nap();
  hipFreeAsync(b, (void *)0x2000);
  nap();
  if (getenv("TL_FORK")) {  // a child traced from here: its timeline must not carry the parent's samples
    pid_t c = fork();
    if (!c) return 0;
    waitpid(c, 0, 0);
  }
  return keep[0] && a ? 0 : 1;
}
#endif
