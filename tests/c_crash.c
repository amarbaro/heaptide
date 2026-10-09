#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void own(int s) { (void)s; _exit(9); }
static void scrub(void) { volatile char b[8192]; memset((char *)b, 0, sizeof b); }  // no stale copy below main
static int deep(int n) { volatile char b[4096]; b[0] = n; return deep(n + 1) + b[0]; }  // overflows
static void *overflow(void *a) { free(malloc(16)); return (void *)(long)deep(a != 0); }  // allocates first, as threads do
int main(int argc, char **argv) {
  void *volatile p = malloc(4096); p = 0;  // lost
  scrub();
  if (argc > 1 && !strcmp(argv[1], "abort")) abort();
  pthread_t t;
  if (argc > 1 && !strcmp(argv[1], "overflow")) pthread_create(&t, 0, overflow, 0), pthread_join(t, 0);
  if (argc > 1 && !strcmp(argv[1], "own")) signal(SIGSEGV, own);  // a program's own handler is left alone
  *(volatile int *)p = 1;
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
