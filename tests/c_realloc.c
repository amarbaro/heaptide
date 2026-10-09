#include <stdint.h>
#include <stdlib.h>
int main(void) { void *p = malloc(12345); if (realloc(p, SIZE_MAX / 2)) return 1; p = 0; return 0; }
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
