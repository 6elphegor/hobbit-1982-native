/* hobbit: The Hobbit as a native program.
 *
 *   hobbit [--rom FILE] [--scale N] [--seed N] TAPE.tzx
 *
 * Runs the ported game (port/) with no Z80: the game's own memory map,
 * loaded from your tape, with the screen drawn from its display memory.
 * Keys: native/window.c. SAVE and LOAD use hobbit-save.tap. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../port/routines.h"
#include "../ref/hybrid.h"
#include "../ref/spectrum.h"
#include "window.h"

static Spectrum s;

static void no_native_code(Spectrum *m, uint16_t pc) {
  (void)m;
  fprintf(stderr, "hobbit: the game crashed (it went to $%04X, which is not code); the original "
                  "would reset the Spectrum here\n", pc);
  longjmp(window_quit, 2);
}

static void usage(void) {
  fprintf(stderr, "usage: hobbit [--rom FILE] [--scale N] [--seed N] [--rng original|clean] TAPE.tzx\n");
  exit(2);
}

int main(int argc, char **argv) {
  const char *tape = NULL, *rom = NULL;
  int scale = 3, seed = -1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = atoi(argv[++i]) & 0xFF;
    else if (!strcmp(argv[i], "--rng") && i + 1 < argc) port_clean_rng = !strcmp(argv[++i], "clean");
    else if (argv[i][0] != '-' && !tape) tape = argv[i];
    else usage();
  }
  if (!tape) usage();
  char err[256];
  if (spec_init(&s, tape, rom, err, sizeof err) != 0) {
    fprintf(stderr, "hobbit: %s\n", err);
    return 1;
  }
  s.seed = seed;
  if (hybrid_attach(&s, HYBRID_REPLACE, NULL) != 0) return 1;
  s.no_cpu = true;
  s.on_missing = no_native_code;
  s.on_read = window_on_read;
  if (!window_open(&s, scale)) return 1;

  /* The game never returns; Esc or closing the window jumps out. */
  jmp_buf stop;
  s.stop_jmp = &stop;
  int how = setjmp(window_quit);
  if (how == 0 && setjmp(stop) == 0)
    for (;;) spec_step(&s);
  if (how == 2) window_wait();
  window_close();
  return 0;
}
