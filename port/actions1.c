/* Actions: look, put down, pick up, directions, run, enter, follow,
 * throw, talk, shoot, death, inventory, attack, give ($8C4B-$93D9).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * Actions are reached from the action table at $C730 (verb -> routine) and
 * from the per-object handler lists in the object records, both through
 * TriggerAction ($9B6C: CALL $9B80, JP (HL)), so each is entered with a
 * return address on top of the stack. Most run twice: first with
 * DO_IT != 1, to find out whether the action can be done (the "trial"
 * check at $9D44 then abandons the action with RESULT set), then with
 * DO_IT == 1 to do it.
 *
 * Labels keep the original addresses. Calls to this file's own routines
 * are made on the real stack (call_near / call_local), as several of them
 * pop their caller's return address to abandon it. */
#include <stddef.h>

#include "addrs.h"
#include "cpu.h"
#include "routines.h"

/* The sentence being carried out. */
#define ACTION 0xB6E7     /* verb (action ID; $01-$0A are the directions) */
#define OBJ1 0xB6E8       /* direct object ID ($FF none) */
#define OBJ2 0xB6E9       /* indirect object ID ($FF none) */
#define ACTOR 0xB6EA      /* the character acting ($00 the player) */
#define SCORE 0xB6F7      /* completion percentage, 16 bits */
#define VAR_B6F9 0xB6F9   /* unknown: nonzero makes characters always answer */
#define DO_IT 0xB6FA      /* 1: carry the action out; otherwise only test it */
#define RESULT 0xB6FB     /* set by the test pass ($9D44), cleared on failure */
#define WEAPON_NAME 0xB6FC /* name for the attack messages ($026B: bare hands) */
#define OBJ1_REC 0xB708   /* object record of OBJ1 */
#define OBJ2_REC 0xB70A   /* object record of OBJ2 */
#define ACTOR_REC 0xB70C  /* object record of ACTOR */

/* Object records ($C11B..): +0 the number of locations it is in (listed
 * from +$10; $01 for a single object, $02 for a door), +1 the object it
 * is in or on ($FF: none, lying in a location), +2 its size (or, for a
 * container, its capacity), +3 weight / carrying strength, +4 attitude
 * bits (bits 4-6: whom it fights), +5 strength, +6 defence, +7 flags
 * (bit 1 vanishes when put down, bit 2, bit 3 dead/broken, bit 5 open,
 * bit 6 animate), +8/+9 name, +$10 location(s), then the object's own
 * action handlers (3-byte records: verb, address; $FF ends). */
#define PLAYER_REC 0xC11B
#define ROPE_REC 0xC366   /* the rope (object $12): things tied to it go with it */
#define ARROW_REC 0xC4EA
#define OBJ_15_REC 0xC454
#define OBJ_16_REC 0xC469

/* Moving. */
#define LOC_REC 0x8D99    /* record of the location being entered */
#define DEST 0x8D9B       /* the location being entered */
#define MOVER_SIZE 0x8D9C /* size of the mover with what it carries */
#define LOC_SCORES 0x8D6E /* locations worth a one-off percentage */
#define LOC_EVENTS 0xC78E /* events run when the player enters a location */
#define DAMAGE_MSGS 0x9226 /* attack messages, by how close the fight was */

/* Messages. */
#define MSG_YOU_ARE_IN 0xAFFC   /* "You are in" */
#define MSG_YOU_ARE_IN_ARG 0xAFFD
#define MSG_YOU_SEE 0xB003      /* "You see :" */
#define MSG_NOT_CARRYING 0xADF1 /* "You are not carrying it" */
#define MSG_EVAPORATES 0xB142
#define MSG_NOT_IN_IT 0xAFB5
#define MSG_TOO_HEAVY 0xAE04
#define MSG_TOO_MUCH 0xAE0C     /* "You are carrying too much" */
#define MSG_ALREADY 0xAE11      /* "You are already carrying" */
#define MSG_HIT_HEAD 0xAD7D     /* "but fall and hit your head" */
#define MSG_SMASH_SKULL 0xAD8A  /* "but fall and smash your skull" */
#define MSG_TOO_SMALL 0xAE23
#define MSG_TOO_FULL_ENTER 0xAE2E
#define MSG_DARK 0xAE1F         /* "it is dark" */
#define MSG_CANNOT_FOLLOW 0xAFE9
#define MSG_SAYS_NO 0xB1E3
#define MSG_NO_BOW 0xB121
#define MSG_ARROW_MISSES 0xB127
#define MSG_ARROW_HITS 0xB136
#define MSG_DEAD 0xAFF1         /* "You are dead" */
#define MSG_CARRYING 0xADF6     /* "You are carrying" */
#define MSG_NOTHING 0xB33B
#define MSG_CANNOT_KILL 0xAF5F
#define MSG_WASTED 0xAF50
#define MSG_CLEAVE 0xAE3A
#define MSG_TOO_FULL 0xAE1A
#define MSG_GLUTTONY 0xB146

/* Original routines. */
#define R_PRINT_MSG 0x72DD    /* print the message at HL (arguments on the stack) */
#define R_CANT 0x72CE
#define R_NEWLINE 0x8583
#define R_PRINT_NAME 0x74C1   /* print the word DE */
#define R_TALK 0x7EBA
#define R_SCORE 0x83F5        /* "you have mastered n%" */
#define R_DEAD_KEY 0x90DF     /* wait for a key, then restart */
#define R_LIST_CONTENTS 0x9630
#define R_LIST_HERE 0x962B
#define R_LOC_DESCRIBE 0x96A8
#define R_KILL 0x977F
#define R_TALK_TO 0x9A85
#define R_SPEAK_CHECK 0x9B81
#define R_TRIGGER 0x9B6C      /* run the code at HL */
#define R_LOCATION_REC 0x9BB1 /* IX = record of location A */
#define R_OBJECT_REC 0x9BCA   /* IX = record of object A */
#define R_MOVE_CONTENTS 0x9BDD
#define R_LOC_FULLNESS 0x9C41
#define R_CARRIED 0x9C78      /* C if OBJ1 is carried by the actor */
#define R_CARRIED_A 0x9C7B    /* C if object A is carried by the actor */
#define R_INSIDE 0x9C7E       /* object A inside object (HL)? */
#define R_RANDOM_ABS 0x9C9F
#define R_RANDOM 0x9CA8
#define R_SIZE_OF 0x9CE8      /* size of A with its contents */
#define R_WEIGHT_OF 0x9CED    /* weight of what A carries */
#define R_COUNT_IN 0x9D97     /* number of visible objects in A */
#define R_DROP_ALL 0x9D50
#define R_FIND 0x9DBD         /* IX = 3-byte record with key A in table IX */
#define R_EXIT 0x9F08         /* exit A of the actor's location */
#define R_EXIT_OF 0x9F25
#define R_EXIT_VIA 0x9F2D     /* exit through object A */
#define R_SWAP_TRIGGER 0x9F4A /* swap the objects and run the handler at HL */
#define R_FAIL 0x9F76         /* the action fails ("You can't", or a test result) */
#define R_LIST 0x9FAF         /* list the objects in A at location B */
#define R_STATE_MSG 0xA164    /* message A about object IY */
#define R_OBJ_MSG 0xA16C      /* message A about object IX */
#define R_REVIVE 0xA18C
#define R_IS_DEAD 0xA1C8
#define R_CAN_OPEN 0xA1F9
#define R_CLOSED 0xA5CA       /* Z if object IX is not open; A=5 */
#define R_SEE_INTO 0xA050
#define R_A09D 0xA09D         /* unknown: prints a location's article */
#define R_LIGHT 0x95ED        /* C if the player's location is dark */
#define R_LIGHT_SOURCE 0x95DF

/* ---------- helpers ---------- */

static void push_af(Cpu *c) { cpu_push_af(c); }
static void pop_af(Cpu *c) { cpu_pop_af(c); }
static uint8_t ix_rd(const Cpu *c, int off) { return c->mem[(uint16_t)(c->ix + off)]; }
static uint8_t iy_rd(const Cpu *c, int off) { return c->mem[(uint16_t)(c->iy + off)]; }
static void ix_wr(Cpu *c, int off, uint8_t v) { c->mem[(uint16_t)(c->ix + off)] = v; }
static void iy_wr(Cpu *c, int off, uint8_t v) { c->mem[(uint16_t)(c->iy + off)] = v; }

/* JP X to original code (or to another file's routine): the rest of the
 * routine is X's. */
static void tail(Cpu *c, uint16_t addr) { cpu_tail(c, addr); }
static void tail_msg(Cpu *c, uint16_t msg) {
  set_hl(c, msg);
  cpu_tail(c, R_PRINT_MSG);
}

/* CALL $9D44: on the test pass (DO_IT != 1), set RESULT and abandon the
 * action: $9D44 pops its own return address into BC and returns from the
 * routine that called it. Returns true when the caller must return. */
static bool trial(Cpu *c, uint16_t after) {
  wr16(c, (uint16_t)(c->sp - 2), after); /* the CALL's return address */
  c->a = c->mem[DO_IT];
  op_cp(c, 1);
  if (c->zf) return false;
  c->a = op_inc(c, c->a);
  c->mem[RESULT] = c->a;
  set_bc(c, after);
  return true;
}

/* CALL to one of this file's routines that returns to its caller. (False
 * if the original code it handed over to left the frame.) Calls to other
 * code use cpu_call_at on the original CALL instruction, so the real
 * return address is on the stack; cpu_call only where that CALL is at one
 * of this file's registered addresses (the hook would run the C version
 * again). */
static bool call_near(Cpu *c, void (*fn)(Cpu *), uint16_t ret) {
  push16(c, ret);
  fn(c);
  if (c->returned) return false;
  c->sp += 2;
  return true;
}

/* CALL to one of this file's routines that may abandon its caller too (by
 * popping the return address, or by a tail jump that returns past it).
 * Returns false if it did: the caller must then return at once.
 *
 * Where a routine pops its return address, the original leaves its frame
 * at that POP (check mode compares the state there), so the C version
 * pops and hands the rest to the original code with cpu_tail. */
static bool call_local(Cpu *c, void (*fn)(Cpu *), uint16_t ret) {
  uint16_t sp0 = c->sp;
  push16(c, ret);
  fn(c);
  if (c->returned || c->sp == sp0) return false;
  c->sp += 2;
  return true;
}

static void p_dead(Cpu *c);
static void p_9117(Cpu *c);
static void p_9145(Cpu *c);
static void p_92ed(Cpu *c);

/* ---------- look, put down, pick up ---------- */

/* $8C4B Action_Look: describe the location (or, inside something, what is
 * in it) and list what is there. */
static void p_look(Cpu *c) {
  if (trial(c, 0x8C4E)) return;
  c->ix = rd16(c, ACTOR_REC);
  c->a = ix_rd(c, 1);
  op_cp(c, 0xFF);
  if (c->zf) goto L8C95;
  set_hl(c, 0x0080);
  wr16(c, MSG_YOU_ARE_IN_ARG, get_hl(c));
  set_hl(c, MSG_YOU_ARE_IN);
  cpu_call_at(c, 0x8C62); /* CALL R_PRINT_MSG */
  push16(c, c->ix);
  c->a = ix_rd(c, 1);
  cpu_call_at(c, 0x8C6A); /* CALL R_OBJECT_REC */
  cpu_call_at(c, 0x8C6D); /* CALL R_A09D */
  c->e = ix_rd(c, 8);
  c->a = ix_rd(c, 9);
  c->a = op_and(c, c->a, 0x0F);
  c->d = c->a;
  cpu_call_at(c, 0x8C79); /* CALL R_PRINT_NAME */
  c->a = 0x2E;
  cpu_call_at(c, 0x8C7E); /* CALL R_PRINT_CHAR */
  cpu_call_at(c, 0x8C81); /* CALL R_NEWLINE */
  set_hl(c, MSG_YOU_SEE);
  cpu_call_at(c, 0x8C87); /* CALL R_PRINT_MSG */
  c->ix = pop16(c);
  c->a = ix_rd(c, 1);
  c->b = ix_rd(c, 0x10);
  tail(c, R_LIST);
  return;
L8C95:
  c->a = ix_rd(c, 0x10);
  tail(c, R_LIST_CONTENTS);
}

/* $8C9B: return (C) if OBJ1 is carried by the actor; otherwise say "You
 * are not carrying it" and abandon the caller. */
static void p_8c9b(Cpu *c) {
  cpu_call_at(c, 0x8C9B);
  if (c->cf) return;
  set_hl(c, pop16(c)); /* POP HL: abandon the caller */
  cpu_tail(c, 0x8CA0);  /* LD HL,MSG_NOT_CARRYING; JP R_PRINT_MSG */
}

/* $8CA6 Action_Putdown: put OBJ1 where the actor is (in what the actor is
 * in); things that vanish when put down ($07 bit 1) evaporate. */
static void p_putdown(Cpu *c) {
  if (!call_local(c, p_8c9b, 0x8CA9)) return;
  if (trial(c, 0x8CAC)) return;
  c->ix = rd16(c, OBJ1_REC);
  c->a = ix_rd(c, 1);
  op_cp(c, 0x12);
  if (!c->zf) goto L8CBB;
  c->ix = ROPE_REC;
L8CBB:
  push16(c, c->ix);
  c->ix = rd16(c, ACTOR_REC);
  c->a = ix_rd(c, 1);
  c->ix = pop16(c);
  ix_wr(c, 1, c->a);
  op_bit_at(c, 1, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  if (c->zf) return;
  ix_wr(c, 0x10, 0);
  set_hl(c, MSG_EVAPORATES);
  set_de(c, 8);
  c->ix = op_add16(c, c->ix, get_de(c));
  push16(c, c->ix); /* the name, for the message */
  cpu_call_at(c, 0x8CDC); /* CALL R_PRINT_MSG */
}

/* $8CF1: can the actor lift OBJ1 (its weight with what it carries, against
 * the actor's strength less what it already carries)? If not, say why and
 * abandon the caller; otherwise go on to $8D25. */
static void p_8d25(Cpu *c);
static void p_8cf1(Cpu *c) {
  c->ix = rd16(c, OBJ1_REC);
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x8CF8); /* CALL R_WEIGHT_OF */
  c->a = op_add(c, c->a, ix_rd(c, 3), 0);
  if (!c->cf) goto L8D02;
  c->a = 0xFF;
L8D02:
  c->b = c->a;
  c->iy = rd16(c, ACTOR_REC);
  c->a = iy_rd(c, 3);
  c->a = op_sub(c, c->a, c->b, 0);
  set_hl(c, MSG_TOO_HEAVY);
  if (c->cf) goto L8D20;
  push_af(c);
  c->a = c->mem[ACTOR];
  cpu_call_at(c, 0x8D14); /* CALL R_WEIGHT_OF */
  c->b = c->a;
  pop_af(c);
  c->a = op_sub(c, c->a, c->b, 0);
  if (!c->sf) {
    p_8d25(c);
    return;
  }
  set_hl(c, MSG_TOO_MUCH);
L8D20: /* EX (SP),HL; POP HL: drop the return address */
  wr16(c, c->sp, get_hl(c));
  c->sp += 2;
  cpu_tail(c, 0x8D22); /* JP R_PRINT_MSG */
}

/* $9246: Z flag from OBJ1's kind - 1 (NZ: not a single object). */
static void p_9246(Cpu *c) {
  c->ix = rd16(c, OBJ1_REC);
  c->a = ix_rd(c, 0);
  c->a = op_dec(c, c->a);
}

/* $8D25: OBJ1 must be a single object, and not one that vanishes ($07
 * bit 1); otherwise the action fails ($9F76) and the caller is abandoned. */
static void p_8d25(Cpu *c) {
  if (!call_near(c, p_9246, 0x8D28)) return;
  if (!c->zf) goto L8D2F;
  op_bit_at(c, 1, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  if (c->zf) return;
L8D2F:
  set_hl(c, pop16(c)); /* POP HL: abandon the caller */
  cpu_tail(c, 0x8D30); /* JP R_FAIL */
}

/* $8D33 Action_Pickup, and from $8D3C the object handler at $8CE0: take
 * OBJ1, unless it is too heavy, or the actor is in or on it. */
static void pickup(Cpu *c, bool check) {
  if (check) {
    cpu_call_at(c, 0x8D33);
    set_hl(c, MSG_ALREADY);
    if (c->cf) {
      cpu_tail(c, R_PRINT_MSG);
      return;
    }
  }
  /* $8D3C */
  if (!call_local(c, p_8cf1, 0x8D3F)) return;
  c->a = c->mem[ACTOR];
L8D42:
  cpu_call_at(c, 0x8D42); /* CALL R_OBJECT_REC */
  c->a = c->mem[OBJ1];
  op_cp(c, ix_rd(c, 1));
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  c->a = ix_rd(c, 1);
  op_cp(c, 0xFF);
  if (!c->zf) goto L8D42;
  c->ix = rd16(c, OBJ1_REC);
  if (trial(c, 0x8D5C)) return;
  c->a = ix_rd(c, 1);
  op_cp(c, 0x12);
  c->a = c->mem[ACTOR];
  if (c->zf) goto L8D6A;
  ix_wr(c, 1, c->a);
  return;
L8D6A:
  c->mem[ROPE_REC + 1] = c->a;
}
static void p_pickup(Cpu *c) { pickup(c, true); }

/* $8CE0 (object handler): take OBJ1 from OBJ2, if it is in it. */
static void p_8ce0(Cpu *c) {
  c->a = c->mem[OBJ1];
  set_hl(c, OBJ2);
  cpu_call_at(c, 0x8CE6); /* CALL R_INSIDE */
  set_hl(c, MSG_NOT_IN_IT);
  if (!c->zf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  pickup(c, false);
}

/* ---------- moving ---------- */

/* $8E85: can the mover go through the exit whose door is object A, into
 * DEST? A (and NZ) on return: 0 yes, 1 the door is closed, 2 the mover
 * does not fit the door, 3 the location is too full. Sets LOC_REC. */
static void p_8e85(Cpu *c) {
  c->a = op_and(c, c->a, c->a);
  if (c->zf) goto L8EA7;
  cpu_call_at(c, 0x8E88); /* CALL R_OBJECT_REC */
  c->a = op_and(c, ix_rd(c, 7), 0x28);
  if (c->zf) goto L8ECE;
  c->a = c->mem[ACTOR];
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) goto L8E9E;
  op_bit_at(c, 7, ix_rd(c, 4), (uint16_t)(c->ix + 4));
  if (!c->zf) goto L8ECE;
L8E9E:
  c->a = c->mem[MOVER_SIZE];
  c->b = ix_rd(c, 2);
  c->a = op_sub(c, c->a, c->b, 0);
  if (!c->cf) goto L8ECA;
L8EA7:
  c->a = c->mem[DEST];
  c->b = c->a;
  cpu_call_at(c, 0x8EAB); /* CALL R_LOCATION_REC */
  wr16(c, LOC_REC, c->ix);
  c->a = 0xFF;
  op_cp(c, ix_rd(c, 1));
  if (c->zf) goto L8EC4;
  c->a = c->b;
  cpu_call_at(c, 0x8EBA); /* CALL R_LOC_FULLNESS */
  c->c = c->a;
  c->a = c->mem[MOVER_SIZE];
  c->a = op_sub(c, c->a, c->c, 0);
  if (!c->cf) goto L8EC6;
L8EC4:
  c->a = op_xor(c, c->a, c->a);
  return;
L8EC6:
  c->a = 3;
  c->a = op_and(c, c->a, c->a);
  return;
L8ECA:
  c->a = 2;
  c->a = op_and(c, c->a, c->a);
  return;
L8ECE:
  c->a = 1;
  c->a = op_and(c, c->a, c->a);
}

/* $8D9D Action_Dir and its later entries: move the actor in direction
 * ACTION (a random one, in the dark). $8DAB: from the actor's position
 * (it climbs out of an open container first); $8E12: move the actor to
 * location B; $8E39: the player arrives at DEST (a one-off percentage
 * for some locations, then describe it). In the dark, a failed move may
 * hurt or kill the player. */
static void dir_body(Cpu *c, uint16_t entry) {
  switch (entry) {
  case 0x8DAB: goto L8DAB;
  case 0x8E12: goto L8E12;
  case 0x8E39: goto L8E39;
  default: break;
  }
  cpu_call_at(c, 0x8D9D);
  if (!c->cf) goto L8DAB;
  c->a = 9;
  cpu_call_at(c, 0x8DA4); /* CALL R_RANDOM_ABS */
  c->a = op_inc(c, c->a);
  c->mem[ACTION] = c->a;
L8DAB:
  c->iy = rd16(c, ACTOR_REC);
  c->a = iy_rd(c, 1);
  op_cp(c, 0xFF);
  if (c->zf) goto L8DC3;
  cpu_call_at(c, 0x8DB6); /* CALL R_OBJECT_REC */
  op_bit_at(c, 6, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  if (c->zf) goto L8DD9;
  iy_wr(c, 1, 0xFF);
L8DC3:
  c->a = c->mem[ACTOR];
  cpu_call_at(c, 0x8DC6); /* CALL R_SIZE_OF */
  c->a = op_add(c, c->a, iy_rd(c, 2), 0);
  c->mem[MOVER_SIZE] = c->a;
  c->a = c->mem[ACTION];
  cpu_call_at(c, 0x8DD2); /* CALL R_EXIT */
  op_cp(c, 0xFF);
  if (!c->zf) goto L8DFA;
L8DD9: /* no way: in the dark the player falls */
  cpu_call_at(c, 0x8DD9); /* CALL R_LIGHT */
  if (!c->cf) {
    tail(c, R_FAIL);
    return;
  }
  if (trial(c, 0x8DE2)) return;
  c->ix = PLAYER_REC;
  c->a = op_and(c, c->a, c->a);
  set_hl(c, MSG_HIT_HEAD);
  ix_wr(c, 5, op_rr(c, ix_rd(c, 5)));
  if (!c->zf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  set_hl(c, MSG_SMASH_SKULL);
  cpu_call_at(c, 0x8DF4); /* CALL R_PRINT_MSG */
  p_dead(c);
  return;
L8DFA:
  c->a = ix_rd(c, 2);
  c->a = op_and(c, c->a, c->a);
  if (c->zf) goto L8DD9;
  c->mem[DEST] = c->a;
  c->a = ix_rd(c, 1);
  if (!call_near(c, p_8e85, 0x8E09)) return;
  c->a = op_dec(c, c->a);
  if (c->zf) goto L8DD9;
  c->a = op_dec(c, c->a);
  if (c->zf) goto L8E6D;
  c->a = op_dec(c, c->a);
  if (c->zf) goto L8E72;
L8E12:
  if (trial(c, 0x8E15)) return;
  iy_wr(c, 0x10, c->b);
  c->a = c->mem[ACTOR];
  cpu_call_at(c, 0x8E1B); /* CALL R_MOVE_CONTENTS */
  c->a = c->mem[ACTOR];
  op_cp(c, 0);
  if (!c->zf) return;
  c->ix = LOC_EVENTS;
  c->a = c->mem[DEST];
  cpu_call_at(c, 0x8E2B); /* CALL R_FIND */
  if (c->zf) goto L8E39;
  c->l = ix_rd(c, 1);
  c->h = ix_rd(c, 2);
  cpu_call_at(c, 0x8E36); /* CALL R_TRIGGER */
L8E39:
  cpu_call_at(c, 0x8E39);
  if (c->cf) return;
  c->a = c->mem[ACTOR];
  c->a = op_and(c, c->a, c->a);
  c->a = c->mem[DEST];
  if (!c->zf) goto L8E6A;
  set_hl(c, rd16(c, LOC_REC));
  op_bit(c, 6, c->mem[get_hl(c)]);
  if (!c->zf) {
    tail(c, R_LOC_DESCRIBE);
    return;
  }
  c->mem[get_hl(c)] |= 0x40;
  push_af(c);
  c->ix = LOC_SCORES;
  cpu_call_at(c, 0x8E55); /* CALL R_FIND */
  if (c->zf) goto L8E69;
  push16(c, get_de(c));
  c->e = ix_rd(c, 1);
  c->d = ix_rd(c, 2);
  set_hl(c, rd16(c, SCORE));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  wr16(c, SCORE, get_hl(c));
  set_de(c, pop16(c));
L8E69:
  pop_af(c);
L8E6A:
  tail(c, R_LIST_CONTENTS);
  return;
L8E6D:
  set_hl(c, MSG_TOO_SMALL);
  goto L8E79;
L8E72:
  c->ix = rd16(c, LOC_REC);
  set_hl(c, MSG_TOO_FULL_ENTER);
L8E79:
  push16(c, get_hl(c));
  c->l = ix_rd(c, 2);
  c->h = ix_rd(c, 3);
  ex_sp_hl(c);
  cpu_call_at(c, 0x8E81); /* CALL R_PRINT_MSG */
}
static void p_dir(Cpu *c) { dir_body(c, 0x8D9D); }
static void p_8dab(Cpu *c) { dir_body(c, 0x8DAB); }
static void p_8e12(Cpu *c) { dir_body(c, 0x8E12); }
static void p_8e39(Cpu *c) { dir_body(c, 0x8E39); }

/* $8ED2: NZ if object A is inside something closed (each container up
 * from it must be open, $07 bit 5). */
static void p_8ed2(Cpu *c) {
  push16(c, c->ix);
  cpu_call_at(c, 0x8ED4); /* CALL R_OBJECT_REC */
L8ED7:
  c->a = ix_rd(c, 1);
  op_cp(c, 0xFF);
  if (c->zf) goto L8EE9;
  cpu_call_at(c, 0x8EDE); /* CALL R_OBJECT_REC */
  op_bit_at(c, 5, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  if (!c->zf) goto L8ED7;
  c->a = op_or(c, c->a, 1);
L8EE9:
  c->ix = pop16(c);
}

/* $8EEC (object handler, verb $18): look through OBJ1 (a window or door)
 * into the location beyond. */
static void p_8eec(Cpu *c) {
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x8EEF); /* CALL R_OBJECT_REC */
  cpu_call_at(c, 0x8EF2); /* CALL R_CLOSED */
  if (c->zf) {
    tail(c, R_OBJ_MSG);
    return;
  }
  c->a = c->mem[ACTOR];
  if (!call_near(c, p_8ed2, 0x8EFE)) return;
  if (!c->zf) return;
  cpu_call_at(c, 0x8EFF); /* CALL R_EXIT_OF */
  op_cp(c, 0xFF);
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  c->a = ix_rd(c, 2);
  op_cp(c, 0);
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  if (trial(c, 0x8F12)) return;
  push16(c, c->ix);
  cpu_call_at(c, 0x8F14); /* CALL R_LOCATION_REC */
  op_bit_at(c, 7, ix_rd(c, 0), (uint16_t)(c->ix + 0));
  c->ix = pop16(c);
  if (c->zf) goto L8F35;
  c->iy = rd16(c, ACTOR_REC);
  c->a = iy_rd(c, 0x10);
  push_af(c);
  c->a = ix_rd(c, 2);
  iy_wr(c, 0x10, c->a);
  cpu_call_at(c, 0x8F2D); /* CALL R_LIST_HERE */
  pop_af(c);
  iy_wr(c, 0x10, c->a);
  return;
L8F35:
  tail_msg(c, MSG_DARK);
}

/* $8F3E: go through the exit found (A=$FF none, IX the exit record:
 * +0 direction, +1 door, +2 location), as Action_Dir does. */
static void p_8f3e(Cpu *c) {
  op_cp(c, 0xFF);
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  c->a = ix_rd(c, 2);
  op_cp(c, 0);
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  c->a = ix_rd(c, 1);
  push16(c, c->ix);
  if (!call_near(c, p_8e85, 0x8F53)) return;
  c->ix = pop16(c);
  if (!c->zf) {
    tail(c, R_FAIL);
    return;
  }
  if (trial(c, 0x8F5B)) return;
  c->a = ix_rd(c, 0);
  c->mem[ACTION] = c->a;
  c->a = 0xFF;
  c->mem[OBJ1] = c->a;
  dir_body(c, 0x8DAB);
}

/* $8F3B (object handler, verb $1E): go through the exit by OBJ1. */
static void p_8f3b(Cpu *c) {
  cpu_call_at(c, 0x8F3B);
  p_8f3e(c);
}

/* $8F69 (object handler): put OBJ1 into OBJ2 (objects $17 and $18 stand
 * for $15 and $16), through $9F4A and $924F. */
static void p_8f69(Cpu *c) {
  c->ix = rd16(c, OBJ1_REC);
  c->a = c->mem[OBJ2];
  op_cp(c, 0x17);
  if (c->zf) goto L8F92;
  op_cp(c, 0x18);
  if (c->zf) goto L8F9A;
L8F78:
  c->iy = rd16(c, OBJ2_REC);
  op_bit_at(c, 1, iy_rd(c, 7), (uint16_t)(c->iy + 7));
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  op_bit_at(c, 2, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  c->a = 0x82;
  if (!c->zf) {
    tail(c, R_OBJ_MSG);
    return;
  }
  set_hl(c, 0x924F);
  tail(c, R_SWAP_TRIGGER);
  return;
L8F92:
  c->a = 0x15;
  c->iy = OBJ_15_REC;
  goto L8FA0;
L8F9A:
  c->a = 0x16;
  c->iy = OBJ_16_REC;
L8FA0:
  c->mem[OBJ2] = c->a;
  wr16(c, OBJ2_REC, c->iy);
  iy_wr(c, 1, 0xFF);
  goto L8F78;
}

/* $8FAD Action_Run: a random direction (1-9) that has an exit, then move. */
static void p_run(Cpu *c) {
L8FAD:
  c->a = 0x0A;
  cpu_call_at(c, 0x8FAF); /* CALL R_RANDOM_ABS */
  op_cp(c, 0);
  if (c->zf) goto L8FAD;
L8FB6:
  c->b = c->a;
  cpu_call_at(c, 0x8FB7); /* CALL R_EXIT */
  op_cp(c, c->b);
  if (c->zf) goto L8FC7;
  c->a = c->b;
  c->a = op_inc(c, c->a);
  op_cp(c, 0x0A);
  if (c->cf) goto L8FB6;
  c->a = 1;
  goto L8FB6;
L8FC7:
  c->mem[ACTION] = c->a;
  dir_body(c, 0x8D9D);
}

/* $8FCD Action_Enter: go through the exit by OBJ1. */
static void p_enter(Cpu *c) {
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x8FD0); /* CALL R_EXIT_VIA */
  p_8f3e(c);
}

/* $8FD6 Action_Follow: go where OBJ1 went, if it left from here. */
static void p_follow(Cpu *c) {
  c->ix = rd16(c, ACTOR_REC);
  c->b = ix_rd(c, 0x10);
  c->ix = rd16(c, OBJ1_REC);
  c->a = ix_rd(c, 0x10);
  op_cp(c, c->b);
  if (c->zf) goto L8FEF;
  cpu_call_at(c, 0x8FE7); /* CALL R_EXIT_VIA */
  op_cp(c, 0xFF);
  if (!c->zf) {
    p_8f3e(c);
    return;
  }
L8FEF:
  tail_msg(c, MSG_CANNOT_FOLLOW);
}

/* $8FF5 Action_ThrowAt: throw OBJ1 at OBJ2: an attack ($9171) on
 * something animate, otherwise a blow ($92ED), with the objects swapped;
 * then OBJ1 lands where it is. */
static void p_throw_at(Cpu *c) {
  if (!call_local(c, p_8cf1, 0x8FF8)) return;
  set_hl(c, 0x9171);
  c->a = 0x0F;
  c->ix = rd16(c, OBJ2_REC);
  op_bit_at(c, 6, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  if (!c->zf) goto L900C;
  set_hl(c, 0x92ED);
  c->a = 0x0B;
L900C:
  c->mem[ACTION] = c->a;
  cpu_call_at(c, 0x900F); /* CALL R_SWAP_TRIGGER */
  c->a = 0x2A;
  c->mem[ACTION] = c->a;
  c->a = c->mem[DO_IT];
  op_cp(c, 1);
  if (!c->zf) return;
  c->ix = rd16(c, OBJ1_REC);
  ix_wr(c, 1, 0xFF);
  c->a = 0x0F;
  c->mem[ACTION] = c->a;
  c->a = c->mem[OBJ1];
  tail(c, R_LIGHT_SOURCE);
}

/* $9034 Action_Talkto: say the quoted sentence to OBJ1, who may refuse. */
static void p_talkto(Cpu *c) {
  if (trial(c, 0x9037)) return;
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x903A); /* CALL R_TALK_TO */
  op_cp(c, 0xFF);
  c->a = 0;
  if (c->zf) goto L9058;
  c->a = c->mem[VAR_B6F9];
  op_cp(c, 0);
  if (!c->zf) goto L9058;
  c->a = iy_rd(c, 6);
  op_cp(c, 0);
  if (c->zf) goto L9058;
  cpu_call_at(c, 0x9051); /* CALL R_RANDOM_ABS */
  op_cp(c, 0);
  if (c->zf) goto L905C;
L9058:
  cpu_call_at(c, 0x9058); /* CALL R_TALK */
  return;
L905C:
  set_hl(c, MSG_SAYS_NO);
  cpu_call_at(c, 0x905F); /* CALL R_PRINT_MSG */
  c->a = op_sub(c, c->a, c->a, 0);
  goto L9058;
}

/* $9065 (object handler): open OBJ1 if it is closed, close it if open. */
static void p_9065(Cpu *c) {
  if (trial(c, 0x9068)) return;
  c->ix = rd16(c, OBJ1_REC);
  op_bit_at(c, 5, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  if (c->zf) {
    p_9117(c);
    return;
  }
  p_9145(c);
}

/* $914A: may the actor attack OBJ1 (attitude bits 4-6 of both)? If not,
 * abandon the caller with RESULT 0. The player attacking clears OBJ1's
 * bit 4 first. */
static void p_914a(Cpu *c) {
  c->ix = rd16(c, OBJ1_REC);
  c->a = c->mem[ACTOR];
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) goto L915E;
  op_bit_at(c, 4, ix_rd(c, 4), (uint16_t)(c->ix + 4));
  if (c->zf) goto L915E;
  ix_wr(c, 4, ix_rd(c, 4) & ~0x10);
L915E:
  c->a = op_and(c, ix_rd(c, 4), 0x70);
  c->ix = rd16(c, ACTOR_REC);
  c->a = op_and(c, c->a, ix_rd(c, 4));
  if (c->zf) return;
  set_hl(c, pop16(c)); /* POP HL: abandon the caller */
  cpu_tail(c, 0x916C); /* XOR A; LD (RESULT),A; RET */
}

/* $9076 Action_Shoot: shoot OBJ1 with the bow (object $19). */
static void p_shoot(Cpu *c) {
  c->a = 0x19;
  cpu_call_at(c, 0x9078); /* CALL R_CARRIED_A */
  set_hl(c, MSG_NO_BOW);
  if (!c->cf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  if (!call_local(c, p_914a, 0x9084)) return;
  if (trial(c, 0x9087)) return;
  c->a = 0x0F;
  c->mem[ACTION] = c->a;
  c->a = c->mem[ACTOR];
  op_cp(c, 0x46);
  if (c->zf) goto L90A8;
  c->a = c->mem[OBJ1];
  op_cp(c, 0x3C);
  set_hl(c, MSG_ARROW_MISSES);
  if (c->zf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  c->a = 8;
  cpu_call_at(c, 0x90A0); /* CALL R_RANDOM_ABS */
  op_cp(c, 3);
  if (c->cf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
L90A8:
  c->ix = ARROW_REC;
  c->a = c->mem[OBJ1];
  op_cp(c, 0x1A);
  if (c->zf) goto L90B7;
  ix_wr(c, 1, 0xFF);
L90B7:
  set_hl(c, MSG_ARROW_HITS);
  cpu_call_at(c, 0x90BA); /* CALL R_PRINT_MSG */
  c->ix = rd16(c, OBJ1_REC);
  cpu_call_at(c, 0x90C1); /* CALL R_IS_DEAD */
  if (!c->zf) {
    p_92ed(c);
    return;
  }
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x90CA); /* CALL R_KILL */
  c->a = 6;
  tail(c, R_OBJ_MSG);
}

/* $90D2 YouAreDead: "You are dead", the score, then wait for a key and
 * restart ($90DF, left as original: it reads the keyboard itself). */
static void p_dead(Cpu *c) {
  c->a = op_sub(c, c->a, c->a, 0);
  c->mem[ACTOR] = c->a;
  set_hl(c, MSG_DEAD);
  cpu_call_at(c, 0x90D9); /* CALL R_PRINT_MSG */
  cpu_call_at(c, 0x90DC); /* CALL R_SCORE */
  tail(c, R_DEAD_KEY);
}

/* $90EB Action_Inventory: list what the actor carries. */
static void p_inventory(Cpu *c) {
  if (trial(c, 0x90EE)) return;
  set_hl(c, MSG_CARRYING);
  cpu_call_at(c, 0x90F1); /* CALL R_PRINT_MSG */
  c->a = c->mem[ACTOR];
  cpu_call_at(c, 0x90F7); /* CALL R_COUNT_IN */
  c->a = op_and(c, c->a, c->a);
  set_hl(c, MSG_NOTHING);
  if (c->zf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  c->a = c->mem[ACTOR];
  c->ix = rd16(c, ACTOR_REC);
  c->b = ix_rd(c, 0x10);
  tail(c, R_LIST);
}

/* $9117: open object IX ($07 bit 5); for a container, list what is in
 * it when it can be seen. */
static void p_9117(Cpu *c) {
  ix_wr(c, 7, ix_rd(c, 7) | 0x20);
  c->a = ix_rd(c, 0);
  c->a = op_dec(c, c->a);
  if (!c->zf) return;
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x9123); /* CALL R_COUNT_IN */
  c->a = op_and(c, c->a, c->a);
  if (c->zf) return;
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x912B); /* CALL R_SEE_INTO */
  if (c->cf) return;
  c->b = ix_rd(c, 0x10);
  c->a = c->mem[OBJ1];
  tail(c, R_LIST);
}

/* $910E (object handler): open OBJ1 (record in IX), if it can be. */
static void p_910e(Cpu *c) {
  cpu_call_at(c, 0x910E);
  if (!c->zf) {
    tail(c, R_OBJ_MSG);
    return;
  }
  if (trial(c, 0x9117)) return;
  p_9117(c);
}

/* $9145: close object IX. */
static void p_9145(Cpu *c) { ix_wr(c, 7, ix_rd(c, 7) & ~0x20); }

/* $9138 (object handler): close OBJ1, if it is open. */
static void p_9138(Cpu *c) {
  c->ix = rd16(c, OBJ1_REC);
  cpu_call_at(c, 0x913C); /* CALL R_CLOSED */
  if (c->zf) {
    tail(c, R_OBJ_MSG);
    return;
  }
  if (trial(c, 0x9145)) return;
  p_9145(c);
}

/* $9213: A plus a random number from -$0A to $0A-ish (RNG $9CA8 with
 * A=$0A), clamped to $00-$FF by the sign of the random part. */
static void p_9213(Cpu *c) {
  push16(c, get_bc(c));
  c->b = c->a;
  c->a = 0x0A;
  cpu_call_at(c, 0x9217); /* CALL R_RANDOM */
  c->c = c->a;
  c->a = op_add(c, c->a, c->b, 0);
  if (!c->cf) goto L9224;
  c->a = op_xor(c, c->a, c->a);
  op_bit(c, 7, c->c);
  if (!c->zf) goto L9224;
  c->a = op_dec(c, c->a);
L9224:
  set_bc(c, pop16(c));
}

/* $9171 Action_Attack: the actor attacks OBJ1 (with weapon OBJ2): compare
 * randomised strength (with the weapon's) against OBJ1's defence; a
 * clear win kills, a narrow one wounds (strength and defence of OBJ1
 * reduced, with a message by how narrow). */
static void p_attack(Cpu *c) {
  if (!call_local(c, p_914a, 0x9174)) return;
  c->a = c->mem[OBJ2];
  set_hl(c, 0x026B);
  op_cp(c, 0xFF);
  if (c->zf) goto L9188;
  c->ix = rd16(c, OBJ2_REC);
  c->l = ix_rd(c, 8);
  c->h = ix_rd(c, 9);
L9188:
  wr16(c, WEAPON_NAME, get_hl(c));
  c->ix = rd16(c, ACTOR_REC);
  c->b = ix_rd(c, 5);
  c->a = c->mem[OBJ2];
  c->a = op_inc(c, c->a);
  if (c->zf) goto L91AF;
  c->iy = rd16(c, OBJ2_REC);
  c->a = iy_rd(c, 0);
  c->a = op_dec(c, c->a);
  set_hl(c, MSG_CANNOT_KILL);
  if (!c->zf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  c->a = iy_rd(c, 5);
  c->a = op_add(c, c->a, c->b, 0);
  if (!c->cf) goto L91AE;
  c->a = 0xFF;
L91AE:
  c->b = c->a;
L91AF:
  c->a = c->b;
  if (!call_near(c, p_9213, 0x91B3)) return;
  c->b = c->a;
  if (trial(c, 0x91B7)) return;
  c->ix = rd16(c, OBJ1_REC);
  c->a = ix_rd(c, 6);
  if (!call_near(c, p_9213, 0x91C1)) return;
  op_cp(c, c->b);
  set_hl(c, MSG_WASTED);
  if (!c->cf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  c->c = c->a;
  c->a = op_add(c, c->a, 0x10, 0);
  if (!c->cf) goto L91CF;
  c->a = 0xFF;
L91CF:
  op_cp(c, c->b);
  if (c->cf) goto L91FE;
  c->a = c->b;
  c->a = op_sub(c, c->a, c->c, 0);
  op_rlca(c);
  c->e = c->a;
  c->d = 0;
  c->iy = DAMAGE_MSGS;
  c->iy = op_add16(c, c->iy, get_de(c));
  c->l = iy_rd(c, 0);
  c->h = iy_rd(c, 1);
  op_rrca(c);
  op_rrca(c);
  c->b = c->a;
  op_cpl(c);
  c->a = op_add(c, c->a, ix_rd(c, 5), 0);
  if (!c->cf) goto L91F0;
  ix_wr(c, 5, c->a);
L91F0:
  c->a = c->b;
  op_rrca(c);
  op_cpl(c);
  c->a = op_add(c, c->a, ix_rd(c, 6), 0);
  if (!c->cf) goto L91FB;
  ix_wr(c, 6, c->a);
L91FB:
  cpu_tail(c, R_PRINT_MSG);
  return;
L91FE:
  set_hl(c, MSG_CLEAVE);
  cpu_call_at(c, 0x9201); /* CALL R_PRINT_MSG */
  ix_wr(c, 7, ix_rd(c, 7) | 0x08);
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x920B); /* CALL R_KILL */
  c->a = 6;
  tail(c, R_OBJ_MSG);
}

/* $924F (object handler, reached through $9F4A with the objects swapped):
 * put OBJ1 into OBJ2, if it is open (or the verb is $12) and has room. */
static void p_924f(Cpu *c) {
  if (!call_local(c, p_8d25, 0x9252)) return;
  c->a = c->mem[OBJ2];
  op_cp(c, ix_rd(c, 1));
  if (c->zf) {
    tail(c, R_CANT);
    return;
  }
  c->iy = rd16(c, OBJ2_REC);
  c->a = c->mem[ACTION];
  op_cp(c, 0x12);
  if (c->zf) goto L926C;
  op_bit_at(c, 5, iy_rd(c, 7), (uint16_t)(c->iy + 7));
  if (c->zf) goto L9297;
L926C:
  c->a = iy_rd(c, 2);
  c->a = op_sub(c, c->a, ix_rd(c, 2), 0);
  if (c->cf) goto L927E;
  push_af(c);
  c->a = c->mem[OBJ2];
  cpu_call_at(c, 0x9278); /* CALL R_SIZE_OF */
  c->b = c->a;
  pop_af(c);
  c->a = op_sub(c, c->a, c->b, 0);
L927E:
  set_hl(c, MSG_TOO_FULL);
  if (c->cf || c->zf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  if (trial(c, 0x928A)) return;
  c->a = iy_rd(c, 0x10);
  ix_wr(c, 0x10, c->a);
  c->a = c->mem[OBJ2];
  ix_wr(c, 1, c->a);
  return;
L9297:
  c->a = 5;
  tail(c, R_STATE_MSG);
}

/* $929C (object handler) and $92B5: eat OBJ1 (from a container: +1
 * strength; from the floor: +$0A). Too much ($80 or more) kills; otherwise
 * OBJ1 is gone (location and the bytes after it cleared). */
static void eat(Cpu *c, bool from_929c) {
  if (!from_929c) goto L92B5;
  if (trial(c, 0x929F)) return;
  c->ix = rd16(c, OBJ1_REC);
  c->a = ix_rd(c, 1);
  op_cp(c, 0xFF);
  if (c->zf) goto L92B5;
  cpu_call_at(c, 0x92AA); /* CALL R_OBJECT_REC */
  ix_wr(c, 7, ix_rd(c, 7) & ~0x04);
  c->a = 1;
  goto L92BA;
L92B5:
  if (trial(c, 0x92B8)) return;
  c->a = 0x0A;
L92BA:
  c->ix = rd16(c, ACTOR_REC);
  c->a = op_add(c, c->a, ix_rd(c, 5), 0);
  op_cp(c, 0x80);
  if (!c->cf) goto L92DC;
  ix_wr(c, 5, c->a);
  c->ix = rd16(c, OBJ1_REC);
  ix_wr(c, 1, 0xFF);
  c->b = ix_rd(c, 0);
  do {
    ix_wr(c, 0x10, 0);
    c->ix++;
  } while (--c->b);
  return;
L92DC:
  set_hl(c, MSG_GLUTTONY);
  cpu_call_at(c, 0x92DF); /* CALL R_PRINT_MSG */
  c->a = c->mem[ACTOR];
  tail(c, R_KILL);
}
static void p_929c(Cpu *c) { eat(c, true); }
static void p_92b5(Cpu *c) { eat(c, false); }

/* $92ED (object handler, verb $0B; also a blow from a thrown object):
 * break or wound OBJ1 (with OBJ2), and OBJ2 may break on it. */
static void p_92ed(Cpu *c) {
  c->ix = rd16(c, OBJ1_REC);
  op_bit_at(c, 1, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  if (!c->zf) {
    tail(c, R_FAIL);
    return;
  }
  op_bit_at(c, 3, ix_rd(c, 7), (uint16_t)(c->ix + 7));
  if (!c->zf) goto L9399;
  c->a = op_sub(c, c->a, c->a, 0);
  op_cp(c, ix_rd(c, 6));
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  c->b = c->a;
  c->a = c->mem[OBJ2];
  c->a = op_inc(c, c->a);
  if (c->zf) goto L932C;
  c->iy = rd16(c, OBJ2_REC);
  c->a = iy_rd(c, 5);
  c->a = op_and(c, c->a, c->a);
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  push16(c, c->ix);
  c->ix = rd16(c, OBJ2_REC);
  c->a = 0x0B;
  cpu_call_at(c, 0x9320); /* CALL R_SPEAK_CHECK */
  c->ix = pop16(c);
  c->a = op_inc(c, c->a);
  if (c->zf) {
    tail(c, R_FAIL);
    return;
  }
  c->b = iy_rd(c, 5);
L932C:
  if (trial(c, 0x932F)) return;
  c->a = 0x15;
  cpu_call_at(c, 0x9331); /* CALL R_RANDOM */
  c->a = op_add(c, c->a, c->b, 0);
  c->iy = rd16(c, ACTOR_REC);
  c->a = op_add(c, c->a, iy_rd(c, 5), 0);
  if (!c->cf) goto L9340;
  c->a = 0xFF;
L9340:
  c->a = op_sub(c, c->a, ix_rd(c, 6), 0);
  if (c->cf) goto L9360;
  ix_wr(c, 7, ix_rd(c, 7) | 0x08);
  c->a = c->mem[OBJ1];
  cpu_call_at(c, 0x934C); /* CALL R_REVIVE */
  ix_wr(c, 5, op_sra(c, ix_rd(c, 5)));
  c->a = ix_rd(c, 4);
  op_cp(c, 2);
  cpu_call_at(c, 0x9358); /* CALL C,$9D50 */
  c->a = 0x83;
  cpu_call_at(c, 0x935D); /* CALL R_OBJ_MSG */
L9360:
  c->a = c->mem[OBJ2];
  op_cp(c, 0xFF);
  if (c->zf) return;
  c->iy = rd16(c, OBJ2_REC);
  op_bit_at(c, 3, iy_rd(c, 7), (uint16_t)(c->iy + 7));
  if (!c->zf) return;
  c->b = iy_rd(c, 6);
  c->a = 0x15;
  cpu_call_at(c, 0x9374); /* CALL R_RANDOM */
  c->a = op_add(c, c->a, c->b, 0);
  if (!c->cf) goto L937C;
  c->a = 0xFF;
L937C:
  c->a = op_sub(c, c->a, ix_rd(c, 6), 0);
  if (c->cf) return;
  iy_wr(c, 7, iy_rd(c, 7) | 0x08);
  c->a = c->mem[OBJ2];
  cpu_call_at(c, 0x9387); /* CALL R_REVIVE */
  c->a = iy_rd(c, 5);
  c->a = op_sra(c, c->a);
  iy_wr(c, 5, c->a);
  cpu_call_at(c, 0x9392); /* CALL R_DROP_ALL */
  push16(c, c->iy);
  c->ix = pop16(c);
L9399:
  c->a = 0x83;
  tail(c, R_OBJ_MSG);
}

/* $939E Action_Give: give OBJ1 to OBJ2, if OBJ2 can carry it. */
static void p_give(Cpu *c) {
  c->iy = rd16(c, OBJ2_REC);
  cpu_call_at(c, 0x93A2); /* CALL R_CARRIED */
  set_hl(c, MSG_NOT_CARRYING);
  if (!c->cf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  c->a = c->mem[OBJ2];
  cpu_call_at(c, 0x93AE); /* CALL R_WEIGHT_OF */
  c->ix = rd16(c, OBJ1_REC);
  c->a = op_add(c, c->a, ix_rd(c, 3), 0);
  push16(c, (uint16_t)(c->a << 8 | cpu_f(c))); /* PUSH AF; POP BC */
  set_bc(c, pop16(c));
  c->a = iy_rd(c, 3);
  c->a = op_sub(c, c->a, c->b, 0);
  set_hl(c, MSG_TOO_MUCH);
  if (c->cf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  if (trial(c, 0x93C7)) return;
  c->a = c->mem[OBJ2];
  ix_wr(c, 1, c->a);
  c->a = iy_rd(c, 0x10);
  ix_wr(c, 0x10, c->a);
  c->b = c->a;
  c->a = c->mem[OBJ1];
  tail(c, R_MOVE_CONTENTS);
}

/* Continuations: where $8C9B, $8CF1, $8D25 and $914A have popped their
 * return address (abandoning their caller), the rest runs as a routine of
 * its own, returning to the caller's caller. */

/* $8CA0: "You are not carrying it". */
static void p_8ca0(Cpu *c) { tail_msg(c, MSG_NOT_CARRYING); }
/* $8D22: print the message at HL. */
static void p_8d22(Cpu *c) { tail(c, R_PRINT_MSG); }
/* $8D30: the action fails. */
static void p_8d30(Cpu *c) { tail(c, R_FAIL); }
/* $916C: RESULT 0. */
static void p_916c(Cpu *c) {
  c->a = op_xor(c, c->a, c->a);
  c->mem[RESULT] = c->a;
}

#define STD (OUT_REGS | OUT_ZF | OUT_CF)

const PortRoutine actions1_routines[] = {
    {0x8C4B, "Action_Look", p_look, STD},
    {0x8C9B, "CheckCarrying", p_8c9b, STD},
    {0x8CA6, "Action_Putdown", p_putdown, STD},
    {0x8CE0, "TakeFrom", p_8ce0, STD},
    {0x8CF1, "CheckLift", p_8cf1, STD},
    {0x8D25, "CheckSingle", p_8d25, STD},
    {0x8D33, "Action_Pickup", p_pickup, STD},
    {0x8D9D, "Action_Dir", p_dir, STD},
    {0x8DAB, "MoveActor", p_8dab, STD},
    {0x8E12, "MoveTo", p_8e12, STD},
    {0x8E39, "Arrive", p_8e39, STD},
    {0x8E85, "CanEnter", p_8e85, STD},
    {0x8ED2, "InsideClosed", p_8ed2, STD},
    {0x8EEC, "LookThrough", p_8eec, STD},
    {0x8F3B, "GoThrough", p_8f3b, STD},
    {0x8F3E, "GoExit", p_8f3e, STD},
    {0x8F69, "PutIn", p_8f69, STD},
    {0x8FAD, "Action_Run", p_run, STD},
    {0x8FCD, "Action_Enter", p_enter, STD},
    {0x8FD6, "Action_Follow", p_follow, STD},
    {0x8FF5, "Action_ThrowAt", p_throw_at, STD},
    {0x9034, "Action_Talkto", p_talkto, STD},
    {0x9065, "OpenOrClose", p_9065, STD},
    {0x9076, "Action_Shoot", p_shoot, STD},
    {0x90D2, "YouAreDead", p_dead, STD},
    {0x90EB, "Action_Inventory", p_inventory, STD},
    {0x910E, "Open", p_910e, STD},
    {0x9117, "OpenIt", p_9117, STD},
    {0x9138, "Close", p_9138, STD},
    {0x9145, "CloseIt", p_9145, STD},
    {0x914A, "CheckHostile", p_914a, STD},
    {0x9171, "Action_Attack", p_attack, STD},
    {0x9213, "Randomise", p_9213, STD},
    {0x9246, "IsSingle", p_9246, STD},
    {0x924F, "PutInto", p_924f, STD},
    {0x929C, "Eat", p_929c, STD},
    {0x92B5, "EatFood", p_92b5, STD},
    {0x92ED, "Break", p_92ed, STD},
    {0x939E, "Action_Give", p_give, STD},
    {0x8CA0, "NotCarrying", p_8ca0, STD},
    {0x8D22, "LiftFailMsg", p_8d22, STD},
    {0x8D30, "SingleFail", p_8d30, STD},
    {0x916C, "NoFight", p_916c, STD},
    {0, NULL, NULL, 0},
};
