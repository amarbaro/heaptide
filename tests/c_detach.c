// heaptide detach: started without heaptide (see c_attach). Once its GOT slot for malloc is heaptide's
// (attached), it loses 1000 B at lose_before on each of 20 steps and prints "leaked"; once the slot
// holds libc's malloc again (detached), it loses 1000 B at lose_after 20 times (must not be reported),
// prints "detached" and exits 0 after 2 s. Exit 3: never attached, 4: never detached.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
__attribute__((noinline)) static void lose_before(void) { void *volatile p = malloc(1000); (void)p; }
__attribute__((noinline)) static void lose_after(void) { void *volatile p = malloc(1000); (void)p; }
static void *libc;
__attribute__((noinline)) static int hooked(void) { void *(*volatile m)(size_t) = malloc; return (void *)m != libc; }
int main(void) {
  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
  libc = dlsym(RTLD_DEFAULT, "malloc");
  printf("%d\n", getpid()); fflush(stdout);
  struct timespec ms = {0, 50000000};
  for (int t = 0; t < 400 && !hooked(); t++) nanosleep(&ms, 0);
  if (!hooked()) return 3;
  nanosleep(&(struct timespec){0, 200000000}, 0);  // the constructor finishes after the slots are written
  for (int i = 0; i < 20; i++) lose_before();
  puts("leaked"); fflush(stdout);
  for (int t = 0; t < 400 && hooked(); t++) nanosleep(&ms, 0);
  if (hooked()) return 4;
  for (int i = 0; i < 20; i++) lose_after();
  puts("detached"); fflush(stdout);
  nanosleep(&(struct timespec){2, 0}, 0);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
