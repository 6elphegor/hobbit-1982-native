/* The interface between ported routines and the rest of the game.
 *
 * While the port is in progress, each ported routine keeps the original's
 * register interface: it takes its inputs in, and leaves its results in,
 * the Z80 registers held in Cpu, and works on the game's 64K memory map.
 * Keeping the original memory map is not just a convenience: the random
 * number generator (CalcRandom, $9CA8) reads bytes from all of it, so the
 * layout of the game's state is part of its behaviour.
 *
 * Routines that have not been ported yet are reached through cpu_call,
 * which runs the original code. When everything is ported, cpu_call goes
 * away and the routines can take plain C arguments. */
#ifndef HOBBIT_CPU_H
#define HOBBIT_CPU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Cpu Cpu;

struct Cpu {
  uint8_t a, b, c, d, e, h, l;
  uint8_t b_, c_, d_, e_, h_, l_; /* the alternate set, for EXX */
  uint16_t ix, iy, sp;
  uint8_t r; /* the refresh register (the game takes its seed from it, $6CA7) */
  bool sf, zf, hf, pf, nf, cf; /* S Z H P/V N C */
  bool yf, xf; /* the undocumented bits 5 and 3 of F (they end up in pushed AF bytes) */
  uint8_t *mem;

  /* Run the routine at addr with the current registers, until it returns
   * (to the return address pushed for it, wherever SP is then: routines
   * that take arguments from under their return address, such as some
   * messages of $72DD, are fine). If it leaves this routine's frame
   * instead (a restart after a death, say), cpu_call does not come back:
   * the ported routine is abandoned, as the original was. */
  void (*call)(Cpu *cpu, uint16_t addr);
  /* Run the CALL instruction at site (CALL nn or CALL cc,nn; a conditional
   * one tests the flags as they are, so set them as the original would) in the original
   * code: it pushes the real return address, site+3, as the original does,
   * and runs until execution comes back to site+3 (SP at or above where it
   * was: callees may take arguments off the stack). Prefer this to call:
   * the stack holds the same bytes as in the original, and the random
   * number generator reads them. Leaving the frame abandons the routine,
   * as for call. */
  void (*call_at)(Cpu *cpu, uint16_t site);
  /* Jump to original code at addr. It runs until it reaches resume (with
   * SP as it is now), and then returns true; or until it leaves the
   * ported routine's frame (returns from it, or abandons it, as a
   * restart does), and then sets returned and returns false: the ported
   * routine must then return at once. resume < 0: never. */
  bool (*jump)(Cpu *cpu, uint16_t addr, int resume);
  /* Read an I/O port. */
  uint8_t (*in)(Cpu *cpu, uint16_t port);
  /* Read an I/O port as the IN instruction at pc does: use this for the
   * keyboard, since the test typist decides when to press keys by where
   * the game is reading them. */
  uint8_t (*in_at)(Cpu *cpu, uint16_t port, uint16_t pc);
  /* Write an I/O port (the border, the ZX Printer). */
  void (*out)(Cpu *cpu, uint16_t port, uint8_t v);
  /* Say that the original would now be at addr, where the host watches
   * the game: $86A1 (a character printed in the text window), $85B8 (on
   * the input line), $8B93 (a GetKey scan), $6CAC (the seed is set). Call
   * it where your C code passes through one of these addresses itself,
   * rather than through cpu_call_at/cpu_jump or by being registered
   * there, with the registers as the original has them at that point. */
  void (*at)(Cpu *cpu, uint16_t addr);
  uint16_t frame_sp; /* SP on entry, pointing at the return address */
  bool returned;
  void *host;
  void *frame; /* the host's, for leaving a routine whose frame is gone */
};

static inline uint16_t get_bc(const Cpu *c) { return (uint16_t)(c->b << 8 | c->c); }
static inline uint16_t get_de(const Cpu *c) { return (uint16_t)(c->d << 8 | c->e); }
static inline uint16_t get_hl(const Cpu *c) { return (uint16_t)(c->h << 8 | c->l); }
static inline void set_bc(Cpu *c, uint16_t v) { c->b = v >> 8, c->c = (uint8_t)v; }
static inline void set_de(Cpu *c, uint16_t v) { c->d = v >> 8, c->e = (uint8_t)v; }
static inline void set_hl(Cpu *c, uint16_t v) { c->h = v >> 8, c->l = (uint8_t)v; }

static inline uint16_t rd16(const Cpu *c, uint16_t a) {
  return (uint16_t)(c->mem[a] | c->mem[(uint16_t)(a + 1)] << 8);
}
static inline void wr16(Cpu *c, uint16_t a, uint16_t v) {
  c->mem[a] = (uint8_t)v;
  c->mem[(uint16_t)(a + 1)] = v >> 8;
}

static inline void cpu_call(Cpu *c, uint16_t addr) { c->call(c, addr); }
static inline void cpu_call_at(Cpu *c, uint16_t site) { c->call_at(c, site); }
static inline void cpu_at(Cpu *c, uint16_t addr) { c->at(c, addr); }
static inline void cpu_tail(Cpu *c, uint16_t addr) { c->jump(c, addr, -1); }
static inline bool cpu_jump(Cpu *c, uint16_t addr, uint16_t resume) { return c->jump(c, addr, resume); }

/* The Z80 stack, for the few places where the original leaves something on
 * it across a return. */
static inline void push16(Cpu *c, uint16_t v) {
  c->sp -= 2;
  wr16(c, c->sp, v);
}
static inline uint16_t pop16(Cpu *c) {
  uint16_t v = rd16(c, c->sp);
  c->sp += 2;
  return v;
}

static inline void exx(Cpu *c) {
  uint8_t t;
#define SWAP(r) (t = c->r, c->r = c->r##_, c->r##_ = t)
  SWAP(b), SWAP(c), SWAP(d), SWAP(e), SWAP(h), SWAP(l);
#undef SWAP
}

/* Blanker ($70E2): zero B bytes from HL. Leaves HL after them, B=0, A=0. */
static inline void blank(Cpu *c) {
  c->a = 0;
  c->sf = 0, c->zf = 1, c->hf = 0, c->pf = 1, c->nf = 0, c->cf = 0, c->yf = 0, c->xf = 0;
  uint16_t hl = (uint16_t)(c->h << 8 | c->l);
  do c->mem[hl++] = 0;
  while (--c->b);
  c->h = hl >> 8, c->l = (uint8_t)hl;
}

/* LDIR. */
static inline void ldir(Cpu *c) {
  uint16_t hl = (uint16_t)(c->h << 8 | c->l), de = (uint16_t)(c->d << 8 | c->e);
  uint16_t bc = (uint16_t)(c->b << 8 | c->c);
  uint8_t v;
  do c->mem[de++] = v = c->mem[hl++];
  while (--bc);
  c->h = hl >> 8, c->l = (uint8_t)hl, c->d = de >> 8, c->e = (uint8_t)de, c->b = c->c = 0;
  c->hf = 0, c->pf = 0, c->nf = 0;
  v += c->a; /* bits 5 and 3 of F: bits 1 and 3 of the last byte + A */
  c->yf = (v >> 1) & 1, c->xf = (v >> 3) & 1;
}

/* ---------- Z80 operations, with their documented flags ----------
 * S Z H P/V N C as the Z80 sets them; the undocumented bits 3 and 5 are
 * not kept. Each takes and returns values, leaving flags in the Cpu, so
 * a translation can stay close to the original instruction by
 * instruction. */

static inline bool parity(uint8_t v) {
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return !(v & 1);
}

/* Bits 5 and 3 of F, which the Z80 copies from various places: usually
 * the result (as in the reference machine's core, third_party/z80). */
static inline void flags_xy(Cpu *c, uint8_t v) {
  c->yf = (v >> 5) & 1;
  c->xf = (v >> 3) & 1;
}

static inline void flags_szp(Cpu *c, uint8_t r) {
  c->sf = r >> 7;
  c->zf = r == 0;
  c->pf = parity(r);
  flags_xy(c, r);
}

/* F as a byte, and back: for PUSH AF / POP AF. */
static inline uint8_t cpu_f(const Cpu *c) {
  return (uint8_t)(c->sf << 7 | c->zf << 6 | c->yf << 5 | c->hf << 4 | c->xf << 3 | c->pf << 2 |
                   c->nf << 1 | c->cf);
}
static inline void cpu_set_f(Cpu *c, uint8_t f) {
  c->sf = f >> 7 & 1, c->zf = f >> 6 & 1, c->yf = f >> 5 & 1, c->hf = f >> 4 & 1;
  c->xf = f >> 3 & 1, c->pf = f >> 2 & 1, c->nf = f >> 1 & 1, c->cf = f & 1;
}

/* ADD/ADC A,v: returns the result (store it in A). */
static inline uint8_t op_add(Cpu *c, uint8_t a, uint8_t v, bool carry) {
  unsigned r = a + v + carry;
  c->hf = ((a & 0x0F) + (v & 0x0F) + carry) > 0x0F;
  c->pf = ((a ^ ~v) & (a ^ r) & 0x80) != 0;
  c->cf = r > 0xFF;
  c->nf = 0;
  c->sf = (r >> 7) & 1;
  c->zf = (uint8_t)r == 0;
  flags_xy(c, (uint8_t)r);
  return (uint8_t)r;
}

/* SUB/SBC/CP: returns a - v - borrow (CP: discard it). */
static inline uint8_t op_sub(Cpu *c, uint8_t a, uint8_t v, bool borrow) {
  int r = a - v - borrow;
  c->hf = (a & 0x0F) < (v & 0x0F) + borrow;
  c->pf = ((a ^ v) & (a ^ (uint8_t)r) & 0x80) != 0;
  c->cf = r < 0;
  c->nf = 1;
  c->sf = ((uint8_t)r) >> 7;
  c->zf = (uint8_t)r == 0;
  flags_xy(c, (uint8_t)r);
  return (uint8_t)r;
}
/* CP: bits 5 and 3 come from the operand, not the result. */
static inline void op_cp(Cpu *c, uint8_t v) {
  op_sub(c, c->a, v, 0);
  flags_xy(c, v);
}

static inline uint8_t op_inc(Cpu *c, uint8_t v) { /* INC r: C unchanged */
  uint8_t r = v + 1;
  c->hf = (v & 0x0F) == 0x0F;
  c->pf = v == 0x7F;
  c->nf = 0;
  c->sf = r >> 7;
  c->zf = r == 0;
  flags_xy(c, r);
  return r;
}
static inline uint8_t op_dec(Cpu *c, uint8_t v) { /* DEC r: C unchanged */
  uint8_t r = v - 1;
  c->hf = (v & 0x0F) == 0;
  c->pf = v == 0x80;
  c->nf = 1;
  c->sf = r >> 7;
  c->zf = r == 0;
  flags_xy(c, r);
  return r;
}

static inline uint8_t op_and(Cpu *c, uint8_t a, uint8_t v) {
  uint8_t r = a & v;
  flags_szp(c, r);
  c->hf = 1, c->nf = 0, c->cf = 0;
  return r;
}
static inline uint8_t op_or(Cpu *c, uint8_t a, uint8_t v) {
  uint8_t r = a | v;
  flags_szp(c, r);
  c->hf = 0, c->nf = 0, c->cf = 0;
  return r;
}
static inline uint8_t op_xor(Cpu *c, uint8_t a, uint8_t v) {
  uint8_t r = a ^ v;
  flags_szp(c, r);
  c->hf = 0, c->nf = 0, c->cf = 0;
  return r;
}
static inline void op_cpl(Cpu *c) { c->a = ~c->a, c->hf = 1, c->nf = 1, flags_xy(c, c->a); }
static inline void op_neg(Cpu *c) { c->a = op_sub(c, 0, c->a, 0); }
static inline void op_scf(Cpu *c) { c->cf = 1, c->hf = 0, c->nf = 0, flags_xy(c, c->a); }
static inline void op_ccf(Cpu *c) { c->hf = c->cf, c->cf = !c->cf, c->nf = 0, flags_xy(c, c->a); }

/* RLCA RRCA RLA RRA (on A; S Z P/V unchanged). */
static inline void op_rlca(Cpu *c) {
  c->cf = c->a >> 7, c->a = (uint8_t)(c->a << 1 | c->cf), c->hf = 0, c->nf = 0, flags_xy(c, c->a);
}
static inline void op_rrca(Cpu *c) {
  c->cf = c->a & 1, c->a = (uint8_t)(c->a >> 1 | c->cf << 7), c->hf = 0, c->nf = 0, flags_xy(c, c->a);
}
static inline void op_rla(Cpu *c) {
  bool o = c->a >> 7;
  c->a = (uint8_t)(c->a << 1 | c->cf), c->cf = o, c->hf = 0, c->nf = 0, flags_xy(c, c->a);
}
static inline void op_rra(Cpu *c) {
  bool o = c->a & 1;
  c->a = (uint8_t)(c->a >> 1 | c->cf << 7), c->cf = o, c->hf = 0, c->nf = 0, flags_xy(c, c->a);
}

/* The CB shifts and rotates: return the result. */
static inline uint8_t op_shift_flags(Cpu *c, uint8_t r, bool carry) {
  flags_szp(c, r);
  c->hf = 0, c->nf = 0, c->cf = carry;
  return r;
}
static inline uint8_t op_rlc(Cpu *c, uint8_t v) { return op_shift_flags(c, (uint8_t)(v << 1 | v >> 7), v >> 7); }
static inline uint8_t op_rrc(Cpu *c, uint8_t v) { return op_shift_flags(c, (uint8_t)(v >> 1 | v << 7), v & 1); }
static inline uint8_t op_rl(Cpu *c, uint8_t v) { return op_shift_flags(c, (uint8_t)(v << 1 | c->cf), v >> 7); }
static inline uint8_t op_rr(Cpu *c, uint8_t v) { return op_shift_flags(c, (uint8_t)(v >> 1 | c->cf << 7), v & 1); }
static inline uint8_t op_sla(Cpu *c, uint8_t v) { return op_shift_flags(c, (uint8_t)(v << 1), v >> 7); }
static inline uint8_t op_sra(Cpu *c, uint8_t v) { return op_shift_flags(c, (uint8_t)(v >> 1 | (v & 0x80)), v & 1); }
static inline uint8_t op_srl(Cpu *c, uint8_t v) { return op_shift_flags(c, v >> 1, v & 1); }

/* BIT n,v: Z (and P/V) set if the bit is clear. Bits 5 and 3 of F: from
 * v for BIT n,r; for BIT n,(IX+d) use op_bit_at with the address, whose
 * high byte they come from. (For BIT n,(HL) the Z80 takes them from its
 * internal MEMPTR, which is not modelled; op_bit takes them from v.) */
static inline bool op_bit(Cpu *c, int n, uint8_t v) {
  bool set = (v >> n) & 1;
  c->zf = !set, c->pf = !set, c->sf = n == 7 && set, c->hf = 1, c->nf = 0;
  flags_xy(c, v);
  return set;
}
static inline bool op_bit_at(Cpu *c, int n, uint8_t v, uint16_t addr) {
  bool set = op_bit(c, n, v);
  flags_xy(c, addr >> 8);
  return set;
}

/* 16-bit arithmetic. ADD: H and C from the add, S Z P/V unchanged. */
static inline uint16_t op_add16(Cpu *c, uint16_t a, uint16_t v) {
  unsigned r = a + v;
  c->hf = ((a & 0x0FFF) + (v & 0x0FFF)) > 0x0FFF;
  c->cf = r > 0xFFFF;
  c->nf = 0;
  flags_xy(c, (uint8_t)(r >> 8));
  return (uint16_t)r;
}
static inline uint16_t op_adc16(Cpu *c, uint16_t a, uint16_t v) {
  unsigned r = a + v + c->cf;
  c->hf = ((a & 0x0FFF) + (v & 0x0FFF) + c->cf) > 0x0FFF;
  c->pf = ((a ^ ~v) & (a ^ r) & 0x8000) != 0;
  c->cf = r > 0xFFFF;
  c->nf = 0;
  c->sf = (r >> 15) & 1;
  c->zf = (uint16_t)r == 0;
  flags_xy(c, (uint8_t)(r >> 8));
  return (uint16_t)r;
}
static inline uint16_t op_sbc16(Cpu *c, uint16_t a, uint16_t v) {
  int r = a - v - c->cf;
  c->hf = (a & 0x0FFF) < (v & 0x0FFF) + c->cf;
  c->pf = ((a ^ v) & (a ^ (uint16_t)r) & 0x8000) != 0;
  c->cf = r < 0;
  c->nf = 1;
  c->sf = ((uint16_t)r) >> 15;
  c->zf = (uint16_t)r == 0;
  flags_xy(c, (uint8_t)((uint16_t)r >> 8));
  return (uint16_t)r;
}

/* LDDR (LDIR is above). */
static inline void lddr(Cpu *c) {
  uint16_t hl = get_hl(c), de = get_de(c), bc = get_bc(c);
  uint8_t v;
  do c->mem[de--] = v = c->mem[hl--];
  while (--bc);
  set_hl(c, hl), set_de(c, de), set_bc(c, 0);
  c->hf = 0, c->pf = 0, c->nf = 0;
  v += c->a;
  c->yf = (v >> 1) & 1, c->xf = (v >> 3) & 1;
}

/* PUSH AF / POP AF on the real stack. */
static inline void cpu_push_af(Cpu *c) {
  c->sp -= 2;
  c->mem[(uint16_t)(c->sp + 1)] = c->a;
  c->mem[c->sp] = cpu_f(c);
}
static inline void cpu_pop_af(Cpu *c) {
  cpu_set_f(c, c->mem[c->sp]);
  c->a = c->mem[(uint16_t)(c->sp + 1)];
  c->sp += 2;
}

/* EX (SP),HL */
static inline void ex_sp_hl(Cpu *c) {
  uint16_t t = rd16(c, c->sp);
  wr16(c, c->sp, get_hl(c));
  set_hl(c, t);
}

/* Older names, used by the parser. */
static inline void flags_dec(Cpu *c, uint8_t before, uint8_t after) { /* DEC r */
  c->sf = after >> 7;
  c->zf = after == 0;
  c->hf = (before & 0x0F) == 0;
  c->pf = before == 0x80;
  c->nf = 1;
  flags_xy(c, after);
}
static inline void flags_logic(Cpu *c, bool h) { /* AND (h=1), OR, XOR (h=0) on A */
  c->sf = c->a >> 7;
  c->zf = c->a == 0;
  c->hf = h;
  c->pf = parity(c->a);
  c->nf = 0;
  c->cf = 0;
  flags_xy(c, c->a);
}

#endif
