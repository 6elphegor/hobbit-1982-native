/* The executor (clean edition): carrying out the sentences the parser
 * built, and the orders characters were given. */
#ifndef HOBBIT_CLEAN_EXECUTOR_H
#define HOBBIT_CLEAN_EXECUTOR_H

#include "clean.h"

/* Carry out the sentences parsed into the records at $B9C8, each with its
 * game turn ($7960). */
void execute_sentences(void);

/* Try the current action (V_ACTION on V_OBJECT1/V_OBJECT2) again for the
 * current actor, as a character's script does: true if it was done
 * ($7AF5). The executor's own state is left as it was found. */
bool retry_action(void);

/* Of the orders just said (SPEECH_COUNT of them), give the first n (at
 * most) to V_OBJECT1 and cancel the rest ($7EBA). */
void give_orders(uint8_t n);

/* The speech slot holding an order for the current actor, or the address
 * after the last slot if there is none ($7EFF). */
uint16_t find_orders(bool *found);

/* Whether the current actor has an order waiting ($7F10). */
bool has_orders(void);

/* The current actor carries out its order ($7F1A): its slot is taken off
 * it; with run false, only that, and *record is set to the order's
 * sentence record (and true returned). Otherwise the order is carried out
 * as a quoted sentence: true if it was done; if not, the actor's other
 * orders are dropped too. */
bool obey_orders(bool run, uint16_t *record);

/* Cancel every order given to character who ($7F60). */
void drop_orders(uint8_t who);

#endif
