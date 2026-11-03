// SPDX-License-Identifier: Apache-2.0
// 64 MiB live at a named site, then "ready" and a long sleep: the test kills it with SIGKILL, and
// the heaptide wrapper must still write a report naming the site (from the /dev/shm tables);
// with an argument it leaves through _exit() at once instead
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
__attribute__((noinline)) static void *big_alloc(void) { void *p = malloc(64 << 20); memset(p, 1, 64 << 20); return p; }
int main(int argc, char **argv) {
  void *volatile keep = big_alloc();
  (void)keep;
  if (argc > 1) _exit(0);  // "exit": leaves without atexit, as dash does
  printf("ready\n"); fflush(stdout);
  sleep(60);
  return 0;
}
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
