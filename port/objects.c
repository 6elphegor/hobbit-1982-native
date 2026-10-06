/* Object and location lookups, and the random number generator
 * ($9A85-$A137).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * ---------- The data ----------
 *
 * Object index ($C063): 3-byte records [id][record lo][record hi], ending
 * with $FF. Objects and characters share it (characters are $3C up).
 * Loops that walk it start IX at $C060 (Step3ByteTable adds 3 first); the
 * weight/size sum ($9D00) starts at $C063 and so skips object 0.
 *
 * Object record (e.g. object 0, "you", at $C11B):
 *   +$00  number of locations it is in (doors and the like are in 2;
 *         a count other than 1 makes ObjectFirstLocation return $FF)
 *   +$01  the object it is inside or carried by ("mother"), $FF if none
 *   +$02  size        +$03  weight (both summed by $9CE8/$9CED)
 *   +$04  bits 4-7: index of its "contains" message ($A09D)
 *   +$05  strength, +$06 toughness (breaking, $92ED)
 *   +$07  attributes: bit 7 present/visible, bit 6 a character, bit 5
 *         open (characters have it too), bit 3 broken (the two together,
 *         $28: you can see inside), bit 1 liquid (evaporates when its
 *         container goes), bit 0 locked (doors)
 *   +$08  name: three dictionary words (adjective, adjective, noun; $0FFF
 *         of each is the word's offset from $6000), printed by $9ED6
 *   +$10  its locations (+$00 of them, usually one)
 *
 * Location table ($B9E0): a word per location $00-$4F pointing at its
 * record ($BA8A...; location 0 is not used). LocateLocation leaves IX
 * unchanged for an id >= $50.
 * Location record: +$01 capacity (against the sizes of what is there,
 * $9C41), +$02 name (three words, e.g. HALL TUNNEL LIKE), +$08/9 ?, from
 * +$0A the exits: 3-byte records [direction][door object, 0 none]
 * [destination], ending with $FF. Directions 1 N, 2 S, 3 E, 4 W, 5 NE,
 * 6 NW, 7 SE, 8 SW, 9 up, 10 down ($A20E: their dictionary words).
 *
 * Hangs of the original (fuzzing): two ways, both reproduced here.
 * 1. A sentence with too many noun phrases ("CLIMB FOUL ELVENKINGS ALL BUT
 *    AN ALL ONTO SPIDER CAMP") builds parser records past $B9C8: IY reaches
 *    $B9E0 and the LDIR at $784A writes 10 bytes over $B9EE-$B9F7, the
 *    location pointers of locations 7-11. When Gandalf (who wanders) is in
 *    one of them, the random walk at $8FAD tries directions 1-9 through
 *    $9F08 for ever: the location's "exits" are now ROM bytes with none.
 * 2. Attacking and killing an object the parser made up from garbage (id
 *    $FF) empties it through $9D53: LocateObject($FF) gives the word after
 *    the index as its record, so every object with no container (mother
 *    $FF) is put in object 7 (the web), the web included, and liquids
 *    evaporate. The next "is it inside" test ($9C8A) walks 7 -> 7 for ever.
 *
 * Character table ($CACB): 7-byte records ending with $FF:
 *   +$00 character id, +$01 number of entries in its table, +$02/3 the
 *   entry chosen, +$04/5 pointer to a table of 3-byte [key][pointer].
 *
 * Variables: $B6E8/$B6E9 the first and second object of the command (ids,
 * $FF none), $B6EA the character acting (0 = you), $B708/$B70A their
 * records, $B70C the record of the acting character, $B6E7 the action,
 * $B6F5 your location, $980C 1 when it is dark, $B70E the random seed,
 * $B712 the random pointer, $B6FA/$B6FB/$B702/$B703/$B704 output control.
 *
 * ---------- How the port calls out ----------
 *
 * Calls run the original CALL instruction (cpu_call_at), so the real
 * return address goes on the stack. Where the CALL is itself a registered
 * entry point ($9C8A, $9C9F, $9E25, $A0AE), cpu_call is used instead, as
 * running that instruction would run the routine's C version.
 * The stack is kept exactly as the original keeps it (push16/pop16 for
 * every PUSH and POP across a call), since several routines recurse.
 * The message printer ($72DD) takes parameters pushed before the call
 * off the stack (from under its return address); cpu_call_at allows it. */
#include <stddef.h>

#include "cpu.h"
#include "routines.h"

#define OBJ_INDEX 0xC063     /* object index: [id][record], $FF ends */
#define OBJ_INDEX_M3 0xC060  /* 3 bytes before it, for Step3ByteTable loops */
#define LOC_TABLE 0xB9E0     /* location records, a word per location */
#define LOC_COUNT 0x50
#define CHAR_TABLE 0xCACB    /* characters: 7-byte records, $FF ends */
#define OBJ1 0xB6E8          /* first object of the command */
#define OBJ2 0xB6E9          /* second object */
#define ACTOR_ID 0xB6EA      /* the character acting (0 = you) */
#define ACTION 0xB6E7
#define OBJ1_REC 0xB708
#define OBJ2_REC 0xB70A
#define ACTOR_REC 0xB70C     /* record of the character acting */
#define YOUR_LOC 0xB6F5
#define DARK 0x980C          /* 1 when your location is dark */
#define MOVE_FLAG 0x9BDC     /* data byte in code: set by $9BDD, cleared when you move */
#define RNG_SEED 0xB70E
#define RNG_PTR 0xB712       /* the pointer the RNG reads through (high byte at +1 counts first) */
#define MATCH_MODE 0xB710    /* $9DD9: 0 objects, 1 characters, 2 either */
#define VAR_B70F 0xB70F      /* unknown: nonzero skips the visibility test in $9DD9 */
#define VAR_B6FA 0xB6FA      /* unknown: output enable, with $B6FB */
#define VAR_B6FB 0xB6FB
#define VAR_B6FE 0xB6FE      /* unknown: first object given as a pronoun/plural? */
#define VAR_B6FF 0xB6FF
#define VAR_B702 0xB702
#define VAR_B703 0xB703      /* name printing mode for $9ED6 */
#define VAR_B704 0xB704
#define INDENT 0x869F        /* text indentation, used by the object lists */
#define EXIT_DISP 0x9F42     /* self-modified displacement of LD A,(IX+d) at $9F40 */
#define LOOK_ARG 0x8D9B      /* data in code, written by $9BDD before LOOK */
#define YOUR_LOC_OBJ 0xC12B  /* +$10 of object 0 ("you") */
#define DIR_WORDS 0xA20E     /* dictionary words for the directions */
#define MSG_TABLE_A09D 0xAFCA

#define FULL (OUT_REGS | OUT_ZF | OUT_CF | OUT_SF | OUT_PF | OUT_ALT)

/* CALL nn at addr in the original: cpu_call_at (the real return address
 * on the stack; a callee that leaves the frame abandons the routine).
 * addr must not be a registered entry point. */
#define CALL(addr) cpu_call_at(c, addr)
/* A conditional CALL (CALL NZ/Z) at addr, run by the original
 * instruction itself with the flags as they are; resumes after it. False
 * if the callee left this routine's frame (return at once). */
#define CALL_IF(addr) \
  do { \
    cpu_call_at(c, addr); \
  } while (0)

static uint8_t rd(const Cpu *c, uint16_t a) { return c->mem[a]; }
static uint8_t at_ix(const Cpu *c, int d) { return c->mem[(uint16_t)(c->ix + d)]; }
static uint8_t at_iy(const Cpu *c, int d) { return c->mem[(uint16_t)(c->iy + d)]; }

/* PUSH AF / POP AF, F with its undocumented bits as the Z80 has them. */
static void push_af(Cpu *c) { cpu_push_af(c); }
static void pop_af(Cpu *c) { cpu_pop_af(c); }
/* AF kept in C, for EX AF,AF' (AF' is not modelled). */
typedef struct {
  uint8_t a, f;
} SavedAF;
static SavedAF save_af(const Cpu *c) { return (SavedAF){c->a, cpu_f(c)}; }
static void restore_af(Cpu *c, SavedAF s) { c->a = s.a, cpu_set_f(c, s.f); }

/* Add n opcode fetches to R (low 7 bits), as the original's instructions
 * would. Only $9DBD counts them: it runs before the game takes its seed
 * from R ($6C31, then $6CA7). */
static void add_r(Cpu *c, unsigned n) { c->r = (uint8_t)((c->r & 0x80) | ((c->r + n) & 0x7F)); }

/* ---------- characters ---------- */

/* $9A85: find character A in the character table at $CACB. Leaves IY and
 * HL at its record (or the $FF that ends the table), A its first byte, Z
 * if found or at the end. */
static void p_9a85(Cpu *c) {
  push16(c, get_de(c));
  push16(c, get_bc(c));
  uint8_t key = c->a;
  uint16_t hl = CHAR_TABLE;
  for (;;) {
    c->a = c->mem[hl];
    op_cp(c, key);
    if (c->zf) break;
    op_cp(c, 0xFF);
    if (c->zf) break;
    hl = op_add16(c, hl, 7);
  }
  set_hl(c, hl);
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  push16(c, hl);
  c->iy = pop16(c);
}

/* $9AA0: for character A, find key B in its table, and make that entry
 * its current one (+$02/3). IX and IY are kept. */
static void p_9aa0(Cpu *c) {
  push16(c, c->iy);
  push16(c, c->ix);
  CALL(0x9AA4);
  op_cp(c, 0xFF);
  if (c->zf) goto L9AC8;
  c->l = at_iy(c, 4);
  c->h = at_iy(c, 5);
  push16(c, get_hl(c));
  c->ix = pop16(c);
  c->a = c->b;
  CALL(0x9AB5);
  op_cp(c, 0xFF);
  if (c->zf) goto L9AC8;
  c->l = at_ix(c, 1);
  c->h = at_ix(c, 2);
  c->mem[(uint16_t)(c->iy + 2)] = c->l;
  c->mem[(uint16_t)(c->iy + 3)] = c->h;
L9AC8:
  c->ix = pop16(c);
  c->iy = pop16(c);
}

/* $9ACD: when an object A (not $FF or 0) that is in one location is
 * mentioned... if it is in your location (and not at (HL)), it is light,
 * and the move flag is set, print message DE with the object's name. */
static void p_9acd(Cpu *c) {
  op_cp(c, 0xFF);
  if (c->zf) return;
  c->a = op_and(c, c->a, c->a);
  if (c->zf) return;
  c->b = c->a;
  c->a = rd(c, DARK);
  op_cp(c, 0x02);
  if (c->zf) return;
  c->a = c->b;
  CALL(0x9ADA);
  c->c = c->a;
  op_cp(c, rd(c, get_hl(c)));
  if (c->zf) return;
  c->a = rd(c, YOUR_LOC);
  op_cp(c, c->c);
  if (!c->zf) return;
  c->a = rd(c, MOVE_FLAG);
  c->a = op_and(c, c->a, c->a);
  if (c->zf) return;
  c->a = 0x01;
  c->mem[VAR_B702] = c->a;
  push16(c, get_de(c));
  set_hl(c, pop16(c));
  c->a = c->b;
  CALL(0x9AF2);
  push16(c, get_de(c));
  set_de(c, 0x0008);
  c->ix = op_add16(c, c->ix, get_de(c));
  set_de(c, pop16(c));
  push16(c, c->ix); /* the name, taken off the stack by the message */
  CALL(0x9AFE);
}

/* $9B02 Action_None: note your location (that of object 0) in $B6F5 and
 * whether it is dark in $980C. */
static void p_action_none(Cpu *c) {
  c->a = 0x00;
  CALL(0x9B04);
  c->mem[YOUR_LOC] = c->a;
  CALL(0x9B0A);
  c->a = 0x00;
  if (c->cf) c->a = op_inc(c, c->a);
  c->mem[DARK] = c->a;
}

/* $9B16 (reached only by JP NZ from $986D, inside the character loop
 * $980E, and jumping back into it at $9870/$9901) is part of that loop
 * and is translated with it in port/actions2.c. */

/* $9B44: compare the objects of the command with the actor: NZ (A=1) if
 * there is no first object; otherwise Z if the actor is one of them. */
static void p_9b44(Cpu *c) {
  c->a = rd(c, OBJ1);
  c->a = op_inc(c, c->a);
  if (c->zf) {
    c->a = op_inc(c, c->a);
    return;
  }
  c->a = rd(c, VAR_B6FE);
  c->a = op_and(c, c->a, c->a);
  if (c->zf) {
    set_hl(c, OBJ1);
    c->a = rd(c, ACTOR_ID);
    op_cp(c, rd(c, get_hl(c)));
    if (c->zf) return;
    c->a = rd(c, OBJ2);
    op_cp(c, rd(c, get_hl(c)));
    if (c->zf) return;
  }
  c->a = rd(c, VAR_B6FF);
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) return;
  c->a = rd(c, OBJ2);
  set_hl(c, ACTOR_ID);
  op_cp(c, rd(c, get_hl(c)));
}

/* $9B6C TriggerAction: run the action code at HL (if not 0), keeping IX,
 * IY, DE, BC and HL. */
static void p_trigger_action(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  push16(c, get_de(c));
  push16(c, get_bc(c));
  push16(c, get_hl(c));
  c->a = op_or(c, c->l, c->h);
  CALL_IF(0x9B75); /* CALL NZ,$9B80 */
  set_hl(c, pop16(c));
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  c->iy = pop16(c);
  c->ix = pop16(c);
}

/* $9B80 TriggerAction_Jump: JP (HL). */
static void p_trigger_action_jump(Cpu *c) { cpu_tail(c, get_hl(c)); }

/* $9B81: look up key A in the table that follows the record at IX
 * (IX + (IX+0) + $10, a 3-byte id table), leaving IX at the entry. */
static void p_9b81(Cpu *c) {
  push16(c, get_de(c));
  c->d = c->a;
  c->a = at_ix(c, 0);
  c->a = op_add(c, c->a, 0x10, 0);
  c->e = c->a;
  c->a = c->d;
  c->d = 0x00;
  c->ix = op_add16(c, c->ix, get_de(c));
  CALL(0x9B8E);
  set_de(c, pop16(c));
}

/* ---------- tables ---------- */

/* $9B93 Step3ByteTable: move IX on to the next 3-byte record; IY = its
 * pointer, A = its id, Z at the $FF that ends the table. Uses DE' (left
 * holding IY). */
static void p_step3(Cpu *c) {
  exx(c);
  set_de(c, 0x0003);
  c->ix = op_add16(c, c->ix, get_de(c));
  c->d = at_ix(c, 2);
  c->e = at_ix(c, 1);
  push16(c, get_de(c));
  c->iy = pop16(c);
  c->a = at_ix(c, 0);
  op_cp(c, 0xFF);
  exx(c);
}

/* $9BA9 Step3ByteTable_Next: the same, keeping A. */
static void p_step3_next(Cpu *c) {
  push16(c, get_bc(c));
  c->b = c->a;
  CALL(0x9BAB);
  c->a = c->b;
  set_bc(c, pop16(c));
}

/* $9BB1 LocateLocation: IX = the record of location A. A >= $50: A=0, Z,
 * and IX unchanged. */
static void p_locate_location(Cpu *c) {
  op_cp(c, LOC_COUNT);
  if (!c->cf) {
    c->a = op_xor(c, c->a, c->a);
    return;
  }
  push16(c, get_de(c));
  push16(c, get_hl(c));
  uint16_t hl = c->a;
  hl = op_add16(c, hl, hl);
  hl = op_add16(c, hl, LOC_TABLE);
  push16(c, rd16(c, hl));
  c->ix = pop16(c);
  set_hl(c, pop16(c));
  set_de(c, pop16(c));
}

/* $9BCA LocateObject: IX = the record of object A. A not found: A=$FF, Z,
 * and IX = the word after the end of the index. */
static void p_locate_object(Cpu *c) {
  c->ix = OBJ_INDEX;
  CALL(0x9BCE);
  push16(c, rd16(c, (uint16_t)(c->ix + 1))); /* PUSH HL; EX (SP),HL */
  c->ix = pop16(c);
}

/* $9C17: give everything inside object A (recursively) location B. If
 * object 0 (you) is among them, clear the move flag. */
static void p_9c17(Cpu *c) {
  push16(c, c->iy);
  push16(c, c->ix);
  c->ix = OBJ_INDEX_M3;
  for (;;) {
    CALL(0x9C1F);
    if (c->zf) break;
    op_cp(c, at_iy(c, 1));
    if (!c->zf) continue;
    c->mem[(uint16_t)(c->iy + 0x10)] = c->b;
    push_af(c);
    c->a = at_ix(c, 0);
    c->a = op_and(c, c->a, c->a);
    if (c->zf) c->mem[MOVE_FLAG] = c->a;
    CALL(0x9C36);
    pop_af(c);
  }
  c->ix = pop16(c);
  c->iy = pop16(c);
}

/* $9BDD: object A has moved to location B: move its contents with it. If
 * that took you with it (you were inside), and you are somewhere, look
 * around as character 0. Sets and clears the move flag ($9BDC). */
static void p_9bdd(Cpu *c) {
  set_hl(c, MOVE_FLAG);
  c->mem[get_hl(c)] = 0x01;
  CALL(0x9BE2);
  c->a = rd(c, get_hl(c));
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) return;
  push16(c, get_hl(c));
  c->a = rd(c, YOUR_LOC_OBJ);
  c->a = op_and(c, c->a, c->a);
  if (c->zf) goto L9C13;
  c->a = rd(c, ACTOR_ID);
  push_af(c);
  set_hl(c, rd16(c, ACTOR_REC));
  push16(c, get_hl(c));
  set_hl(c, 0xC11B);
  wr16(c, ACTOR_REC, get_hl(c));
  c->a = op_xor(c, c->a, c->a);
  c->mem[ACTOR_ID] = c->a;
  c->a = c->b;
  c->mem[LOOK_ARG] = c->a;
  CALL(0x9C05);
  CALL(0x9C08);
  set_hl(c, pop16(c));
  wr16(c, ACTOR_REC, get_hl(c));
  pop_af(c);
  c->mem[ACTOR_ID] = c->a;
L9C13:
  set_hl(c, pop16(c));
  c->a = op_xor(c, c->a, c->a);
  c->mem[get_hl(c)] = c->a;
}

/* $9C41: the room left in location A: its capacity less the sizes of the
 * objects there (that are in one location only), 0 if over. */
static void p_9c41(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  push16(c, get_bc(c));
  c->b = c->a;
  CALL(0x9C47);
  c->a = at_ix(c, 1);
  c->c = c->a;
  c->ix = OBJ_INDEX_M3;
  for (;;) {
    CALL(0x9C52);
    if (c->zf) break;
    c->a = at_iy(c, 0);
    op_cp(c, 0x01);
    if (!c->zf) continue;
    c->a = c->b;
    op_cp(c, at_iy(c, 0x10));
    if (!c->zf) continue;
    c->a = c->c;
    c->a = op_sub(c, c->a, at_iy(c, 2), 0);
    if (c->cf) {
      c->c = 0x00;
      break;
    }
    c->c = c->a;
  }
  c->a = c->c;
  set_bc(c, pop16(c));
  c->iy = pop16(c);
  c->ix = pop16(c);
}

/* $9C8A: C if object A is inside (at any depth) object (HL). Otherwise NC
 * with A = the outermost object, Z if that is object 0. Loops for ever if
 * the containers form a cycle. */
static void p_9c8a(Cpu *c) {
  for (;;) {
    cpu_call_at(c, 0x9C8A);
    push_af(c);
    c->a = at_ix(c, 1);
    op_cp(c, 0xFF);
    if (c->zf) {
      pop_af(c);
      c->a = op_and(c, c->a, c->a);
      return;
    }
    c->ix = pop16(c);
    op_cp(c, rd(c, get_hl(c)));
    if (c->zf) break;
  }
  op_scf(c);
}

/* $9C7E: C if object A is inside object (HL); $FF: C (and Z). */
static void p_9c7e(Cpu *c) {
  op_cp(c, 0xFF);
  op_scf(c);
  if (c->zf) return;
  push16(c, c->ix);
  CALL(0x9C84);
  c->ix = pop16(c);
}

/* $9C7B: is object A carried (at any depth) by the actor? */
static void p_9c7b(Cpu *c) {
  set_hl(c, ACTOR_ID);
  p_9c7e(c);
}

/* $9C78: is the first object of the command carried by the actor? */
static void p_9c78(Cpu *c) {
  c->a = rd(c, OBJ1);
  p_9c7b(c);
}

/* ---------- random numbers ---------- */

/* $9CA8 CalcRandom: a random number from 0-C in A, less C, where C is A
 * on entry and the range is 2A (or $FF). Mixes the seed with bytes read
 * through a pointer that walks all 64K, by 256 at a time ($B712, high byte
 * at $B713 counted first), plus DE. */
/* The clean generator: a 16-bit xorshift kept in the same two bytes as
 * the original's pointer, started from the game's seed, giving one byte. */
static uint8_t clean_random_byte(Cpu *c) {
  uint16_t x = (uint16_t)(c->mem[RNG_PTR] << 8 | c->mem[RNG_PTR + 1]);
  if (x == 0) x = (uint16_t)(0xACE1 ^ c->mem[RNG_SEED] ^ c->mem[RNG_SEED] << 8);
  x ^= (uint16_t)(x << 7);
  x ^= (uint16_t)(x >> 9);
  x ^= (uint16_t)(x << 8);
  c->mem[RNG_PTR] = x >> 8;
  c->mem[RNG_PTR + 1] = (uint8_t)x;
  return (uint8_t)(x ^ x >> 8);
}

static void p_calc_random(Cpu *c) {
  push16(c, c->ix);
  push16(c, get_bc(c));
  c->c = c->a;
  c->a = op_sla(c, c->a);
  if (c->cf) c->a = 0xFF;
  c->b = c->a;
  if (port_clean_rng) {
    /* Same interface: never the previous value; then the same scaling. */
    do c->a = clean_random_byte(c);
    while (c->a == rd(c, RNG_SEED));
    op_cp(c, rd(c, RNG_SEED));
  } else for (;;) {
    if (++c->mem[RNG_PTR + 1] == 0) ++c->mem[RNG_PTR];
    c->ix = rd16(c, RNG_PTR);
    c->a = rd(c, RNG_SEED);
    c->a = op_add(c, c->a, at_ix(c, 0), c->cf);
    c->ix = op_add16(c, c->ix, get_de(c));
    c->a = op_xor(c, c->a, at_ix(c, 1));
    push16(c, get_hl(c));
    op_cp(c, rd(c, RNG_SEED));
    pop16(c);
    if (!c->zf) break;
  }
  c->mem[RNG_SEED] = c->a;
  for (;;) {
    op_cp(c, c->b);
    if (c->cf || c->zf) break;
    c->a = op_srl(c, c->a);
  }
  c->a = op_sub(c, c->a, c->c, 0);
  set_bc(c, pop16(c));
  c->ix = pop16(c);
}

/* $9C9F GetRandomNum: CalcRandom, made positive. */
static void p_get_random(Cpu *c) {
  cpu_call_at(c, 0x9C9F);
  if (!op_bit(c, 7, c->a)) return;
  op_neg(c);
}

/* ---------- weights and contents ---------- */

/* $9D00: add to C the sizes (B=1) or weights (B=0, counting what they
 * contain) of the objects inside object A. Overflow: C=$FF. */
static void p_9d00(Cpu *c) {
  push16(c, c->ix);
  c->ix = OBJ_INDEX;
  for (;;) {
    CALL(0x9D06);
    if (c->zf) goto L9D2F;
    op_cp(c, at_iy(c, 1));
    if (!c->zf) continue;
    push_af(c);
    c->a = op_sub(c, c->a, c->a, 0);
    op_cp(c, c->b);
    c->a = c->c;
    if (c->zf) {
      c->a = op_add(c, c->a, at_iy(c, 3), 0);
      if (c->pf) goto L9D32;
      c->c = c->a;
      c->a = at_ix(c, 0);
      CALL(0x9D29);
    } else {
      c->a = op_add(c, c->a, at_iy(c, 2), 0);
      if (c->pf) goto L9D32;
      c->c = c->a;
    }
    pop_af(c);
  }
L9D32:
  pop_af(c);
  c->c = 0xFF;
L9D2F:
  c->ix = pop16(c);
}

/* $9CF0: the total in A, keeping BC, IX and IY. */
static void l9cf0(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  c->c = 0x00;
  CALL(0x9CF6);
  c->a = c->c;
  c->iy = pop16(c);
  c->ix = pop16(c);
  set_bc(c, pop16(c));
}

/* $9CE8: total size of what object A contains. */
static void p_9ce8(Cpu *c) {
  push16(c, get_bc(c));
  c->b = 0x01;
  l9cf0(c);
}

/* $9CED: total weight of what object A contains, at any depth. */
static void p_9ced(Cpu *c) {
  push16(c, get_bc(c));
  c->b = 0x00;
  l9cf0(c);
}

/* $9D37 GetObjectLocationInIX: IX = the record of the actor's location.
 * AF kept. */
static void p_get_object_location(Cpu *c) {
  push_af(c);
  c->ix = rd16(c, ACTOR_REC);
  c->a = at_ix(c, 0x10);
  CALL(0x9D3F);
  pop_af(c);
}

/* $9D44: unless $B6FA is 1, set $B6FB to $B6FA+1 and return from the
 * caller as well (POP BC; RET). */
static void p_9d44(Cpu *c) {
  c->a = rd(c, VAR_B6FA);
  op_cp(c, 0x01);
  if (c->zf) return;
  c->a = op_inc(c, c->a);
  c->mem[VAR_B6FB] = c->a;
  set_bc(c, pop16(c)); /* POP BC: the caller's return address */
  cpu_tail(c, 0x9D4F);  /* RET, to the caller's caller */
}

/* $9D4F: the RET after $9D44's POP BC, as a continuation. */
static void p_9d4f(Cpu *c) { (void)c; }

/* $9D53: object A has gone: what was in it goes to its container, except
 * liquids (attribute bit 1), which evaporate (no location, no container,
 * invisible) with a message. B is left as A's container. */
static void p_9d53(Cpu *c) {
  push16(c, c->iy);
  push16(c, c->ix);
  push16(c, get_hl(c));
  CALL(0x9D58);
  c->b = at_ix(c, 1);
  c->ix = OBJ_INDEX_M3;
  for (;;) {
    CALL(0x9D62);
    if (c->zf) break;
    op_cp(c, at_iy(c, 1));
    if (!c->zf) continue;
    if (!op_bit_at(c, 1, at_iy(c, 7), (uint16_t)(c->iy + 7))) {
      c->mem[(uint16_t)(c->iy + 1)] = c->b;
      continue;
    }
    /* BIT n,(IY+d): bits 5/3 from the high byte of IY+d */
    push_af(c);
    c->mem[(uint16_t)(c->iy + 0x10)] = 0x00;
    c->mem[(uint16_t)(c->iy + 1)] = 0xFF;
    c->mem[(uint16_t)(c->iy + 7)] &= 0x7F;
    CALL(0x9D7F);
    set_hl(c, 0xB143); /* "evaporates" */
    CALL(0x9D85);
    pop_af(c);
  }
  set_hl(c, pop16(c));
  c->ix = pop16(c);
  c->iy = pop16(c);
}

/* $9D50: the same for the first object of the command. */
static void p_9d50(Cpu *c) {
  c->a = rd(c, OBJ1);
  p_9d53(c);
}

/* $9D97 ObjectCount: how many visible objects are directly inside A. */
static void p_object_count(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  push16(c, get_bc(c));
  c->b = 0x00;
  c->ix = OBJ_INDEX_M3;
  for (;;) {
    CALL(0x9DA2);
    if (c->zf) break;
    op_cp(c, at_iy(c, 1));
    if (!c->zf) continue;
    if (!op_bit_at(c, 7, at_iy(c, 7), (uint16_t)(c->iy + 7))) continue;
    c->b = op_inc(c, c->b);
  }
  c->a = c->b;
  set_bc(c, pop16(c));
  c->iy = pop16(c);
  c->ix = pop16(c);
}

/* $9DBD IndexIdTable: IX = the 3-byte record with key A in the table at
 * IX, or its $FF end (A=$FF, Z). Uses BC' DE' HL'. */
static void p_index_id_table(Cpu *c) {
  unsigned r = 7; /* opcode fetches, for R (the game's seed comes from it) */
  exx(c);
  push16(c, c->ix);
  uint16_t hl = pop16(c);
  c->b = c->a;
  c->e = 0x03;
  c->d = 0x00;
  for (;;) {
    r += 3;
    c->a = c->mem[hl];
    op_cp(c, c->b);
    if (c->zf) break;
    r += 2;
    op_cp(c, 0xFF);
    if (c->zf) break;
    r += 2;
    hl = op_add16(c, hl, get_de(c));
  }
  set_hl(c, hl);
  push16(c, hl);
  c->ix = pop16(c);
  op_cp(c, 0xFF);
  exx(c);
  add_r(c, r + 6);
}

/* ---------- matching words to objects ---------- */

/* $9DD9: from the index record at IX, find the next object whose name
 * matches the words being parsed ($71F3), of the kind $B710 asks for
 * (0 not a character, 1 a character, 2 either), and visible to the actor
 * unless $B70F is set. Returns its id in A ($FF at the end), IX at it. */
static void p_9dd9(Cpu *c) {
  push16(c, get_bc(c));
  push16(c, get_de(c));
  push16(c, c->iy);
  c->iy = rd16(c, ACTOR_REC);
  c->d = at_iy(c, 0x10);
  c->a = rd(c, MATCH_MODE);
  c->e = c->a;
  for (;;) {
    CALL(0x9DE8);
    if (c->zf) break;
    c->a = 0x02;
    op_cp(c, c->e);
    if (!c->zf) {
      c->a = at_iy(c, 7);
      c->a = op_and(c, c->a, 0x48);
      op_cp(c, 0x40);
      c->a = 0x00;
      if (c->zf) c->a = op_inc(c, c->a);
      op_cp(c, c->e);
      if (!c->zf) continue;
    }
    set_bc(c, 0x0008);
    push16(c, c->iy);
    c->iy = op_add16(c, c->iy, get_bc(c));
    CALL(0x9E08);
    c->iy = pop16(c);
    if (!c->zf) continue;
    c->a = rd(c, VAR_B70F);
    c->a = op_and(c, c->a, c->a);
    if (!c->zf) break;
    c->a = at_ix(c, 0);
    CALL(0x9E18);
    if (!c->zf) break;
  }
  c->a = at_ix(c, 0);
  c->iy = pop16(c);
  set_de(c, pop16(c));
  set_bc(c, pop16(c));
}

/* $9E2B: exchange IX and IY. */
static void p_9e2b(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  c->ix = pop16(c);
  c->iy = pop16(c);
}

/* $9E25: $9E40 with IX and IY the other way round. */
static void p_9e25(Cpu *c) {
  cpu_call_at(c, 0x9E25);
  CALL(0x9E28);
  p_9e2b(c);
}

/* $9E40: can the character at IX see object A (record IY)? NZ if so:
 * it is visible, and either is the character's container, or shares its
 * innermost closed container, or (in none) is at the character's
 * location. */
static void p_9e40(Cpu *c) {
  if (!op_bit_at(c, 7, at_iy(c, 7), (uint16_t)(c->iy + 7))) return;
  push16(c, c->iy);
  push16(c, c->ix);
  push16(c, get_bc(c));
  c->b = c->a;
  c->c = at_ix(c, 0x10);
  push16(c, c->iy);
  CALL(0x9E50);
  op_cp(c, c->b);
  c->b = c->a;
  c->ix = pop16(c);
  if (c->zf) goto L9E72;
  CALL(0x9E59);
  op_cp(c, c->b);
  if (!c->zf) goto L9E6F;
  c->a = op_inc(c, c->a);
  if (!c->zf) goto L9E72;
  c->b = at_iy(c, 0);
  c->a = c->c;
  do {
    op_cp(c, at_iy(c, 0x10));
    if (c->zf) goto L9E72;
    c->iy++;
  } while (--c->b);
L9E6F:
  c->a = op_xor(c, c->a, c->a);
  goto L9E74;
L9E72:
  c->a = op_or(c, c->a, 0x01);
L9E74:
  set_bc(c, pop16(c));
  c->ix = pop16(c);
  c->iy = pop16(c);
}

/* $9E34: can the actor see object A (record IY)? */
static void p_9e34(Cpu *c) {
  push16(c, c->ix);
  c->ix = rd16(c, ACTOR_REC);
  CALL(0x9E3A);
  c->ix = pop16(c);
}

/* $9E7A ContainerLit: the innermost container of the object at IX that
 * cannot be seen into (attributes $28 clear), walking out through those
 * that can; A=$FF (Z) if there is none. IX kept. The original keeps the
 * container's id in AF' (EX AF,AF'), which it leaves changed; nothing
 * reads AF' before writing it, and it is not modelled here. Loops for ever
 * if open containers form a cycle. */
static void p_container_lit(Cpu *c) {
  push16(c, c->ix);
  for (;;) {
    c->a = at_ix(c, 1);
    op_cp(c, 0xFF);
    if (c->zf) break;
    SavedAF af = save_af(c); /* EX AF,AF' */
    c->a = at_ix(c, 1);
    CALL(0x9E87);
    c->a = at_ix(c, 7);
    c->a = op_and(c, c->a, 0x28);
    if (!c->zf) continue;
    restore_af(c, af); /* EX AF,AF' */
    break;
  }
  c->ix = pop16(c);
}

/* ---------- locations and exits ---------- */

/* $9E95: IX = the actor's location record + 7 (before its exits). */
static void p_9e95(Cpu *c) {
  push16(c, get_de(c));
  CALL(0x9E96);
  set_de(c, 0x0007);
  c->ix = op_add16(c, c->ix, get_de(c));
  set_de(c, pop16(c));
}

/* $9EA0: from the exit record at IX, find the next exit whose destination
 * matches the words being parsed; A = the destination ($FF at the end). */
static void p_9ea0(Cpu *c) {
  push16(c, c->iy);
  push16(c, get_de(c));
  set_de(c, 0x0002);
  for (;;) {
    CALL(0x9EA6);
    if (c->zf) break;
    c->a = at_ix(c, 2);
    push16(c, c->ix);
    CALL(0x9EB0);
    push16(c, c->ix);
    c->iy = pop16(c);
    c->ix = pop16(c);
    c->iy = op_add16(c, c->iy, get_de(c));
    CALL(0x9EBB);
    if (c->zf) {
      c->a = at_ix(c, 2);
      break;
    }
  }
  set_de(c, pop16(c));
  c->iy = pop16(c);
}

/* $9ED6: print the name at IY (adjective, adjective, noun; just the noun
 * if $B703 is set). AF and DE kept. */
static void p_9ed6(Cpu *c) {
  push_af(c);
  push16(c, get_de(c));
  c->a = rd(c, VAR_B703);
  op_cp(c, 0x00);
  if (c->zf) {
    c->d = at_iy(c, 1);
    c->e = at_iy(c, 0);
    CALL(0x9EE5);
    c->e = at_iy(c, 2);
    c->d = at_iy(c, 3);
    CALL(0x9EEE);
    c->e = at_iy(c, 4);
    c->d = at_iy(c, 5);
    CALL(0x9EF7);
  }
  c->e = at_iy(c, 0);
  c->d = at_iy(c, 1);
  c->a = op_or(c, c->d, c->e);
  CALL_IF(0x9F02); /* CALL NZ,$7478 */
  set_de(c, pop16(c));
  pop_af(c);
}

/* $9EC7: print the name of the object at IY. */
static void p_9ec7(Cpu *c) {
  push16(c, c->iy);
  push16(c, get_de(c));
  set_de(c, 0x0008);
  c->iy = op_add16(c, c->iy, get_de(c));
  CALL(0x9ECF);
  set_de(c, pop16(c));
  c->iy = pop16(c);
}

/* $9F08: find an exit in direction A from the actor's location (one with
 * a destination). A = A and Z if found, else $FF (Z); IX at the record. */
static void p_9f08(Cpu *c) {
  push16(c, get_bc(c));
  push16(c, c->iy);
  c->b = c->a;
  CALL(0x9F0C);
  for (;;) {
    CALL(0x9F0F);
    if (c->zf) break;
    c->a = at_ix(c, 2);
    c->a = op_and(c, c->a, c->a);
    if (c->zf) continue;
    c->a = at_ix(c, 0);
    op_cp(c, c->b);
    if (c->zf) break;
  }
  c->iy = pop16(c);
  set_bc(c, pop16(c));
}

/* $9F30: find an exit from the actor's location whose byte d ($9F42, 1:
 * door, 2: destination) is A. A = A and Z if found, else $FF (Z). */
static void l9f30(Cpu *c) {
  push16(c, get_bc(c));
  push16(c, c->iy);
  c->b = c->a;
  CALL(0x9F38);
  for (;;) {
    CALL(0x9F3B);
    if (c->zf) break;
    c->a = at_ix(c, (int8_t)rd(c, EXIT_DISP));
    op_cp(c, c->b);
    if (c->zf) break;
  }
  c->iy = pop16(c);
  set_bc(c, pop16(c));
}

/* $9F28: find the exit through door A. */
static void p_9f28(Cpu *c) {
  push_af(c);
  c->mem[EXIT_DISP] = 0x01; /* LD A,$01; LD ($9F42),A */
  pop_af(c);
  l9f30(c);
}

/* $9F25: the exit through the first object of the command. */
static void p_9f25(Cpu *c) {
  c->a = rd(c, OBJ1);
  p_9f28(c);
}

/* $9F2D: find the exit to location A. */
static void p_9f2d(Cpu *c) {
  push_af(c);
  c->mem[EXIT_DISP] = 0x02;
  pop_af(c);
  l9f30(c);
}

/* $9F4A: run the action at HL with the two objects of the command (and
 * their records) swapped, then put them back. */
static void p_9f4a(Cpu *c) {
  set_de(c, rd16(c, OBJ1_REC));
  c->iy = rd16(c, OBJ2_REC);
  wr16(c, OBJ1_REC, c->iy);
  wr16(c, OBJ2_REC, get_de(c));
  set_bc(c, rd16(c, OBJ1));
  c->a = c->b;
  c->mem[OBJ1] = c->a;
  c->a = c->c;
  c->mem[OBJ2] = c->a;
  CALL(0x9F66);
  wr16(c, OBJ1, get_bc(c));
  wr16(c, OBJ1_REC, get_de(c));
  wr16(c, OBJ2_REC, c->iy);
}

/* $9F76: if $B6FA is 1, go on at $712B; else clear $B6FB (A=0, Z). */
static void p_9f76(Cpu *c) {
  c->a = rd(c, VAR_B6FA);
  c->a = op_dec(c, c->a);
  if (c->zf) {
    cpu_tail(c, 0x712B);
    return;
  }
  c->a = op_sub(c, c->a, c->a, 0);
  c->mem[VAR_B6FB] = c->a;
}

/* $9F82 ObjectFirstLocation: the location of object A if it is in exactly
 * one, else $FF (NZ). */
static void p_object_first_location(Cpu *c) {
  op_cp(c, 0xFF);
  if (c->zf) return;
  CALL(0x9F85);
  c->a = 0x01;
  op_cp(c, at_ix(c, 0));
  c->a = 0xFF;
  if (!c->zf) return;
  c->a = at_ix(c, 0x10);
}

/* ---------- listing objects ---------- */

/* $9FC7: list the objects in container A ($FF: those lying at location
 * B) visible to the actor, indented by D, with what they contain
 * (recursively, indented 2 more). Counts them in C. A and HL kept. */
static void p_9fc7(Cpu *c) {
  push16(c, get_hl(c));
  c->l = c->a;
  c->a = rd(c, INDENT);
  c->h = c->a;
  c->a = c->d;
  c->mem[INDENT] = c->a;
  c->a = c->l;
  ex_sp_hl(c);
  push16(c, c->ix);
  c->ix = OBJ_INDEX_M3;
  for (;;) {
    CALL(0x9FD9);
    if (c->zf) break;
    op_cp(c, at_iy(c, 1));
    if (!c->zf) continue;
    push_af(c);
    c->a = op_inc(c, c->a);
    c->e = at_iy(c, 0);
    if (c->zf) {
      c->a = 0x01;
      op_cp(c, c->e);
      if (!c->zf) goto LA03D;
    }
    c->a = at_iy(c, 0x10);
    op_cp(c, c->b);
    if (!c->zf) {
      c->e = op_dec(c, c->e);
      if (c->zf) goto LA03D;
      c->a = at_iy(c, 0x11);
      op_cp(c, c->b);
      if (!c->zf) goto LA03D;
    }
    c->a = rd(c, ACTOR_ID);
    op_cp(c, at_ix(c, 0));
    if (c->zf) {
      c->a = 0x04;
      op_cp(c, c->d);
      if (c->zf) goto LA03D;
    }
    c->a = at_ix(c, 0);
    CALL(0xA00E);
    if (c->zf) goto LA03D;
    c->c = op_inc(c, c->c);
    c->a = op_sub(c, c->a, c->a, 0);
    c->mem[VAR_B704] = c->a;
    c->mem[VAR_B703] = c->a;
    CALL(0xA01B);
    c->a = rd(c, ACTOR_ID);
    op_cp(c, at_ix(c, 0));
    if (c->zf) {
      CALL(0xA041);
      goto LA03D;
    }
    c->a = 0x2E;
    CALL(0xA028);
    c->a = at_ix(c, 0);
    CALL(0xA02E);
    if (c->cf) goto LA03D;
    c->a = at_ix(c, 0);
    push16(c, get_de(c));
    c->d = op_inc(c, c->d);
    c->d = op_inc(c, c->d);
    CALL(0xA039);
    set_de(c, pop16(c));
  LA03D:
    pop_af(c);
  }
  c->ix = pop16(c);
  ex_sp_hl(c);
  c->a = c->h;
  c->mem[INDENT] = c->a;
  c->a = c->l;
  set_hl(c, pop16(c));
}

/* $9FAF: list what is in container A ($FF: at location B), or "nothing". */
static void p_9faf(Cpu *c) {
  push16(c, c->iy);
  push16(c, get_de(c));
  push16(c, get_bc(c));
  c->c = 0x00;
  c->d = 0x04;
  CALL(0x9FB7);
  c->a = op_sub(c, c->a, c->a, 0);
  op_cp(c, c->c);
  set_hl(c, 0xB33B); /* "nothing" */
  CALL_IF(0x9FBF);   /* CALL Z,$72DD */
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  c->iy = pop16(c);
}

/* $9F94: "You see :" and what is at the actor's location. IY, AF, BC
 * kept. */
static void p_9f94(Cpu *c) {
  push16(c, c->iy);
  push_af(c);
  push16(c, get_bc(c));
  set_hl(c, 0xB003); /* "You see :" */
  CALL(0x9F9B);
  c->a = 0xFF;
  c->iy = rd16(c, ACTOR_REC);
  c->b = at_iy(c, 0x10);
  CALL(0x9FA7);
  set_bc(c, pop16(c));
  pop_af(c);
  c->iy = pop16(c);
}

/* $A050: if object A can be seen into and holds visible objects, print
 * what it holds ("... is carrying" for a character, else its own phrase
 * with "there"), NC; else a new line and C. */
static void p_a050(Cpu *c) {
  push16(c, c->ix);
  push16(c, get_bc(c));
  push16(c, get_de(c));
  c->c = c->a;
  CALL(0xA055);
  c->a = at_ix(c, 7);
  c->a = op_and(c, c->a, 0x28);
  if (c->zf) goto LA097;
  c->a = c->c;
  CALL(0xA060);
  op_cp(c, 0x00);
  if (c->zf) goto LA097;
  /* The messages take their parameters off the stack. */
  if (op_bit_at(c, 6, at_ix(c, 7), (uint16_t)(c->ix + 7))) {
    c->a = c->c;
    push_af(c);
    set_hl(c, 0xADF9); /* "<A> is carrying" */
  } else {
    set_hl(c, 0x039B);
    c->a = op_dec(c, c->a);
    if (!c->zf) set_hl(c, 0x0065);
    push16(c, get_hl(c));
    c->l = at_ix(c, 8);
    c->a = at_ix(c, 9);
    c->a = op_and(c, c->a, 0x0F);
    c->h = c->a;
    push16(c, get_hl(c));
    CALL(0xA088);
    set_hl(c, 0xAFE0); /* "<...> there <...>" */
  }
  CALL(0xA08E);
  c->a = op_and(c, c->a, c->a);
LA092:
  set_de(c, pop16(c));
  set_bc(c, pop16(c));
  c->ix = pop16(c);
  return;
LA097:
  CALL(0xA097);
  op_scf(c);
  goto LA092;
}

/* $A09D: print the "contains" phrase of the object at IX (bits 4-7 of
 * +$04 index the 4-byte messages at $AFCA). */
static void p_a09d(Cpu *c) {
  set_hl(c, MSG_TABLE_A09D);
  c->a = at_ix(c, 4);
  op_rlca(c);
  op_rlca(c);
  c->a = op_and(c, c->a, 0x3C);
  c->e = c->a;
  c->d = 0x00;
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  cpu_tail(c, 0x72DD);
}

/* $A0AE: IX = location A's record + 7, BC = 3 (to step through exits). */
static void p_a0ae(Cpu *c) {
  cpu_call_at(c, 0xA0AE);
  set_bc(c, 0x0007);
  c->ix = op_add16(c, c->ix, get_bc(c));
  set_bc(c, 0x0003);
}

/* $A0BD: DE = word (A & $7F) of the table at HL. */
static void p_a0bd(Cpu *c) {
  c->e = c->a & 0x7F;
  c->d = 0x00;
  uint16_t hl = op_add16(c, get_hl(c), get_de(c));
  hl = op_add16(c, hl, get_de(c));
  c->e = c->mem[hl];
  hl++;
  c->d = c->mem[hl];
  set_hl(c, hl);
}

/* $A0BA: DE = the dictionary word for direction A. */
static void p_a0ba(Cpu *c) {
  set_hl(c, DIR_WORDS);
  p_a0bd(c);
}

/* $A0C8: for each exit of location A with a visible door, print "to the
 * <direction> there is <door>". */
static void p_a0c8(Cpu *c) {
  push16(c, get_bc(c));
  push16(c, get_de(c));
  push16(c, c->iy);
  push16(c, c->ix);
  CALL(0xA0CE);
  c->ix = op_add16(c, c->ix, get_bc(c));
  push16(c, c->ix);
  c->iy = pop16(c);
  do {
    c->a = op_sub(c, c->a, c->a, 0);
    op_cp(c, at_iy(c, 1));
    if (!c->zf) {
      c->a = at_iy(c, 1);
      CALL(0xA0E0);
      if (op_bit_at(c, 7, at_ix(c, 7), (uint16_t)(c->ix + 7))) {
        set_de(c, 0x0008);
        c->ix = op_add16(c, c->ix, get_de(c));
        push16(c, c->ix); /* the door's name, taken by the last message */
        c->a = at_iy(c, 0);
        CALL(0xA0F3);
        op_cp(c, 0x09);
        if (c->cf) {
          set_hl(c, 0xAE17); /* "to the" */
          CALL(0xA107);
        } else if (c->zf) {
          set_de(c, 0x07B5);
        } else {
          set_de(c, 0x082B);
        }
        CALL(0xA10A);
        set_hl(c, 0xB013); /* "there is <door>" */
        CALL(0xA110);
      }
    }
    c->iy = op_add16(c, c->iy, get_bc(c));
    c->a = 0xFF;
    op_cp(c, at_iy(c, 0));
  } while (!c->zf);
  c->ix = pop16(c);
  c->iy = pop16(c);
  set_de(c, pop16(c));
  set_bc(c, pop16(c));
}

/* $A124: move IX on (by BC) to the next exit with a destination but no
 * door byte... ($00 in +$01, nonzero +$00); Z at the $FF end. */
static void p_a124(Cpu *c) {
  for (;;) {
    c->ix = op_add16(c, c->ix, get_bc(c));
    c->a = 0xFF;
    op_cp(c, at_ix(c, 0));
    if (c->zf) return;
    c->a = op_xor(c, c->a, c->a);
    op_cp(c, at_ix(c, 1));
    if (!c->zf) continue;
    op_cp(c, at_ix(c, 0));
    if (c->zf) continue;
    return;
  }
}

const PortRoutine objects_routines[] = {
    {0x9A85, "FindCharacter", p_9a85, FULL},
    {0x9AA0, "CharSetEntry", p_9aa0, FULL},
    {0x9ACD, "MentionObject", p_9acd, FULL},
    {0x9B02, "Action_None", p_action_none, FULL},
    {0x9B44, "ActorIsObject", p_9b44, FULL},
    {0x9B6C, "TriggerAction", p_trigger_action, FULL},
    {0x9B80, "TriggerActJump", p_trigger_action_jump, FULL},
    {0x9B81, "RecordTable", p_9b81, FULL},
    {0x9B93, "Step3ByteTable", p_step3, FULL},
    {0x9BA9, "Step3Next", p_step3_next, FULL},
    {0x9BB1, "LocateLocation", p_locate_location, FULL},
    {0x9BCA, "LocateObject", p_locate_object, FULL},
    {0x9BDD, "MoveContents", p_9bdd, FULL},
    {0x9C17, "SetContentsLoc", p_9c17, FULL},
    {0x9C41, "LocationRoom", p_9c41, FULL},
    {0x9C78, "Obj1Carried", p_9c78, FULL},
    {0x9C7B, "ObjCarried", p_9c7b, FULL},
    {0x9C7E, "ObjInside", p_9c7e, FULL},
    {0x9C8A, "ObjInsideLoop", p_9c8a, FULL},
    {0x9C9F, "GetRandomNum", p_get_random, FULL},
    {0x9CA8, "CalcRandom", p_calc_random, FULL},
    {0x9CE8, "ContentsSize", p_9ce8, FULL},
    {0x9CED, "ContentsWeight", p_9ced, FULL},
    {0x9D00, "SumContents", p_9d00, FULL},
    {0x9D37, "GetObjLocInIX", p_get_object_location, FULL},
    {0x9D44, "OutputCheck", p_9d44, FULL},
    {0x9D4F, "OutputCheckRet", p_9d4f, FULL},
    {0x9D50, "EmptyObj1", p_9d50, FULL},
    {0x9D53, "EmptyObject", p_9d53, FULL},
    {0x9D97, "ObjectCount", p_object_count, FULL},
    {0x9DBD, "IndexIdTable", p_index_id_table, FULL},
    {0x9DD9, "MatchObject", p_9dd9, FULL},
    {0x9E25, "CanSeeSwapped", p_9e25, FULL},
    {0x9E2B, "SwapIXIY", p_9e2b, FULL},
    {0x9E34, "ActorCanSee", p_9e34, FULL},
    {0x9E40, "CanSee", p_9e40, FULL},
    {0x9E7A, "ContainerLit", p_container_lit, FULL},
    {0x9E95, "ActorLocExits", p_9e95, FULL},
    {0x9EA0, "MatchExit", p_9ea0, FULL},
    {0x9EC7, "PrintObjName", p_9ec7, FULL},
    {0x9ED6, "PrintName", p_9ed6, FULL},
    {0x9F08, "FindExitDir", p_9f08, FULL},
    {0x9F25, "FindExitObj1", p_9f25, FULL},
    {0x9F28, "FindExitDoor", p_9f28, FULL},
    {0x9F2D, "FindExitTo", p_9f2d, FULL},
    {0x9F4A, "SwapAction", p_9f4a, FULL},
    {0x9F76, "OutputDone", p_9f76, FULL},
    {0x9F82, "ObjFirstLoc", p_object_first_location, FULL},
    {0x9F94, "YouSee", p_9f94, FULL},
    {0x9FAF, "ListContents", p_9faf, FULL},
    {0x9FC7, "ListObjects", p_9fc7, FULL},
    {0xA050, "ListInside", p_a050, FULL},
    {0xA09D, "ContainsMsg", p_a09d, FULL},
    {0xA0AE, "LocExits", p_a0ae, FULL},
    {0xA0BA, "DirectionWord", p_a0ba, FULL},
    {0xA0BD, "TableWord", p_a0bd, FULL},
    {0xA0C8, "ListDoors", p_a0c8, FULL},
    {0xA124, "NextOpenExit", p_a124, FULL},
    {0, NULL, NULL, 0},
};
