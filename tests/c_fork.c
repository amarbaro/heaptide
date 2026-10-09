#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
struct node { struct node *next; } *root;
void *lose_parent(void) { return malloc(64); }
void *lose_child(void) { return malloc(32); }
int main(void) {
  root = malloc(sizeof *root);
  lose_parent();
  if (fork() == 0) {
    root->next = malloc(48);  // reachable only through a block the parent allocated
    lose_child();
    exit(0);
  }
  wait(0);
  return 0;
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs (header at the end: the tests check line numbers)
