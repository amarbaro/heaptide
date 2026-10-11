// The program owns SIGRTMAX-2: its handler must survive a snapshot (heaptide parks threads with
// a free real-time signal instead). A worker thread makes the snapshot park someone. Prints
// "held" once ready, waits 3 s for the snapshot, raises the signal: exit 0 if its handler ran.
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
static volatile sig_atomic_t got;
static void mine(int s) { (void)s; got = 1; }
static void *idle(void *a) { (void)a; for (;;) pause(); }
int main(void) {
  signal(SIGRTMAX - 2, mine);
  pthread_t t; pthread_create(&t, 0, idle, 0);
  printf("held\n"); fflush(stdout);
  sleep(3);
  raise(SIGRTMAX - 2);
  return got ? 0 : 1;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
