/* Random numbers (clean edition). */
#ifndef HOBBIT_CLEAN_RNG_H
#define HOBBIT_CLEAN_RNG_H

#include "clean.h"

/* A random number from about -n to n: a byte, never the same as the last
 * one, halved until it is at most 2n (or 255), less n. */
int8_t random_spread(uint8_t n);

/* random_spread, made positive: 0 to n. */
uint8_t random_upto(uint8_t n);

#endif
