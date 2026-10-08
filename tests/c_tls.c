// Planted: three workers block while main exits. One holds the only pointer to a 4096 B block in
// a stack slot, one the only pointer to a 2048 B block in a callee-saved register (-O2), one
// blocks every signal (cannot be parked: its SP comes from /proc) and holds a 1024 B block in a
// stack slot. All reachable (thread roots), so direct = 0 (LSan agrees).
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
static volatile int quit;
static void *on_stack(void *a) {
  void *volatile p = malloc(4096);
  while (!quit) pause();
  return a == p ? a : 0;
}
static void *in_reg(void *a) {
  void *p = malloc(2048);
  __asm__ volatile("" : "+r"(p));
  while (!quit) pause();
  __asm__ volatile("" : "+r"(p));
  return a == p ? a : 0;
}
static void *masked(void *a) {
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, 0);
  void *volatile p = malloc(1024);
  while (!quit) sleep(1);
  return a == p ? a : 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, on_stack, 0);
  pthread_create(&t, 0, in_reg, 0);
  pthread_create(&t, 0, masked, 0);
  usleep(100000);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
