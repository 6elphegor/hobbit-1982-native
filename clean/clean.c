#include "clean.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t *mem;
bool original_bugs;

#ifdef CLEAN_ADAPTERS /* the table of clean routines, for the faithful port's host */

#define TABLES(m) m##_clean,
static const CleanRoutine *const tables[] = {CLEAN_MODULES(TABLES)};
#undef TABLES
#define SCRATCH(m) m##_scratch,
static const CleanScratch *const scratch[] = {CLEAN_MODULES(SCRATCH)};
#undef SCRATCH

#define MAX 1024
static CleanRoutine all[MAX + 1];
static const CleanRoutine *by_addr[0x10000];
static bool is_scratch[0x10000];

const CleanRoutine *clean_all(void) {
  static bool joined;
  if (!joined) {
    int n = 0;
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++)
      for (const CleanRoutine *r = tables[t]; r->adapter; r++) {
        if (by_addr[r->addr]) {
          fprintf(stderr, "two clean routines at $%04X: %s and %s\n", r->addr, by_addr[r->addr]->name, r->name);
          exit(2);
        }
        all[n] = *r;
        by_addr[r->addr] = &all[n++];
      }
    for (size_t t = 0; t < sizeof scratch / sizeof scratch[0]; t++)
      for (const CleanScratch *x = scratch[t]; x->why; x++)
        for (unsigned a = x->lo; a <= x->hi; a++) is_scratch[a] = true;
    joined = true;
  }
  return all;
}

const CleanRoutine *clean_find(uint16_t addr) {
  clean_all();
  return by_addr[addr];
}

bool clean_is_scratch(uint16_t addr) {
  clean_all();
  return is_scratch[addr];
}

#endif /* CLEAN_ADAPTERS */
