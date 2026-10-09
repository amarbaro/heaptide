#include <stdlib.h>
char *mid;  // the only pointer to its block points 16 bytes in
struct node { char pad[24]; struct node *next; };
void *lose(void) {
  struct node *n = malloc(sizeof *n);
  n->next = (struct node *)((char *)malloc(64) + 8);  // indirect, reached only through an interior pointer
  return n;
}
int main(void) {
  mid = (char *)malloc(64) + 16;
  lose();
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
