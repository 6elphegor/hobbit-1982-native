/* The table of ported routines, by the address of the original. */
#ifndef HOBBIT_ROUTINES_H
#define HOBBIT_ROUTINES_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

/* Results a routine leaves for its callers, compared in check mode.
 * Memory and SP are always compared. */
enum {
  OUT_A = 1 << 0, OUT_B = 1 << 1, OUT_C = 1 << 2, OUT_D = 1 << 3, OUT_E = 1 << 4,
  OUT_H = 1 << 5, OUT_L = 1 << 6, OUT_IX = 1 << 7, OUT_IY = 1 << 8,
  OUT_ZF = 1 << 9, OUT_CF = 1 << 10, OUT_SF = 1 << 11, OUT_PF = 1 << 12,
  OUT_ALT = 1 << 13, /* BC' DE' HL' */
  OUT_FLAGS = 1 << 14, /* H, N and the undocumented bits 5 and 3 */
  OUT_BC = OUT_B | OUT_C, OUT_DE = OUT_D | OUT_E, OUT_HL = OUT_H | OUT_L,
  OUT_REGS = OUT_A | OUT_BC | OUT_DE | OUT_HL | OUT_IX | OUT_IY,
};

/* Flags. PORT_TOP: a top-level entry (the game's start, the restart
 * point): it never returns, and reaching it from anywhere abandons
 * everything that was running, as the original's JP to it (and its LD SP)
 * does. It is not compared in check mode; the transcripts check it. */
enum { PORT_TOP = 1 };

typedef struct {
  uint16_t addr;
  const char *name;
  void (*fn)(Cpu *c);
  unsigned outputs;
  unsigned flags;
} PortRoutine;

/* Each port/ file has its own table, ending with {0}: a file's routines
 * are listed in that file, and routines.c joins the tables. */
extern const PortRoutine parser_routines[], sentence_routines[], executor_routines[],
    actions1_routines[], actions2_routines[], objects_routines[], actions3_routines[],
    text_routines[], events_routines[], io_routines[], drawing_routines[], meta_routines[],
    main_routines[];

/* Which random number generator CalcRandom ($9CA8) uses: false, the
 * original's (it mixes in bytes read from all over memory, so matching it
 * needs every byte of memory, stack included, to match); true, a proper
 * generator with the same interface (range, no repeats). */
extern bool port_clean_rng;

/* All ported routines, as one array ending with {0}. */
const PortRoutine *port_all(void);

/* The ported routine at addr, or NULL. */
const PortRoutine *port_find(uint16_t addr);

#endif
