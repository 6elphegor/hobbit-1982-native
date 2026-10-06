/* Running ported routines inside the reference machine.
 *
 * HYBRID_REPLACE: when the game calls a ported routine, run the C version
 *   instead of the original.
 * HYBRID_CHECK: run both, from the same state, on every call, and report
 *   any difference in the results (the routine's output registers, SP,
 *   memory, what it printed, and keys consumed). The game then carries on
 *   from the original's result, so one mismatch does not derail the run.
 *
 * Ported routines that call original code do so through the machine; in
 * check mode, ported routines reached from there run natively. */
#ifndef HOBBIT_HYBRID_H
#define HOBBIT_HYBRID_H

#include <stdio.h>

#include "spectrum.h"

/* HYBRID_CHECK_CLEAN: for each routine with a clean version (clean/), run
 * the faithful port's and the clean one from the same state and compare
 * (game memory except declared scratch, the registers its callers use,
 * output, keys); routines without one run natively. Needs the clean
 * random number generator. */
typedef enum { HYBRID_OFF, HYBRID_REPLACE, HYBRID_CHECK, HYBRID_CHECK_CLEAN } HybridMode;

/* In HYBRID_REPLACE, run the clean version of a routine where there is
 * one. */
void hybrid_use_clean(bool on);
int hybrid_clean_skip(const char *list);
/* In --check-clean, a trial run (with other inputs) that never ends is
 * abandoned (no return); false if there is none going on. */
bool hybrid_abandon_trial(void);

/* HYBRID_CHECK_CLEAN: compare only the first limit calls of each routine
 * (0: all), and run each of those mutate more times with other inputs. */
void hybrid_check_sampling(unsigned long limit, int mutate);

/* only: NULL for all ported routines, or a comma-separated list of
 * addresses (hex, e.g. "6E97,6DD6"). Returns -1 on a bad list. */
int hybrid_attach(Spectrum *s, HybridMode mode, const char *only);

/* In check mode, save the machine state before the first mismatching call
 * of each routine to path: 64K of memory, then A B C D E H L IX IY SP PC. */
void hybrid_dump_mismatch(const char *path);

/* In check mode, also compare the stack below the routine's return
 * address (scratch the routine leaves behind: the random number generator
 * reads it too, so it matters in the long run). */
void hybrid_strict_stack(bool on);

/* Per-routine call counts and mismatches. Returns the mismatch total. */
unsigned long hybrid_report(FILE *f);

#endif
