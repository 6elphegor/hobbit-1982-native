/* The clean edition: the game in ordinary C, with a proper random number
 * generator. While the faithful port is being replaced, each clean routine
 * stands in for a faithful one through an adapter (it unpacks the Z80
 * registers into arguments, and its results back into registers), and
 * clean code reaches routines that are not clean yet through bridge_call.
 * When everything is clean, the adapters and the bridge go. See
 * docs/CLEAN.md. */
#ifndef HOBBIT_CLEAN_H
#define HOBBIT_CLEAN_H

#include <stdbool.h>
#include <stdint.h>

#include "data.h"

#ifdef CLEAN_ADAPTERS /* the faithful port's side: not in the clean edition alone */
#include "../port/cpu.h"
#include "../port/routines.h"

/* ---------- the table of clean routines ---------- */

typedef struct {
  uint16_t addr;              /* the faithful routine it replaces */
  const char *name;
  void (*adapter)(Cpu *c);    /* registers in, call the clean code, registers out */
  unsigned outputs;           /* registers and flags its callers use (OUT_ in routines.h) */
  unsigned fixed;             /* inputs the check does not mutate: ones every caller sets to
                               * one of a few constants (which the adapter decodes), or keeps
                               * in a range the original never leaves; say why at the adapter */
} CleanRoutine;

/* Memory the clean code need not keep as the original does: scratch
 * bytes that nothing reads after the routine that wrote them. Each module
 * lists its own, with the reason. */
typedef struct {
  uint16_t lo, hi; /* inclusive */
  const char *why;
} CleanScratch;

/* Each clean/ file has a table of each, ending with {0}. */
#define CLEAN_MODULES(X) \
  X(rng) X(objects) X(text) X(screen) X(parser) X(executor) X(actions) X(characters) X(drawing) X(game)
#define DECLARE(m) extern const CleanRoutine m##_clean[]; extern const CleanScratch m##_scratch[];
CLEAN_MODULES(DECLARE)
#undef DECLARE

const CleanRoutine *clean_all(void);
const CleanRoutine *clean_find(uint16_t addr);
bool clean_is_scratch(uint16_t addr);

/* ---------- reaching code that is not clean yet ---------- */

typedef struct {
  uint8_t a, b, c, d, e, h, l;
  uint16_t ix, iy;
  bool zf, cf;
} Regs;

/* Run the routine at addr (clean, through its adapter, if it has one;
 * otherwise the faithful port's) with these registers, and read them back
 * after. Only for use while the conversion is going on. */
void bridge_call(uint16_t addr, Regs *r);

/* The Cpu of the routine running now (for device access from clean code
 * while the host machine is still there). */
Cpu *bridge_cpu(void);

#endif /* CLEAN_ADAPTERS */

#endif
