/* The clean edition: what the game asks of the machine it runs on.
 *
 * The host supplies these: ref/hybrid.c while the faithful port is still
 * around (through the Cpu of the routine running), and a host of its own
 * for the clean edition alone. Addresses passed as "at" are the
 * original's: a test machine tells by them what kind of wait or event it
 * is (the typist answers a picture pause, the transcript takes the
 * characters printed). */
#ifndef HOBBIT_CLEAN_PLATFORM_H
#define HOBBIT_CLEAN_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

/* Read an input port (the keyboard's half-rows at $xxFE, the printer at
 * $FB), as the original's IN at address at. */
uint8_t device_in(uint16_t port, uint16_t at);

/* Write an output port (the border at $FE, the printer at $FB). */
void device_out(uint16_t port, uint8_t v);

/* The game is where the original would be at addr, with A = a: the host
 * may watch it there (the characters printed; the seed, which a test
 * machine may then replace in memory). */
void device_at(uint16_t addr, uint8_t a);

/* A byte that differs from run to run (the original read the Z80's
 * refresh register). */
uint8_t device_random_byte(void);

/* A tape block with flag $FF, len bytes at start: saved, or loaded (or,
 * with verify, compared with memory). True if all went well. */
bool device_tape(bool save, uint16_t start, uint16_t len, bool verify);

/* Start the game again from the restart ($6C27): every death, QUIT and
 * tape error ends here. Never returns. */
_Noreturn void device_restart(void);

/* The original jumps to addr, where there is no game code (the ROM, or
 * data): a crash, which on a Spectrum ends in a reset. The quirks that
 * do this (a token read from past the end of the line, a quote left open
 * when a line is repeated) are kept, as everything the game does is.
 * Never returns. */
_Noreturn void device_crash(uint16_t addr);

/* The original loops for ever here, never asking for input again (a
 * quirk kept, as everything the game does is). Never returns. */
_Noreturn void device_hang(void);

#endif
