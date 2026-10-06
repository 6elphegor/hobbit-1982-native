#include "hybrid.h"

#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "../clean/clean.h"
#include "../clean/platform.h"
#include "../port/routines.h"

#define SENTINEL 0x3FFE         /* return address for calls from C: in ROM, never run */
#define MAX_STEPS 4000000000UL  /* a routine that runs longer than this is stuck */
#define MAX_REPORTS 8           /* mismatch reports printed per routine */
#define MAX_ROUTINES 1024

static HybridMode mode;
static bool enabled[MAX_ROUTINES];
static unsigned long calls[MAX_ROUTINES], mismatches[MAX_ROUTINES];
static int pure_depth; /* > 0 while running an original for comparison */
static const char *dump_path; /* save the state before the first mismatching call */
static bool strict_stack;      /* compare the stack below the return address too */
static bool in_native; /* inside a C pass in check mode */

static int index_of(const PortRoutine *r) { return (int)(r - port_all()); }
static int native_depth; /* ported routines running */

/* A top-level entry (PORT_TOP) that is enabled. */
static bool is_top(uint16_t pc) {
  const PortRoutine *r = port_find(pc);
  return r && (r->flags & PORT_TOP) && enabled[index_of(r)];
}

/* The ported routine's frame is gone (it returned past it, or the game
 * restarted): stop running it. */
static bool frame_left(const Cpu *c, const z80 *z) { return z->sp > c->frame_sp || is_top(z->pc); }

/* ---------- registers ---------- */

static void sync_in(Cpu *c, Spectrum *s) {
  z80 *z = &s->cpu;
  c->a = z->a, c->b = z->b, c->c = z->c, c->d = z->d, c->e = z->e, c->h = z->h, c->l = z->l;
  c->b_ = z->b_, c->c_ = z->c_, c->d_ = z->d_, c->e_ = z->e_, c->h_ = z->h_, c->l_ = z->l_;
  c->ix = z->ix, c->iy = z->iy, c->sp = z->sp, c->r = z->r;
  c->sf = z->sf, c->zf = z->zf, c->hf = z->hf, c->pf = z->pf, c->nf = z->nf, c->cf = z->cf;
  c->yf = z->yf, c->xf = z->xf;
  c->mem = s->mem;
  c->host = s;
}

static void sync_out(Spectrum *s, const Cpu *c) {
  z80 *z = &s->cpu;
  z->a = c->a, z->b = c->b, z->c = c->c, z->d = c->d, z->e = c->e, z->h = c->h, z->l = c->l;
  z->b_ = c->b_, z->c_ = c->c_, z->d_ = c->d_, z->e_ = c->e_, z->h_ = c->h_, z->l_ = c->l_;
  z->ix = c->ix, z->iy = c->iy, z->sp = c->sp, z->r = c->r;
  z->sf = c->sf, z->zf = c->zf, z->hf = c->hf, z->pf = c->pf, z->nf = c->nf, z->cf = c->cf;
  z->yf = c->yf, z->xf = c->xf;
}

/* cpu_call: run code at addr until it returns to the sentinel pushed for
 * it (SP may have moved, if it took arguments off the stack), or until it
 * leaves the ported routine's frame, which abandons the routine. */
static void host_call(Cpu *c, uint16_t addr) {
  Spectrum *s = c->host;
  sync_out(s, c);
  z80 *z = &s->cpu;
  z->sp -= 2;
  s->mem[z->sp] = SENTINEL & 0xFF;
  s->mem[(uint16_t)(z->sp + 1)] = SENTINEL >> 8;
  z->pc = addr;
  while (z->pc != SENTINEL) {
    if (frame_left(c, z)) _longjmp(*(jmp_buf *)c->frame, 1);
    spec_step(s);
  }
  sync_in(c, s);
}

/* cpu_call_at: do what the CALL at site does (push site+3 and go to its
 * target, if its condition holds), until execution comes back to site+3.
 * The CALL is done here rather than by the Z80, so that it works without
 * one, and when site is the entry of a ported routine (one that starts
 * with a CALL). */
static bool condition(const z80 *z, uint8_t op) {
  switch ((op >> 3) & 7) {
  case 0: return !z->zf;
  case 1: return z->zf;
  case 2: return !z->cf;
  case 3: return z->cf;
  case 4: return !z->pf;
  case 5: return z->pf;
  case 6: return !z->sf;
  default: return z->sf;
  }
}

static void host_call_at(Cpu *c, uint16_t site) {
  Spectrum *s = c->host;
  uint8_t op = s->mem[site];
  if (op != 0xCD && (op & 0xC7) != 0xC4) {
    fprintf(stderr, "cpu_call_at($%04X): not a CALL ($%02X)\n", site, op);
    exit(4);
  }
  sync_out(s, c);
  z80 *z = &s->cpu;
  uint16_t sp = z->sp, back = (uint16_t)(site + 3);
  if (op != 0xCD && !condition(z, op)) return;
  z->sp -= 2;
  s->mem[z->sp] = back & 0xFF;
  s->mem[(uint16_t)(z->sp + 1)] = back >> 8;
  z->pc = s->mem[(uint16_t)(site + 1)] | s->mem[(uint16_t)(site + 2)] << 8;
  while (!(z->pc == back && z->sp >= sp)) {
    if (frame_left(c, z)) _longjmp(*(jmp_buf *)c->frame, 1);
    spec_step(s);
  }
  sync_in(c, s);
}

/* cpu_jump: run original code at addr until it reaches resume at the
 * current SP, or leaves the ported routine's frame. */
static bool host_jump(Cpu *c, uint16_t addr, int resume) {
  Spectrum *s = c->host;
  sync_out(s, c);
  z80 *z = &s->cpu;
  uint16_t sp = z->sp;
  z->pc = addr;
  for (;;) {
    if (z->pc == resume && z->sp == sp) {
      sync_in(c, s);
      return true;
    }
    if (frame_left(c, z)) break;
    spec_step(s);
  }
  sync_in(c, s);
  c->returned = true;
  return false;
}

static uint8_t host_in(Cpu *c, uint16_t port) { return spec_port_in(c->host, port); }

static uint8_t host_in_at(Cpu *c, uint16_t port, uint16_t pc) {
  Spectrum *s = c->host;
  s->ipc = pc;
  return spec_port_in(s, port);
}

static void host_out(Cpu *c, uint16_t port, uint8_t v) { spec_port_out(c->host, port, v); }

static void host_at(Cpu *c, uint16_t addr) {
  Spectrum *s = c->host;
  sync_out(s, c);
  spec_at(s, addr);
  sync_in(c, s);
}

static Cpu *current; /* the Cpu of the innermost routine running, for bridge_call */

/* Run fn, the C version of the routine at addr, in place of the original,
 * then return (popping the return address from wherever SP is then, as
 * RET would). */
static void run_fn(Spectrum *s, uint16_t addr, void (*fn)(Cpu *), unsigned flags) {
  Cpu c;
  sync_in(&c, s);
  c.call = host_call;
  c.call_at = host_call_at;
  c.jump = host_jump;
  c.in = host_in;
  c.in_at = host_in_at;
  c.out = host_out;
  c.at = host_at;
  /* A top-level entry has no caller: its frame is the whole stack (it
   * resets SP itself, and only a jump to a top-level entry leaves it). */
  c.frame_sp = (flags & PORT_TOP) ? 0xFFFF : c.sp;
  c.returned = false;
  jmp_buf frame;
  c.frame = &frame;
  int depth = native_depth;
  Cpu *outer = current;
  spec_at(s, addr);
  if (_setjmp(frame)) { /* the frame was left: the machine is where the original went */
    native_depth = depth;
    current = outer;
    return;
  }
  native_depth++;
  current = &c;
  fn(&c);
  current = outer;
  native_depth = depth;
  if (c.returned) return; /* the original finished it */
  sync_out(s, &c);
  z80 *z = &s->cpu;
  z->pc = s->mem[z->sp] | s->mem[(uint16_t)(z->sp + 1)] << 8;
  z->sp += 2;
}

static void run_native(Spectrum *s, const PortRoutine *r) { run_fn(s, r->addr, r->fn, r->flags); }

Cpu *bridge_cpu(void) { return current; }

/* bridge_call (clean/clean.h): clean code calling a routine that is not
 * clean yet. */
void bridge_call(uint16_t addr, Regs *r) {
  Cpu *c = current, keep = *c;
  c->a = r->a, c->b = r->b, c->c = r->c, c->d = r->d, c->e = r->e, c->h = r->h, c->l = r->l;
  c->ix = r->ix, c->iy = r->iy, c->zf = r->zf, c->cf = r->cf;
  cpu_call(c, addr);
  r->a = c->a, r->b = c->b, r->c = c->c, r->d = c->d, r->e = c->e, r->h = c->h, r->l = c->l;
  r->ix = c->ix, r->iy = c->iy, r->zf = c->zf, r->cf = c->cf;
  /* The caller's registers are its own: clean code calling other clean
   * code (whose bridges come here) must not change what an adapter
   * further out has in them. The stack pointer (messages take arguments
   * off the stack) and the refresh count go on as they are. */
  keep.sp = c->sp, keep.r = c->r, keep.returned = c->returned;
  *c = keep;
}

/* ---------- the clean edition's machine (clean/platform.h) ----------
 * Through the Cpu of the routine running now, while the port is around. */

uint8_t device_in(uint16_t port, uint16_t at) { return current->in_at(current, port, at); }

void device_out(uint16_t port, uint8_t v) { current->out(current, port, v); }

void device_at(uint16_t addr, uint8_t a) {
  uint8_t saved = current->a;
  current->a = a;
  current->at(current, addr);
  current->a = saved;
}

/* The refresh register as the machine has it (native code does not
 * count it as the original would). */
uint8_t device_random_byte(void) { return current->r; }

/* The ROM's SA-BYTES ($04C2) and LD-BYTES ($0556), which the machine
 * traps: A = the flag, IX = start, DE = length, carry set to load (reset
 * to verify); carry set on return if all went well. Registers kept. */
bool device_tape(bool save, uint16_t start, uint16_t len, bool verify) {
  Cpu *c = current, keep = *c;
  c->a = 0xFF;
  c->cf = !save && !verify;
  c->ix = start;
  set_de(c, len);
  cpu_call(c, save ? 0x04C2 : 0x0556);
  bool ok = c->cf;
  keep.sp = c->sp, keep.r = c->r;
  *c = keep;
  return ok;
}

/* The faithful restart, from the routine running: its frame is left (as
 * the original's jump leaves all of them), and the host unwinds to the
 * outermost loop, which runs $6C27. */
_Noreturn void device_restart(void) {
  cpu_tail(current, 0x6C27);
  _longjmp(*(jmp_buf *)current->frame, 1);
}

/* The jump, from the routine running: the machine goes where the
 * original would (in native mode, having no code there ends the run as
 * the game's crashes do). */
_Noreturn void device_crash(uint16_t addr) {
  cpu_tail(current, addr);
  _longjmp(*(jmp_buf *)current->frame, 1);
}

/* The machine is hung: the host reports it as it reports the original's. */
_Noreturn void device_hang(void) {
  Spectrum *s = current->host;
  s->hung = true;
  spec_stop(s);
  abort(); /* (spec_stop does not return) */
}

/* ---------- check mode ---------- */

typedef struct {
  uint8_t stream[4096];
  uint8_t ch[4096];
  size_t n;
  void (*forward)(void *ud, int stream, uint8_t ch); /* tee to here, if set */
  void *forward_ud;
} Capture;

static void capture(void *ud, int stream, uint8_t ch) {
  Capture *cap = ud;
  if (cap->n < sizeof cap->ch) {
    cap->stream[cap->n] = (uint8_t)stream;
    cap->ch[cap->n++] = ch;
  }
  if (cap->forward) cap->forward(cap->forward_ud, stream, ch);
}

static bool report_mismatch(const PortRoutine *r, const char *what) {
  int i = index_of(r);
  if (mismatches[i] == 0 || mismatches[i] < MAX_REPORTS)
    fprintf(stderr, "[check] %s ($%04X) call %lu: %s\n", r->name, r->addr, calls[i], what);
  return true;
}

static bool clean_compare; /* comparing a clean routine: its declared scratch is skipped */

static bool compare(const PortRoutine *r, unsigned mask, const Spectrum *o, const Spectrum *n,
                    uint16_t entry_sp, const Capture *co, const Capture *cn) {
  char buf[512];
  bool bad = false;
  const z80 *zo = &o->cpu, *zn = &n->cpu;
  struct {
    unsigned bit;
    const char *name;
    int vo, vn;
  } regs[] = {
      {OUT_A, "A", zo->a, zn->a},     {OUT_B, "B", zo->b, zn->b},     {OUT_C, "C", zo->c, zn->c},
      {OUT_D, "D", zo->d, zn->d},     {OUT_E, "E", zo->e, zn->e},     {OUT_H, "H", zo->h, zn->h},
      {OUT_L, "L", zo->l, zn->l},     {OUT_IX, "IX", zo->ix, zn->ix}, {OUT_IY, "IY", zo->iy, zn->iy},
      {OUT_ZF, "Z", zo->zf, zn->zf},  {OUT_CF, "C flag", zo->cf, zn->cf},
      {OUT_SF, "S", zo->sf, zn->sf},  {OUT_PF, "P/V", zo->pf, zn->pf},
      {OUT_ALT, "BC'", zo->b_ << 8 | zo->c_, zn->b_ << 8 | zn->c_},
      {OUT_ALT, "DE'", zo->d_ << 8 | zo->e_, zn->d_ << 8 | zn->e_},
      {OUT_ALT, "HL'", zo->h_ << 8 | zo->l_, zn->h_ << 8 | zn->l_},
      {OUT_FLAGS, "H", zo->hf, zn->hf},
      {OUT_FLAGS, "N", zo->nf, zn->nf},
      {OUT_FLAGS, "flag bit 5", zo->yf, zn->yf},
      {OUT_FLAGS, "flag bit 3", zo->xf, zn->xf},
  };
  /* Strict: every register and flag, whatever the routine's callers use
   * (a flag that differs can end up in a pushed AF, which the random
   * number generator reads). */
  unsigned outputs = strict_stack ? ~0u : mask;
  /* Leaving through a restart (a top-level entry resets SP and sets up
   * everything again), the clean edition's stack depth and registers at
   * the jump do not matter. */
  bool restart = clean_compare && zo->pc == zn->pc && is_top(zo->pc);
  if (restart) outputs = 0;
  for (size_t k = 0; k < sizeof regs / sizeof regs[0]; k++)
    if ((outputs & regs[k].bit) && regs[k].vo != regs[k].vn) {
      snprintf(buf, sizeof buf, "%s original $%02X, native $%02X", regs[k].name, regs[k].vo, regs[k].vn);
      bad = report_mismatch(r, buf);
    }
  if ((zo->sp != zn->sp && !restart) || zo->pc != zn->pc) {
    snprintf(buf, sizeof buf, "SP/PC original $%04X/$%04X, native $%04X/$%04X", zo->sp, zo->pc, zn->sp, zn->pc);
    bad = report_mismatch(r, buf);
  }
  /* Memory, except the stack below the routine's return address, which
   * holds whatever each version pushed. */
  uint16_t scratch_lo = strict_stack ? entry_sp : entry_sp >= 0x5B00 + 0x300 ? entry_sp - 0x300 : 0x5B00;
  /* Comparing a clean routine (whose generator does not read memory),
   * the stack below where both end is dead: popped, or, after a restart
   * (which sets SP to $5EFF), all of it. */
  uint16_t scratch_hi = entry_sp;
  if (clean_compare && zo->sp == zn->sp && zo->sp > scratch_hi) scratch_hi = zo->sp;
  if (restart && scratch_hi < 0x5EFF) scratch_hi = 0x5EFF;
  int diffs = 0;
  size_t len = 0;
  for (unsigned a = 0x4000; a < 0x10000; a++) {
    if (a >= scratch_lo && a < scratch_hi) continue;
    if (clean_compare && clean_is_scratch((uint16_t)a)) continue;
    if (o->mem[a] != n->mem[a]) {
      if (diffs < 8)
        len += snprintf(buf + len, sizeof buf - len, " $%04X:%02X/%02X", a, o->mem[a], n->mem[a]);
      diffs++;
    }
  }
  if (diffs) {
    char msg[600];
    snprintf(msg, sizeof msg, "%d bytes of memory differ (original/native):%s%s", diffs, buf,
             diffs > 8 ? " ..." : "");
    bad = report_mismatch(r, msg);
  }
  if (co->n != cn->n || memcmp(co->ch, cn->ch, co->n) || memcmp(co->stream, cn->stream, co->n)) {
    snprintf(buf, sizeof buf, "printed output differs (%zu vs %zu characters)", co->n, cn->n);
    bad = report_mismatch(r, buf);
  }
  if (o->qhead != n->qhead || o->getkey_calls != n->getkey_calls) {
    snprintf(buf, sizeof buf, "keyboard use differs (keys %zu/%zu, scans %lu/%lu)", o->qhead, n->qhead,
             o->getkey_calls, n->getkey_calls);
    bad = report_mismatch(r, buf);
  }
  return !bad;
}

static void check(Spectrum *s, const PortRoutine *r) {
  static Spectrum before, original;
  static Capture out_o, out_n;
  void (*on_char)(void *, int, uint8_t) = s->on_char;
  void *ud = s->ud;
  jmp_buf *stop = s->stop_jmp;
  z80 *z = &s->cpu;
  uint16_t entry_sp = z->sp;

  before = *s;

  /* The original, printing as it goes (and recorded), until it leaves the
   * routine's frame: by returning, or by abandoning it (a restart). If the
   * input runs out on the way, the run ends here. */
  out_o.n = 0;
  out_o.forward = on_char, out_o.forward_ud = ud;
  s->on_char = capture, s->ud = &out_o;
  pure_depth++;
  unsigned long steps = 0;
  while (z->sp <= entry_sp && !is_top(z->pc)) {
    spec_step(s);
    if (++steps > MAX_STEPS) {
      fprintf(stderr, "[check] %s ($%04X): the original did not return\n", r->name, r->addr);
      exit(4);
    }
  }
  pure_depth--;
  original = *s;

  /* The port, from the same state. */
  *s = before;
  out_n.n = 0;
  out_n.forward = NULL;
  s->on_char = capture, s->ud = &out_n;
  jmp_buf native_stop;
  s->stop_jmp = &native_stop;
  in_native = true;
  if (_setjmp(native_stop) == 0) {
    run_native(s, r);
    /* If its RET did not leave the frame (the stack held something else,
     * as with an unclosed quote), follow it as the original was followed. */
    for (unsigned long n = 0; z->sp <= entry_sp && !is_top(z->pc) && n < MAX_STEPS; n++) spec_step(s);
    if (!compare(r, r->outputs, &original, s, entry_sp, &out_o, &out_n)) {
      if (dump_path && !mismatches[index_of(r)]) {
        FILE *f = fopen(dump_path, "wb");
        if (f) {
          /* 64K of memory, then the registers on entry. */
          const z80 *zb = &before.cpu;
          uint8_t regs[] = {zb->a, zb->b, zb->c, zb->d, zb->e, zb->h, zb->l, zb->ix & 0xFF, zb->ix >> 8,
                            zb->iy & 0xFF, zb->iy >> 8, zb->sp & 0xFF, zb->sp >> 8, zb->pc & 0xFF, zb->pc >> 8};
          fwrite(before.mem, 1, sizeof before.mem, f);
          fwrite(regs, 1, sizeof regs, f);
          fclose(f);
          fprintf(stderr, "[check] state before the call saved to %s\n", dump_path);
        }
      }
      mismatches[index_of(r)]++;
    }
  } else {
    report_mismatch(r, "the native version asked for more input than the original used");
    mismatches[index_of(r)]++;
  }
  in_native = false;

  /* Carry on from the original. */
  *s = original;
  s->on_char = on_char, s->ud = ud;
  s->stop_jmp = stop;
}

/* ---------- clean check mode ---------- */

static bool use_clean;   /* run clean routines where there are any */
static bool clean_skip[0x10000]; /* --clean-skip */
static int clean_check_depth; /* > 0 inside one of the two passes */

/* Run the faithful routine r and the clean routine cr from the same
 * state, compare, and carry on from the faithful one's result. Nested
 * calls run faithful in the first pass and clean (where there is one) in
 * the second. */
/* Sampling and mutation (--check-limit, --mutate): only the first
 * check_limit calls of each routine are compared; each of those is also
 * run again from the same state with its input registers changed, taken
 * from other calls of the same routine (so they stay in the routine's
 * domain), now and then random. */
static unsigned long check_limit; /* 0: every call */
static int mutations;
static unsigned long checked[MAX_ROUTINES];

typedef struct {
  uint8_t a, b, c, d, e, h, l;
  uint16_t ix, iy;
  bool zf, cf;
} Inputs;
#define SEEN 16
static Inputs seen[MAX_ROUTINES][SEEN];
static unsigned nseen[MAX_ROUTINES];

static uint32_t mrng = 0x9E3779B9u;
static uint32_t mrand(void) {
  mrng ^= mrng << 13, mrng ^= mrng >> 17, mrng ^= mrng << 5;
  return mrng;
}

static Inputs inputs_of(const z80 *z) {
  Inputs in = {z->a, z->b, z->c, z->d, z->e, z->h, z->l, z->ix, z->iy, z->zf, z->cf};
  return in;
}

/* Register pairs are mutated whole (half of one call's pointer and half
 * of another's point nowhere the routine is ever given). A random value
 * never points into the stack's page ($5E00-$5EFF: SP starts at $5EFF),
 * where the faithful routine's own pushes overwrite what a clean one
 * would read, nor into the screen, which printing overwrites while it
 * reads (screen addresses come only from other calls). */
static uint16_t pick16(uint16_t cur, uint16_t other) {
  if (mrand() & 1) return (other >> 8) == 0x5E ? cur : other;
  if (mrand() % 16) return cur;
  uint16_t v = (uint16_t)mrand();
  return (v >> 8) == 0x5E || (v >= 0x4000 && v < 0x5B00) ? cur : v;
}

static void mutate(z80 *z, int i, unsigned fixed) {
  Inputs in = inputs_of(z), keep = in;
  const Inputs *o = nseen[i] ? &seen[i][mrand() % (nseen[i] < SEEN ? nseen[i] : SEEN)] : &in;
  in.a = (mrand() & 1) ? o->a : (mrand() % 8 == 0) ? (uint8_t)mrand() : in.a;
#define PAIR(hi, lo)                                                                   \
  do {                                                                                 \
    uint16_t v = pick16((uint16_t)(in.hi << 8 | in.lo), (uint16_t)(o->hi << 8 | o->lo)); \
    in.hi = v >> 8, in.lo = (uint8_t)v;                                                \
  } while (0)
  PAIR(b, c);
  PAIR(d, e);
  PAIR(h, l);
#undef PAIR
  in.ix = pick16(in.ix, o->ix), in.iy = pick16(in.iy, o->iy);
  if (mrand() % 4 == 0) in.zf = mrand() & 1;
  if (mrand() % 4 == 0) in.cf = mrand() & 1;
  /* What the callers always set to constants stays as this call had it. */
  if (fixed & OUT_A) in.a = keep.a;
  if (fixed & OUT_B) in.b = keep.b;
  if (fixed & OUT_C) in.c = keep.c;
  if (fixed & OUT_D) in.d = keep.d;
  if (fixed & OUT_E) in.e = keep.e;
  if (fixed & OUT_H) in.h = keep.h;
  if (fixed & OUT_L) in.l = keep.l;
  if (fixed & OUT_IX) in.ix = keep.ix;
  if (fixed & OUT_IY) in.iy = keep.iy;
  if (fixed & OUT_ZF) in.zf = keep.zf;
  if (fixed & OUT_CF) in.cf = keep.cf;
  z->a = in.a, z->b = in.b, z->c = in.c, z->d = in.d, z->e = in.e, z->h = in.h, z->l = in.l;
  z->ix = in.ix, z->iy = in.iy, z->zf = in.zf, z->cf = in.cf;
}

/* One comparison: from the state in *s, run the faithful routine r and the
 * clean cr, and compare. *faithful gets the faithful result. With
 * forward, the faithful run's output goes to the transcript. Returns false
 * if the faithful run did not finish within limit steps (mutated inputs can
 * send a routine round for ever): nothing is compared then. */
/* The machine of the trial (a run with other inputs) going on, if any:
 * hybrid_abandon_trial ends it. */
static Spectrum *trial_machine;

bool hybrid_abandon_trial(void) {
  if (!trial_machine) return false;
  sigset_t set; /* (called from a signal handler: let the signal come again) */
  sigemptyset(&set);
  sigaddset(&set, SIGVTALRM);
  sigprocmask(SIG_UNBLOCK, &set, NULL);
  _longjmp(*trial_machine->stop_jmp, 1);
}

static bool dual(Spectrum *s, const PortRoutine *r, const CleanRoutine *cr, Spectrum *faithful, bool forward,
                 unsigned long limit, const char *what) {
  static Spectrum before;
  static Capture out_f, out_c;
  void (*on_char)(void *, int, uint8_t) = s->on_char;
  void *ud = s->ud;
  jmp_buf *stop = s->stop_jmp;
  z80 *z = &s->cpu;
  uint16_t entry_sp = z->sp;
  bool finished = true;

  before = *s;
  clean_check_depth++;
  /* What a pass left unwound (by a jump out of it) is put back. */
  Cpu *outer = current;
  int depth = native_depth, pure = pure_depth;
  bool native = in_native;
#define UNWOUND() (current = outer, native_depth = depth, pure_depth = pure, in_native = native)
  if (!forward) trial_machine = s;

  /* The faithful port. */
  out_f.n = 0;
  out_f.forward = forward ? on_char : NULL, out_f.forward_ud = ud;
  s->on_char = capture, s->ud = &out_f;
  use_clean = false;
  jmp_buf faithful_stop;
  if (!forward) s->stop_jmp = &faithful_stop;
  unsigned long n = 0;
  if (forward || _setjmp(faithful_stop) == 0) {
    run_fn(s, r->addr, r->fn, r->flags);
    for (; z->sp <= entry_sp && !is_top(z->pc) && n < limit; n++) spec_step(s);
  } else {
    finished = false; /* asked for input, crashed, or (a trial) never ended */
    UNWOUND();
  }
  if (n >= limit) finished = false;
  *faithful = *s;
  /* The caller carries on from *faithful: with its own hooks, not the
   * capture (which, left in place, would forward to itself next time). */
  faithful->on_char = on_char, faithful->ud = ud;
  faithful->stop_jmp = stop;

  /* The clean edition, from the same state. */
  if (finished) {
    *s = before;
    out_c.n = 0;
    out_c.forward = NULL;
    s->on_char = capture, s->ud = &out_c;
    jmp_buf clean_stop;
    s->stop_jmp = &clean_stop;
    use_clean = true;
    n = 0;
    if (_setjmp(clean_stop) == 0) {
      run_fn(s, cr->addr, cr->adapter, 0);
      for (; z->sp <= entry_sp && !is_top(z->pc) && n < limit; n++) spec_step(s);
      clean_compare = true;
      if (n >= limit) {
        report_mismatch(r, what ? "(mutated) the clean version did not finish" : "the clean version did not finish");
        mismatches[index_of(r)]++;
      } else if (!compare(r, cr->outputs, faithful, s, entry_sp, &out_f, &out_c)) {
        if (what) report_mismatch(r, what);
        mismatches[index_of(r)]++;
      }
      clean_compare = false;
    } else {
      UNWOUND();
      report_mismatch(r, what ? "(mutated) the clean version asked for more input, crashed or never ended, where the faithful one did not"
                              : "the clean version asked for more input, or crashed, where the faithful one did not");
      mismatches[index_of(r)]++;
    }
  }
#undef UNWOUND
  trial_machine = NULL;
  use_clean = false;
  clean_check_depth--;
  *s = before;
  s->on_char = on_char, s->ud = ud;
  s->stop_jmp = stop;
  return finished;
}

/* Run the faithful routine r and the clean cr from the same state,
 * compare, and carry on from the faithful one's result; then the mutated
 * runs. Nested calls run faithful in the first pass and clean (where there
 * is one) in the second. */
static void check_clean(Spectrum *s, const PortRoutine *r, const CleanRoutine *cr) {
  static Spectrum start, faithful, scratch;
  int i = index_of(r);
  Inputs in = inputs_of(&s->cpu);
  if (nseen[i] < SEEN) seen[i][nseen[i]] = in;
  else if (mrand() % (nseen[i] + 1) < SEEN) seen[i][mrand() % SEEN] = in;
  nseen[i]++;
  checked[i]++;

  start = *s;
  for (int m = 0; m < mutations; m++) {
    *s = start;
    mutate(&s->cpu, i, cr->fixed);
    char what[200];
    const z80 *o = &start.cpu, *z = &s->cpu;
    snprintf(what, sizeof what,
             "(mutated inputs: A=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X IX=%04X IY=%04X Z=%d C=%d; "
             "the call's: A=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X IX=%04X IY=%04X Z=%d C=%d)",
             z->a, z->b, z->c, z->d, z->e, z->h, z->l, z->ix, z->iy, z->zf, z->cf,
             o->a, o->b, o->c, o->d, o->e, o->h, o->l, o->ix, o->iy, o->zf, o->cf);
    dual(s, r, cr, &scratch, false, 20000, what);
  }
  *s = start;
  dual(s, r, cr, &faithful, true, MAX_STEPS, NULL);
  *s = faithful;
}

/* ---------- hook ---------- */

static bool hook(Spectrum *s) {
  if (pure_depth > 0) return false;
  const PortRoutine *r = port_find(s->cpu.pc);
  if (!r || !enabled[index_of(r)]) return false;
  /* A top-level entry runs only from the outermost loop; anywhere deeper,
   * the loops above unwind to there first. */
  if ((r->flags & PORT_TOP) && (native_depth > 0 || in_native)) return false;
  calls[index_of(r)]++;
  const CleanRoutine *cr = clean_skip[s->cpu.pc] ? NULL : clean_find(s->cpu.pc);
  if (cr && mode == HYBRID_CHECK_CLEAN && clean_check_depth == 0 && !(r->flags & PORT_TOP) &&
      (check_limit == 0 || checked[index_of(r)] < check_limit)) {
    check_clean(s, r, cr);
    return true;
  }
  if (cr && use_clean) {
    run_fn(s, cr->addr, cr->adapter, r->flags);
    return true;
  }
  if (mode == HYBRID_CHECK_CLEAN) {
    run_native(s, r);
    return true;
  }
  if (mode == HYBRID_CHECK && !in_native && !(r->flags & PORT_TOP))
    check(s, r);
  else
    run_native(s, r);
  return true;
}

void hybrid_dump_mismatch(const char *path) { dump_path = path; }
void hybrid_strict_stack(bool on) { strict_stack = on; }

void hybrid_use_clean(bool on) { use_clean = on; }

/* --clean-skip: run the faithful routine at these addresses even where
 * there is a clean one (to find which clean routine changes a run). */
int hybrid_clean_skip(const char *list) {
  for (const char *p = list; *p;) {
    char *end;
    unsigned long addr = strtoul(p, &end, 16);
    if (end == p || !clean_find((uint16_t)addr)) {
      fprintf(stderr, "no clean routine at %.*s\n", (int)strcspn(p, ","), p);
      return -1;
    }
    clean_skip[addr] = true;
    p = *end == ',' ? end + 1 : end;
  }
  return 0;
}
void hybrid_check_sampling(unsigned long limit, int mutate) { check_limit = limit, mutations = mutate; }

int hybrid_attach(Spectrum *s, HybridMode m, const char *only) {
  mode = m;
  mem = s->mem; /* for the clean edition (clean/data.h) */
  original_bugs = true; /* it is compared with the faithful port */
  for (const PortRoutine *r = port_all(); r->fn; r++) enabled[index_of(r)] = only == NULL;
  for (const char *p = only; p && *p;) {
    char *end;
    unsigned long addr = strtoul(p, &end, 16);
    const PortRoutine *r = end != p ? port_find((uint16_t)addr) : NULL;
    if (!r) {
      fprintf(stderr, "no ported routine at %.*s\n", (int)strcspn(p, ","), p);
      return -1;
    }
    enabled[index_of(r)] = true;
    p = *end == ',' ? end + 1 : end;
  }
  s->hook = mode == HYBRID_OFF ? NULL : hook;
  return 0;
}

unsigned long hybrid_report(FILE *f) {
  unsigned long total = 0;
  if (mode != HYBRID_CHECK && mode != HYBRID_CHECK_CLEAN) { /* just a summary */
    unsigned long n = 0, used = 0, all = 0;
    for (const PortRoutine *r = port_all(); r->fn; r++) {
      int i = index_of(r);
      if (!enabled[i]) continue;
      all++;
      n += calls[i];
      used += calls[i] > 0;
    }
    fprintf(f, "[hybrid] %lu calls of %lu of the %lu routines\n", n, used, all);
    /* With the clean edition: the faithful routines it still reached. */
    for (const PortRoutine *r = port_all(); use_clean && r->fn; r++) {
      int i = index_of(r);
      if (enabled[i] && calls[i] && (!clean_find(r->addr) || clean_skip[r->addr]))
        fprintf(f, "[clean] faithful %-14s $%04X  %6lu calls\n", r->name, r->addr, calls[i]);
    }
    return 0;
  }
  for (const PortRoutine *r = port_all(); r->fn; r++) {
    int i = index_of(r);
    if (!enabled[i]) continue;
    if (mode == HYBRID_CHECK_CLEAN && !clean_find(r->addr)) continue;
    fprintf(f, "[%s] %-12s $%04X  %6lu calls", mode == HYBRID_CHECK ? "check" : "clean check", r->name,
            r->addr, calls[i]);
    if (mode == HYBRID_CHECK || mode == HYBRID_CHECK_CLEAN) fprintf(f, "  %lu mismatches", mismatches[i]);
    fputc('\n', f);
    total += mismatches[i];
  }
  return total;
}
