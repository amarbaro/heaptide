// heaptide attach: started without heaptide, lets any process ptrace it (Yama 1 allows that), and
// holds a table allocated before attach. Once attached (HEAPTIDE_ATTACH appears in its environment)
// it fills the table, blocks reachable only through pre-attach memory (not leaks), and loses
// 1000 B at lose_after on each of 20 steps (leaks). Prints its pid, exits 0.
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
struct table { void *slot[64]; };
static struct table **holder;  // global -> pre-attach block -> table: the table is never tracked
__attribute__((noinline)) static void lose_after(void) { void *volatile p = malloc(1000); (void)p; }
int main(void) {
  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
  holder = malloc(sizeof *holder); *holder = calloc(1, sizeof **holder);
  printf("%d\n", getpid()); fflush(stdout);
  struct timespec ms = {0, 50000000};
  for (int t = 0; t < 400 && !getenv("HEAPTIDE_ATTACH"); t++) nanosleep(&ms, 0);
  for (int i = 0; i < 20; i++) { (*holder)->slot[i] = malloc(256); lose_after(); nanosleep(&ms, 0); }
  return getenv("HEAPTIDE_ATTACH") ? 0 : 3;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
