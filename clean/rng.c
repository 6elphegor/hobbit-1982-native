/* The clean edition: random numbers.
 *
 * The original mixed bytes read from all over memory into its numbers; this
 * is a proper generator, a 16-bit xorshift, with the original's interface:
 * the same range and scaling, and never the same number twice in a row.
 * Its state is kept where the original kept its pointer, so it is saved
 * and restored with the rest of the game state, and it starts from the
 * game's seed. */
#include "rng.h"

#include <stddef.h>

/* One byte from the generator. */
static uint8_t next_byte(void) {
  uint16_t x = (uint16_t)(mem[V_RANDOM_STATE] << 8 | mem[V_RANDOM_STATE + 1]);
  if (x == 0) x = (uint16_t)(0xACE1 ^ mem[V_RANDOM] ^ mem[V_RANDOM] << 8);
  x ^= (uint16_t)(x << 7);
  x ^= (uint16_t)(x >> 9);
  x ^= (uint16_t)(x << 8);
  mem[V_RANDOM_STATE] = x >> 8;
  mem[V_RANDOM_STATE + 1] = (uint8_t)x;
  return (uint8_t)(x ^ x >> 8);
}

/* A byte that is not the last one, halved until it is at most 2n (or
 * 255): what random_spread(n) adds -n to. */
static uint8_t scaled_byte(uint8_t n) {
  uint8_t limit = n >= 0x80 ? 0xFF : (uint8_t)(n * 2);
  uint8_t v;
  do v = next_byte();
  while (v == mem[V_RANDOM]);
  mem[V_RANDOM] = v;
  while (v > limit) v >>= 1;
  return v;
}

int8_t random_spread(uint8_t n) { return (int8_t)(uint8_t)(scaled_byte(n) - n); }

uint8_t random_upto(uint8_t n) {
  uint8_t v = (uint8_t)random_spread(n);
  return v & 0x80 ? (uint8_t)-v : v;
}

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ---------- adapters (for the faithful callers, while they remain) ---------- */

/* $9CA8 CalcRandom: A in, A out, flags as the SUB that ends it. */
static void a_calc_random(Cpu *c) {
  uint8_t n = c->a;
  c->a = op_sub(c, scaled_byte(n), n, 0);
}

/* $9C9F GetRandomNum: CalcRandom, made positive (BIT 7,A; NEG). */
static void a_get_random(Cpu *c) {
  a_calc_random(c);
  if (op_bit(c, 7, c->a)) op_neg(c);
}

const CleanRoutine rng_clean[] = {
    {0x9CA8, "random_spread", a_calc_random, OUT_REGS | OUT_ZF | OUT_CF | OUT_SF | OUT_PF},
    {0x9C9F, "random_upto", a_get_random, OUT_REGS | OUT_ZF | OUT_CF | OUT_SF | OUT_PF},
    {0, NULL, NULL, 0},
};

const CleanScratch rng_scratch[] = {
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
