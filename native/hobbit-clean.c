/* hobbit-clean: The Hobbit, the clean edition (clean/) as a native
 * program: no Z80 and no faithful port, the game's data from your tape.
 *
 *   hobbit-clean [--rom FILE] [--scale N] [--seed N] [--original-bugs] TAPE.tzx
 *
 * Keys: native/window.c. SAVE and LOAD use hobbit-save.tap. The game's
 * own crashes and infinite loops (it has some) are kept: the window then
 * shows the screen as it was until it is closed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../clean/data.h"
#include "../ref/cleanhost.h"
#include "../ref/spectrum.h"
#include "window.h"

static Spectrum s;

static void usage(void) {
  fprintf(stderr, "usage: hobbit-clean [--rom FILE] [--scale N] [--seed N] [--original-bugs] TAPE.tzx\n");
  exit(2);
}

int main(int argc, char **argv) {
  const char *tape = NULL, *rom = NULL;
  int scale = 3, seed = -1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = atoi(argv[++i]) & 0xFF;
    else if (!strcmp(argv[i], "--original-bugs")) original_bugs = true;
    else if (argv[i][0] != '-' && !tape) tape = argv[i];
    else usage();
  }
  if (!tape) usage();
  char err[256];
  if (spec_init(&s, tape, rom, err, sizeof err) != 0) {
    fprintf(stderr, "hobbit-clean: %s\n", err);
    return 1;
  }
  s.seed = seed;
  s.on_read = window_on_read;
  if (!window_open(&s, scale)) return 1;
  if (setjmp(window_quit) == 0) {
    uint16_t addr;
    switch (clean_run(&s, 0, &addr)) {
    case CLEAN_CRASHED:
      fprintf(stderr, "hobbit-clean: the game crashed (it went to $%04X, which is not code); the original "
                      "would reset the Spectrum here\n", addr);
      window_wait();
      break;
    case CLEAN_HUNG:
      fprintf(stderr, "hobbit-clean: the game has stopped (the original loops for ever here)\n");
      window_wait();
      break;
    default: break;
    }
  }
  window_close();
  return 0;
}
