// SPDX-License-Identifier: Apache-2.0
// 300 distinct call sites, 16 B each: under HEAPTIDE_MAXSTACKS=100 the row table fills, so the stack
// counter must stop at the limit and the shim must warn
#include <stdlib.h>
static void *keep[400];
#define A(i) __attribute__((noinline)) static void f##i(void) { keep[i] = malloc(16); }
#define T(i) A(i##0) A(i##1) A(i##2) A(i##3) A(i##4) A(i##5) A(i##6) A(i##7) A(i##8) A(i##9)
#define H(i) T(i##0) T(i##1) T(i##2) T(i##3) T(i##4) T(i##5) T(i##6) T(i##7) T(i##8) T(i##9)
H(1) H(2) H(3)
#define C(i) f##i();
#define CT(i) C(i##0) C(i##1) C(i##2) C(i##3) C(i##4) C(i##5) C(i##6) C(i##7) C(i##8) C(i##9)
#define CH(i) CT(i##0) CT(i##1) CT(i##2) CT(i##3) CT(i##4) CT(i##5) CT(i##6) CT(i##7) CT(i##8) CT(i##9)
int main(void) { CH(1) CH(2) CH(3) return 0; }
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
