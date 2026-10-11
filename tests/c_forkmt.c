// 8 threads replace blocks in shared slots (malloc new, swap in, free old: every slot always
// holds a live block) while main forks 200 times. Each child leaks one 200000 B block at lose_child
// and frees every slot: its report must show exactly that leak and no negative site, whatever
// the threads were doing at the fork. Exit status = number of children that did not exit 0.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#define T 8
#define S 256
static _Atomic(void *) slot[T][S];
static atomic_int stop;
static void *worker(void *a) {
  long t = (long)a; unsigned s = t + 1;
  while (!stop) { s = s * 1103515245 + 12345; free(atomic_exchange(&slot[t][s % S], malloc(16 + (s >> 8) % 512))); }
  return 0;
}
// 200000 B: above the mmap threshold, so a fresh mapping no dead worker's stack can hold a stale
// pointer to (with 40 B, glibc on Ubuntu reused a chunk a worker had just freed: reachable)
__attribute__((noinline)) static void lose_child(void) { void *volatile p = malloc(200000); (void)p; }
int main(void) {
  pthread_t th[T];
  for (long t = 0; t < T; t++) { for (int i = 0; i < S; i++) slot[t][i] = malloc(16); pthread_create(&th[t], 0, worker, (void *)t); }
  int bad = 0;
  for (int i = 0; i < 200; i++) {
    pid_t p = fork();
    if (!p) { lose_child(); for (int t = 0; t < T; t++) for (int k = 0; k < S; k++) free(slot[t][k]); exit(0); }
    int st; waitpid(p, &st, 0); bad += !WIFEXITED(st) || WEXITSTATUS(st);
  }
  stop = 1;
  for (int t = 0; t < T; t++) pthread_join(th[t], 0);
  for (int t = 0; t < T; t++) for (int k = 0; k < S; k++) free(slot[t][k]);
  return bad;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
