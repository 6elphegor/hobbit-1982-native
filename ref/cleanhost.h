/* The host of the clean edition alone (clean/platform.h), on the test
 * machine's Spectrum (ref/spectrum.c) for its memory, keyboard, typist,
 * tape file and screen: nothing runs on its Z80. For hobbit-clean and the
 * clean edition's window (native/hobbit-clean.c). */
#ifndef HOBBIT_CLEANHOST_H
#define HOBBIT_CLEANHOST_H

#include <stdint.h>

#include "spectrum.h"

/* How a run ended. */
typedef enum {
  CLEAN_STOPPED, /* spec_stop: the script ran out, or the front end stopped it */
  CLEAN_HUNG,    /* the game stopped asking for input */
  CLEAN_CRASHED, /* the game jumped where there is no code */
} CleanEnd;

/* Play the clean edition on s (set up from the tape) until the run ends,
 * game after game. idle_seconds: a hang is that much processor time with
 * no keyboard read (0: only the hangs the game reports itself, which are
 * the original's own infinite loops it knows of). *crash_addr: where a
 * crash went. */
CleanEnd clean_run(Spectrum *s, double idle_seconds, uint16_t *crash_addr);

/* If set, told of every place the game says it is at (device_at), after
 * the machine: a front end may watch the game there (hobbit-clean's video
 * takes a frame as a picture is drawn, at $7FBC). */
extern void (*clean_watch)(Spectrum *s, uint16_t addr);

#endif
