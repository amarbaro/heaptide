// A PartitionAlloc-shaped allocator: exported malloc/free under one non-recursive (error-checking)
// lock, and a large allocation maps memory with mmap + madvise while holding it. 64 TLS keys first,
// as a big program has (libunwind's key then needs a glibc key block, calloc'd on a thread's first
// unwind). Threads started after heaptide arrives (attach: HEAPTIDE_ATTACH; run: at once) allocate
// large and small blocks. A re-entry of malloc under its own lock exits 7 (a real allocator would
// sleep on its futex forever: Vivaldi did); prints "survived" and exits 0 otherwise.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
static pthread_mutex_t lk;
static void *(*lm)(size_t);
static void (*lf)(void *);
static void init(void) {
  if (lm) return;
  lm = dlsym(RTLD_NEXT, "malloc"); lf = dlsym(RTLD_NEXT, "free");
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK); pthread_mutex_init(&lk, &a);
}
static void take(void) {
  if (pthread_mutex_lock(&lk) == EDEADLK) { static const char m[] = "SELF-DEADLOCK: malloc re-entered under its own lock\n"; write(2, m, sizeof m - 1); _exit(7); }
}
void *malloc(size_t n) {
  init(); take();
  size_t *p;
  if (n >= 65536) {
    p = mmap(0, n + 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = 0; else madvise(p, n + 16, MADV_WILLNEED), p[0] = 2;
  } else if ((p = lm(n + 16))) p[0] = 1;
  if (p) p[1] = n;
  pthread_mutex_unlock(&lk);
  return p ? p + 2 : 0;
}
void free(void *q) {
  if (!q) return;
  init(); take();
  size_t *p = (size_t *)q - 2;
  if (p[0] == 2) munmap(p, p[1] + 16); else if (p[0] == 1) lf(p); else { write(2, "BAD FREE\n", 9); _exit(6); }
  pthread_mutex_unlock(&lk);
}
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void *realloc(void *q, size_t n) {
  void *p = malloc(n);
  if (q && p) { size_t o = ((size_t *)q)[-1]; memcpy(p, q, o < n ? o : n); free(q); }
  return p;
}
static void *work(void *x) {
  for (int i = 0; i < 50; i++) { void *b = malloc(1 << 17), *s = malloc(64); free(b); if (i & 1) free(s); }
  return x;
}
int main(int argc, char **argv) {
  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
  pthread_key_t k;
  for (int i = 0; i < 64; i++) pthread_key_create(&k, 0);
  printf("%d\n", getpid()); fflush(stdout);
  if (argc > 1) {  // "attach": wait for heaptide
    for (int t = 0; t < 400 && !getenv("HEAPTIDE_ATTACH"); t++) usleep(50000);
    usleep(300000);
  }
  for (int r = 0; r < 4; r++) {
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, work, 0);
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
  }
  puts("survived");
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
