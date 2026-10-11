// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
// 65536 call sites, one leaked 16 B block each: the report's ranking cost on a huge site count.
#include <stdlib.h>
void *volatile sink;
#define A sink = malloc(16);
#define B A A A A A A A A A A A A A A A A
#define C B B B B B B B B B B B B B B B B
#define D C C C C C C C C C C C C C C C C
int main(void) { D D D D D D D D D D D D D D D D return 0; }
