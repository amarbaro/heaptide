#define _GNU_SOURCE
#include <link.h>
#include <pthread.h>
#include <unistd.h>
// a thread still inside dl_iterate_phdr at exit holds the loader lock
static int cb(struct dl_phdr_info *i, size_t s, void *d) { (void)i; (void)s; (void)d; sleep(3); return 1; }
static void *t(void *a) { (void)a; dl_iterate_phdr(cb, 0); for (;;) pause(); }
int main(void) { pthread_t th; pthread_create(&th, 0, t, 0); usleep(100000); return 0; }
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
