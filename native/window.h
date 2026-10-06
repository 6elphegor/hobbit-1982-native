/* The window of the native programs (hobbit, hobbit-clean): the
 * Spectrum's screen drawn from the game's display memory, the keyboard,
 * and the original's speed. */
#ifndef HOBBIT_WINDOW_H
#define HOBBIT_WINDOW_H

#include <setjmp.h>
#include <stdbool.h>

#include "../ref/spectrum.h"

/* Esc or closing the window jumps here (with 1). */
extern jmp_buf window_quit;

/* Open the window, scale times the Spectrum's screen; false (after saying
 * why) if it cannot be. It reads s's keyboard and screen from then on:
 * window_on_read is to be s's on_read. */
bool window_open(Spectrum *s, int scale);
/* Every keyboard read: keep to the original's speed, take input, and
 * draw the screen 50 times a second. */
void window_on_read(Spectrum *s);
/* Draw the screen now. */
void window_draw(void);
/* Show the screen until the window is closed (or Esc). */
void window_wait(void);
void window_close(void);

#endif
