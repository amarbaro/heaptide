// A snapshot must not end the program's sleep (a park signal would: nanosleep returns EINTR).
// Holds 1 MiB from keep() through a local while it sleeps 2 s; exit 1 if the sleep was cut short.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
__attribute__((noinline)) static void *keep(void) { return malloc(1 << 20); }
int main(void) {
  void *volatile held = keep();
  struct timespec t0, t1, d = {2, 0};
  clock_gettime(CLOCK_MONOTONIC, &t0);
  puts("held"); fflush(stdout);
  int r = nanosleep(&d, 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  free(held);
  return r != 0 || t1.tv_sec - t0.tv_sec < 2 ? 1 : 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
