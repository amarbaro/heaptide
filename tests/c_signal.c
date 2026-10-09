#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
static volatile int got;
static void own(int s) { (void)s; got = 1; }
int main(int argc, char **argv) {
  void *volatile p = malloc(4096); p = 0;  // lost
  if (argc > 1) signal(SIGTERM, own);      // a program with its own handler keeps it
  while (!got) pause();
  return 7;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
