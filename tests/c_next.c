#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
int main(void) {
  void *(*m)(unsigned long) = (void *(*)(unsigned long))dlsym(RTLD_NEXT, "malloc");
  m(4096);  // lost: through the next malloc after the program, which is heaptide's
  return !dlsym(RTLD_NEXT, "puts");
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
