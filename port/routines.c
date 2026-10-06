#include "routines.h"

#include <stdio.h>
#include <stdlib.h>

static const PortRoutine *const tables[] = {
    parser_routines,   sentence_routines, executor_routines, actions1_routines,
    actions2_routines, objects_routines,  actions3_routines, events_routines,
    text_routines,     io_routines,       drawing_routines,  meta_routines,
    main_routines,
};

bool port_clean_rng;

#define MAX_ROUTINES 1024

static PortRoutine all[MAX_ROUTINES + 1];
static const PortRoutine *by_addr[0x10000];

const PortRoutine *port_all(void) {
  static bool joined;
  if (!joined) {
    int n = 0;
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++)
      for (const PortRoutine *r = tables[t]; r->fn; r++) {
        if (n == MAX_ROUTINES) {
          fprintf(stderr, "too many ported routines\n");
          exit(2);
        }
        all[n++] = *r;
      }
    for (int i = 0; i < n; i++) {
      if (by_addr[all[i].addr]) {
        fprintf(stderr, "two ported routines at $%04X: %s and %s\n", all[i].addr,
                by_addr[all[i].addr]->name, all[i].name);
        exit(2);
      }
      by_addr[all[i].addr] = &all[i];
    }
    joined = true;
  }
  return all;
}

const PortRoutine *port_find(uint16_t addr) {
  port_all();
  return by_addr[addr];
}
