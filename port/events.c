/* Events and the rest of the actions ($A541-$AB52).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * The disassembly shows one block from ActionClimbOut, but it is a run of
 * small routines, each reached on its own:
 *
 *  - from the default action table at $C730 (3-byte records: action,
 *    address), and from the action lists inside object records (the
 *    3-byte records at IX+(IX+0)+$10 that $9B81 searches): the routine
 *    for an action done to that object. Called through TriggerAction
 *    ($9B6C, which saves IX IY DE BC HL around a JP (HL));
 *  - from the characters' scripts at $C8xx-$CA83, interpreted around
 *    $9928: ops with bit 0 set call the address that follows, through
 *    $9B6C. Ops with bit 6 set as well ($43) call it twice, first with
 *    $B6FA=0 (asking: may this happen?) and, if the routine set $B6FB,
 *    again with $B6FA=1 (do it);
 *  - from the timed events table at $CA84 (7-byte records: count, timer,
 *    address, second timer, second address), run by $96B3 each turn;
 *  - by CALL: $A5CA, $A86E, $A9D6, $AAC7.
 *
 * Most action routines start with CALL $9D44 (CheckPhase): when $B6FA is
 * not 1 it sets $B6FB and returns from the routine that called it, so
 * that routine only tests whether it applies and stops there. $97FF does
 * the same when $B6FA and $B6FB are not both set. Those two are done in C
 * (check_phase, check_phase2). Every other CALL runs the original CALL instruction (cpu_call_at), so
 * the stack holds the same return addresses as in the original.
 *
 * $A70A-$A71D is not reached from anywhere (no table entry or jump), and
 * is not registered. */
#include <stddef.h>
#include <stdlib.h>

#include "cpu.h"
#include "routines.h"

/* Variables. */
#define CUR_ACTION 0xB6E7    /* the action being done */
#define CUR_OBJECT 0xB6E8    /* its object (first noun) */
#define CUR_OBJECT2 0xB6E9   /* its second object */
#define CUR_CHAR 0xB6EA      /* who does it: 0 is you */
#define VAR_B6E6 0xB6E6      /* unknown: copied into a character's script */
#define CUR_SCRIPT 0xB6EE    /* pointer into a 4-byte record at $C7FC.. (see $97F0) */
#define TIMED_FIRED 0xB6F0   /* a timed event has fired this turn ($96B3) */
#define MAP_DRAWN 0xB6F1     /* unknown: nonzero once the map's clue is set */
#define PREV_LOCATION 0xB6F3 /* unknown: a location, compared with yours at $AB1F */
#define VAR_B6F4 0xB6F4      /* unknown: set to 1 for the event at $A8AB */
#define CHAR_LOCATION 0xB6F5 /* location of the character whose script runs */
#define CHAR_LOCATION2 0xB6F6 /* unknown: a location compared with $B6F5 */
#define VAR_B6F9 0xB6F9      /* unknown: set by $A8D2, cleared by $A8F6 */
#define PHASE 0xB6FA         /* 1: do it; 0: only check (see CheckPhase) */
#define PHASE_OK 0xB6FB      /* set by a check that applies */
#define VAR_B700 0xB700      /* unknown: set by $AAF9, cleared by $AB0B */
#define NIGHT_DAY 0xB702     /* 0 night, 1 day; also output enable */
#define OBJ1_PTR 0xB708      /* record of CUR_OBJECT */
#define OBJ2_PTR 0xB70A      /* record of CUR_OBJECT2 */
#define CHAR_PTR 0xB70C      /* record of CUR_CHAR */
#define MAP_RECORD 0xA7D1    /* operand of LD IY,nn at $A7CF: the map's clue, set by $97CF */

/* Objects: records of $10 bytes and more. +1 is the object it is in
 * ($FF none), +5 strength(?), +7 attributes (bit 7 visible, 6 animal,
 * 5 open, 3 broken, 2 full, 0 locked), +$10 location. */
#define YOU 0xC11B
#define YOUR_MOTHER 0xC11C   /* the object you are in */
#define YOUR_ATTRS 0xC122
#define YOUR_LOCATION 0xC12B
#define OBJ_C13B 0xC13B      /* object $3C's name: the "[0x00] enters" argument */
#define OBJ_C143 0xC143      /* object $3C's location */
#define OBJ_C1AD 0xC1AD
#define OBJ_C205 0xC205
#define OBJ_C20F 0xC20F
#define OBJ_C2BC 0xC2BC      /* object $0B's attributes (the hole that vanishes) */
#define OBJ_C31A 0xC31A
#define OBJ_C388 0xC388      /* attributes of the magic door */
#define OBJ_C3B9 0xC3B9
#define BARREL 0xC3EE        /* object $13, the barrel */
#define BARREL_LOCATION 0xC3FE /* +$10 of the barrel */
#define OBJ_C418 0xC418      /* object $14 */
#define OBJ_C452 0xC452
#define OBJ_C4C1 0xC4C1
#define OBJ_C5CE 0xC5CE
#define OBJ_C5DD 0xC5DD
#define OBJ_C61C 0xC61C
#define OBJ_C62B 0xC62B
#define TROLL1_ATTRS 0xC646  /* object $47 */
#define TROLL2_ATTRS 0xC658  /* object $48 */
#define OBJ_C663 0xC663
/* Timed event records and character scripts. */
#define TIMER_CA85 0xCA85
#define VAR_CAA1 0xCAA1
#define VAR_CAA7 0xCAA7
#define VAR_CAA8 0xCAA8
#define VAR_CAB5 0xCAB5
#define VAR_CAB6 0xCAB6
#define VAR_CAC3 0xCAC3
#define VAR_CAC4 0xCAC4
#define SCRIPT_C9E2 0xC9E2   /* an op of a character's script, rewritten by $A8AB */
#define TROLLS_LOCATION 0xBABC /* location $05: attributes; +8 its description */
#define TROLLS_DESC 0xBAC4
#define PICTURES 0xCC00      /* 3-byte table: location, picture data */

/* Messages. */
#define MSG_AF66 0xAF66
#define MSG_AF6D 0xAF6D
#define MSG_AF76 0xAF76
#define MSG_AF82 0xAF82
#define MSG_AF8B 0xAF8B
#define MSG_AF92 0xAF92
#define MSG_AFA4 0xAFA4
#define MSG_AFF4 0xAFF4
#define MSG_B017 0xB017 /* "[0x00] enter(s)." */
#define MSG_B037 0xB037 /* "the magic door opens." */
#define MSG_B03F 0xB03F /* "the magic door closes." */
#define MSG_B05B 0xB05B /* "hurry up." */
#define MSG_B085 0xB085 /* "the vicious warg run around you and howls." */
#define MSG_B0CB 0xB0CB /* "where's the thief ?" */
#define MSG_B0D5 0xB0D5 /* "get us out of this one, thief !" */
#define MSG_B0E5 0xB0E5 /* "thorin sits down and starts singing about gold." */
#define MSG_B0F4 0xB0F4 /* "thorin wait." */
#define MSG_B0FD 0xB0FD /* "the spider web is slowly smothering you." */
#define MSG_B109 0xB109
#define MSG_B113 0xB113
#define MSG_B11B 0xB11B /* "you cannot reach" */
#define MSG_B13E 0xB13E
#define MSG_B153 0xB153
#define MSG_B15D 0xB15D /* the dragon: "...your cunning has failed you..." */
#define MSG_B17E 0xB17E /* the dragon: "I may not be able to see you..." */
#define MSG_B1A6 0xB1A6
#define MSG_B1BA 0xB1BA
#define MSG_B1D0 0xB1D0 /* "[0x01] go from [0x00] to get to [0x00]" (3 arguments) */
#define MSG_B1DB 0xB1DB /* "someone strangles you from behind" */
#define MSG_B238 0xB238 /* "blimey, look at this!! Can yer cook'em?" */
#define MSG_B24C 0xB24C /* "yer can try, but he wouldn't make above a mouthfull." */
#define MSG_B262 0xB262 /* "in a clearing with two stone trolls." */
#define MSG_B277 0xB277
#define MSG_B2A4 0xB2A4 /* "the hole vanishes" */
#define MSG_B2AA 0xB2AA
#define MSG_B2D9 0xB2D9
#define MSG_B2E7 0xB2E7
#define MSG_B301 0xB301
#define MSG_B30B 0xB30B /* "day dawns" */
#define MSG_B311 0xB311 /* "You see some pale bulbous eyes staring at You" */
#define MSG_B31F 0xB31F /* "some thing drops from above and stings" */
#define MSG_B32C 0xB32C /* "you are thrown onto the bank of the long lake" */
#define MSG_B3D9 0xB3D9

/* Routines. */
#define R_PRINT_MSG 0x72DD   /* print the message at HL */
#define R_SAY 0xA1E3         /* "You say " message at HL, in quotes */
#define R_SAY_DOT 0x97F4     /* message at HL, then ". " and a new line */
#define R_CHECK_PHASE 0x9D44 /* CheckPhase: return from the caller unless $B6FA=1 */
#define R_CHECK_PHASE2 0x97FF /* the same, unless $B6FA and $B6FB are both set */
#define R_YOU_ARE_DEAD 0x90D2
#define R_WAIT_RESTART 0x90DF /* wait for a key and restart */
#define R_RANDOM 0x9C9F      /* random number below A */
#define R_FIND_OBJECT 0x9BCA /* IX = the record of object A */
#define R_FIND_LOCATION 0x9BB1 /* IX = the record of location A */
#define R_FIND_3BYTE 0x9DBD  /* find A in the 3-byte table at IX */
#define R_MOVE_OBJECT 0x9BDD /* object A to location B (with its events) */
#define R_DESCRIBE 0x9D53    /* unknown: update object A's visibility */
#define R_KILL 0x977F        /* object A dies (A=0: you are dead) */
#define R_VISIBLE_TO 0x9E34  /* NZ if IY is visible to CHAR_PTR */
#define R_VISIBLE_SWAP 0x9E25
#define R_CARRIED 0x9CE8     /* total carried by character A(?) */
#define R_ACTION_FAILS 0x9F76 /* the action is not possible here */
#define R_HELP 0x93DA        /* examine: the object's help text */
#define R_TALK 0x7F1A        /* unknown: A=1 speech record */
#define R_TOKEN_NAME 0xA0BA  /* DE = name of direction A */
#define R_A18C 0xA18C
#define R_9F28 0x9F28
#define R_9F2D 0x9F2D
#define R_92B5 0x92B5        /* eat: "foul gluttony" */
#define R_A16C 0xA16C        /* "it is closed"(?) */
#define R_9138 0x9138
#define R_910E 0x910E
#define R_8F3B 0x8F3B
#define R_92ED 0x92ED
#define R_8EEC 0x8EEC
#define R_939E 0x939E
#define R_9630 0x9630
#define R_A3BC 0xA3BC

/* CALL target, the instruction at site in the original (target is given
 * to name the callee, and checked). If the callee leaves this routine
 * instead (a death restarts the game), the routine is abandoned. */
static void call(Cpu *c, uint16_t site, uint16_t target) {
  uint8_t op = c->mem[site]; /* CALL nn, or CALL cc,nn (tested by the caller too) */
  if ((op != 0xCD && (op & 0xC7) != 0xC4) || rd16(c, (uint16_t)(site + 1)) != target) abort();
  cpu_call_at(c, site);
}

/* CALL $9D44 (CheckPhase), returning to ret: true if $B6FA is 1 (go on);
 * otherwise set $B6FB, and the original pops ret into BC and returns from
 * the routine that called it: return false, and return at once. Done in
 * C, not with cpu_call_at, because several routines start with this
 * call, and running it at their own (registered) address would run them
 * again. */
static bool check_phase(Cpu *c, uint16_t ret) {
  wr16(c, (uint16_t)(c->sp - 2), ret); /* the CALL's push */
  c->a = c->mem[PHASE];
  op_cp(c, 0x01);
  if (c->zf) return true;
  c->a = op_inc(c, c->a);
  c->mem[PHASE_OK] = c->a;
  set_bc(c, ret);
  return false;
}

/* CALL $97FF, returning to ret: true if $B6FA and $B6FB are both set;
 * otherwise return from the caller as check_phase does. */
static bool check_phase2(Cpu *c, uint16_t ret) {
  wr16(c, (uint16_t)(c->sp - 2), ret);
  wr16(c, (uint16_t)(c->sp - 4), get_bc(c)); /* PUSH BC */
  uint16_t bc = get_bc(c);
  c->c = c->mem[PHASE];
  c->b = c->mem[PHASE_OK];
  c->a = op_and(c, c->c, c->b);
  if (!c->zf) {
    set_bc(c, bc);
    return true;
  }
  set_bc(c, ret);
  return false;
}

/* LD HL,hl; CALL $72DD (at site). */
static void msg(Cpu *c, uint16_t site, uint16_t hl) {
  set_hl(c, hl);
  call(c, site, R_PRINT_MSG);
}

/* $A1E3 Say: 'You say "', the message at HL, '".' (in actions3.c's range,
 * registered here as the continuation of the JP $A1E3 at $A811, which
 * reaches it with three message arguments pushed and no return address of
 * its own: the message at HL takes them off the stack, and the final
 * message returns to whatever is under them). */
static void p_say(Cpu *c) {
  push16(c, get_hl(c));
  set_hl(c, 0xB009); /* "You say "" */
  call(c, 0xA1E7, R_PRINT_MSG);
  set_hl(c, pop16(c));
  c->a = 0x01;
  c->mem[0xB704] = c->a;
  call(c, 0xA1F0, R_PRINT_MSG);
  set_hl(c, 0xB00F); /* "".", then JP $72DD */
  cpu_tail(c, R_PRINT_MSG);
}

/* JP nn as the last thing a routine does. */
static void tail(Cpu *c, uint16_t addr) {
  if (addr == R_SAY) return p_say(c);
  cpu_tail(c, addr);
}

static void tail_msg(Cpu *c, uint16_t hl, uint16_t addr) {
  set_hl(c, hl);
  tail(c, addr);
}

static uint16_t ix_at(const Cpu *c, int off) { return (uint16_t)(c->ix + off); }
static uint16_t iy_at(const Cpu *c, int off) { return (uint16_t)(c->iy + off); }

/* SUB A */
static void sub_a(Cpu *c) { c->a = op_sub(c, c->a, c->a, 0); }

/* $A5CA: Z if the object at IX is not open (BIT 5,(IX+7)); A=5. */
static void p_a5ca(Cpu *c) {
  op_bit_at(c, 5, c->mem[ix_at(c, 7)], ix_at(c, 7));
  c->a = 0x05;
}

/* $A541 ClimbOut (action $37): get out of the object you are in. */
static void p_climb_out(Cpu *c) {
  c->iy = rd16(c, CHAR_PTR);
  c->a = c->mem[CUR_OBJECT];
  op_cp(c, c->mem[iy_at(c, 1)]);
  if (!c->zf) return tail(c, R_ACTION_FAILS); /* not in it */
  call(c, 0xA54E, R_FIND_OBJECT);
  call(c, 0xA551, 0xA5CA);
  if (c->zf) return tail(c, R_A16C); /* closed */
  if (!check_phase(c, 0xA55A)) return;
  c->mem[iy_at(c, 1)] = 0xFF;
}

/* $A55F ClimbIn (action $36 in some objects' lists): get into the object,
 * if it is visible, not inside you, open, and has room for you. */
static void p_climb_in(Cpu *c) {
  c->iy = rd16(c, CHAR_PTR);
  c->a = c->mem[CUR_OBJECT];
  op_cp(c, c->mem[iy_at(c, 1)]);
  if (c->zf) return tail(c, R_ACTION_FAILS); /* already in it */
  c->iy = rd16(c, OBJ1_PTR);
  call(c, 0xA570, R_VISIBLE_TO);
  if (c->zf) return tail(c, R_ACTION_FAILS);
  c->a = c->mem[CUR_OBJECT];
  /* Up the chain of containers: is it inside the character? */
LA579:
  call(c, 0xA579, R_FIND_OBJECT);
  c->a = c->mem[ix_at(c, 1)];
  op_cp(c, 0xFF);
  if (c->zf) goto LA596;
  c->a = c->mem[CUR_CHAR];
  op_cp(c, c->mem[ix_at(c, 1)]);
  c->a = c->mem[ix_at(c, 1)];
  if (!c->zf) goto LA579;
  c->ix = rd16(c, OBJ1_PTR); /* it is: put it down */
  c->mem[ix_at(c, 1)] = 0xFF;
LA596:
  c->iy = rd16(c, CHAR_PTR);
  c->a = c->mem[CUR_CHAR];
  call(c, 0xA59D, R_CARRIED);
  c->a = op_add(c, c->a, c->mem[iy_at(c, 2)], 0);
  if (!c->cf) c->a = 0xFF;
  c->b = c->a;
  c->ix = rd16(c, OBJ1_PTR);
  call(c, 0xA5AC, 0xA5CA);
  if (c->zf) return tail(c, R_A16C);
  c->a = c->mem[ix_at(c, 2)];
  op_cp(c, 0xFF);
  if (!c->zf) {
    c->a = op_sub(c, c->a, c->b, 0);
    set_hl(c, MSG_B13E);
    if (!c->cf) return tail(c, R_PRINT_MSG); /* too small */
  }
  if (!check_phase(c, 0xA5C3)) return;
  c->a = c->mem[CUR_OBJECT];
  c->mem[iy_at(c, 1)] = c->a;
}

/* $A5D1 (script): the warg howls, if it is where you are. */
static void p_a5d1(Cpu *c) {
  if (!check_phase(c, 0xA5D4)) return;
  set_hl(c, OBJ_C452);
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, c->mem[get_hl(c)]);
  if (!c->zf) return;
  tail_msg(c, MSG_B085, R_SAY_DOT);
}

/* $A5E2 (object list, action 0): with the barrel at location $21, set
 * the timer of the event at $CA84 (thrown onto the bank) to 2. */
static void p_a5e2(Cpu *c) {
  c->a = c->mem[CUR_OBJECT];
  op_cp(c, 0x13);
  if (!c->zf) return;
  if (!check_phase2(c, 0xA5EB)) return;
  c->ix = BARREL;
  c->a = c->mem[ix_at(c, 0x10)];
  op_cp(c, 0x21);
  if (!c->zf) return;
  c->a = 0x02;
  c->mem[TIMER_CA85] = c->a;
}

/* $A5FB (timed): the barrel is washed onto the bank of the long lake. */
static void p_a5fb(Cpu *c) {
  sub_a(c);
  c->mem[TIMED_FIRED] = c->a;
  c->a = c->mem[YOUR_MOTHER];
  op_cp(c, 0x13); /* in the barrel? */
  set_hl(c, MSG_B32C);
  if (c->zf) call(c, 0xA607, R_PRINT_MSG);
  c->a = 0x22;
  c->mem[BARREL_LOCATION] = c->a;
  c->b = c->a;
  c->a = 0x13;
  call(c, 0xA612, R_MOVE_OBJECT);
  c->ix = BARREL;
  c->mem[ix_at(c, 0x10)] = 0x20;
  c->mem[ix_at(c, 7)] &= ~0x20;
  c->mem[ix_at(c, 7)] |= 0x04;
  sub_a(c);
  c->mem[NIGHT_DAY] = c->a;
  c->a = 0x13;
  call(c, 0xA62B, R_DESCRIBE);
  c->a = 0x01;
  c->mem[NIGHT_DAY] = c->a;
  c->iy = OBJ_C418;
  c->mem[iy_at(c, 0x10)] = 0x20;
  c->mem[iy_at(c, 1)] = 0x13;
}

/* $A640 (script): "where's the thief ?", when the character is where you
 * are(?) and you are not visible. */
static void p_a640(Cpu *c) {
  c->a = c->mem[CHAR_LOCATION2];
  set_hl(c, CHAR_LOCATION);
  op_cp(c, c->mem[get_hl(c)]);
  if (!c->zf) return;
  c->a = c->mem[YOUR_ATTRS];
  if (op_bit(c, 7, c->a)) return;
  if (!check_phase(c, 0xA651)) return;
  tail_msg(c, MSG_B0CB, R_SAY);
}

/* $A657 (script): Thorin, at random: waits, sits down and sings about
 * gold, says "get us out of this one, thief !" or "hurry up.". */
static void p_a657(Cpu *c) {
  if (!check_phase(c, 0xA65A)) return;
  c->a = 0x08;
  call(c, 0xA65C, R_RANDOM);
  op_cp(c, 0x05);
  if (!c->cf) return;
  op_cp(c, 0x03);
  set_hl(c, MSG_B0F4);
  if (!c->cf) return tail(c, R_SAY_DOT);
  set_hl(c, MSG_B0D5);
  if (c->zf) return tail(c, R_SAY);
  set_hl(c, MSG_B0E5);
  op_cp(c, 0x00);
  if (c->zf) return tail(c, R_SAY_DOT);
  tail_msg(c, MSG_B05B, R_SAY);
}

/* $A67E (object list, actions $10 and $0C): only from location $20 ("you
 * cannot reach"); then action $0C goes to $9138, others to $910E. */
static void p_a67e(Cpu *c) {
  c->ix = rd16(c, CHAR_PTR);
  c->a = c->mem[ix_at(c, 0x10)];
  op_cp(c, 0x20);
  set_hl(c, MSG_B11B);
  if (!c->zf) return tail(c, R_PRINT_MSG);
  c->a = c->mem[CUR_ACTION];
  op_cp(c, 0x0C);
  if (c->zf) return tail(c, R_9138);
  tail(c, R_910E);
}

/* $A698 (script): object $3C follows you into locations $27, $2C and $29:
 * "<it> enters." */
static void p_a698(Cpu *c) {
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, 0x27);
  if (c->zf) goto LA6A6;
  op_cp(c, 0x2C);
  if (c->zf) goto LA6A6;
  op_cp(c, 0x29);
  if (!c->zf) return;
LA6A6:
  if (!check_phase(c, 0xA6A9)) return;
  c->a = c->mem[YOUR_LOCATION];
  set_hl(c, OBJ_C143);
  op_cp(c, c->mem[get_hl(c)]);
  if (c->zf) return;
  c->mem[get_hl(c)] = c->a;
  c->a = 0x01;
  c->mem[NIGHT_DAY] = c->a;
  push16(c, OBJ_C13B);
  /* The message takes the pushed name off the stack. */
  msg(c, 0xA6BE, MSG_B017);
}

/* $A6C2 (script): the dragon, where you are, threatens you. */
static void p_a6c2(Cpu *c) {
  c->a = c->mem[YOUR_LOCATION];
  set_hl(c, CHAR_LOCATION2);
  op_cp(c, c->mem[get_hl(c)]);
  if (!c->zf) return;
  if (!check_phase(c, 0xA6CD)) return;
  c->a = c->mem[YOUR_ATTRS];
  c->a = op_and(c, c->a, 0x80);
  set_hl(c, MSG_B17E); /* you are invisible */
  if (!c->zf) set_hl(c, MSG_B15D);
  call(c, 0xA6BE, R_PRINT_MSG); /* $A6BE */
}

/* $A6DC (script): the dragon, unless it is at location $29, may burn you
 * where you are if the location is lit(?) (bit 7 of its first byte). */
static void p_a6dc(Cpu *c) {
  c->a = c->mem[OBJ_C5DD];
  op_cp(c, 0x29);
  if (c->zf) return;
  c->a = c->mem[YOUR_LOCATION];
  call(c, 0xA6E5, R_FIND_LOCATION);
  if (!op_bit_at(c, 7, c->mem[ix_at(c, 0)], ix_at(c, 0))) return;
  if (!check_phase(c, 0xA6F0)) return;
  c->a = 0x01;
  c->mem[NIGHT_DAY] = c->a;
  set_hl(c, MSG_B1A6);
  c->a = 0x64;
  call(c, 0xA6FA, R_RANDOM);
  op_cp(c, 0x50);
  if (c->cf) return call(c, 0xA6BE, R_PRINT_MSG); /* $A6BE */
  msg(c, 0xA704, MSG_B1BA);
  tail(c, R_YOU_ARE_DEAD);
}

/* $A71E (object list, action $1C): print $AFF4 if the character is
 * visible(?); otherwise copy $CAA7 to $CAA8 and print $B2AA. */
static void p_a71e(Cpu *c) {
  if (!check_phase(c, 0xA721)) return;
  c->ix = rd16(c, CHAR_PTR);
  bool set = op_bit_at(c, 7, c->mem[ix_at(c, 7)], ix_at(c, 7));
  set_hl(c, MSG_AFF4);
  if (set) return tail(c, R_PRINT_MSG);
  c->a = c->mem[VAR_CAA7];
  c->mem[VAR_CAA8] = c->a;
  tail_msg(c, MSG_B2AA, R_PRINT_MSG);
}

/* $A73B (object list, action 0): once object $C3B2 is broken, break
 * $C1A6 as well and ... (A18C with 2), and print $B109 if you see it. */
static void p_a73b(Cpu *c) {
  set_hl(c, OBJ_C3B9);
  if (!op_bit(c, 3, c->mem[get_hl(c)])) return;
  if (!check_phase2(c, 0xA744)) return;
  c->ix = OBJ_C1AD;
  c->mem[ix_at(c, 7)] |= 0x08;
  c->a = 0x02;
  call(c, 0xA74E, R_A18C);
  c->a = 0x02;
  c->iy = YOU;
  call(c, 0xA757, R_VISIBLE_SWAP);
  if (c->zf) return;
  tail_msg(c, MSG_B109, R_PRINT_MSG);
}

/* $A761 (object list, actions $0C $18): for you, only from inside
 * something ("you cannot reach" if not in anything, else not possible);
 * others: action $0C $9138, $10 $910E. */
static void p_a761(Cpu *c) {
  c->a = c->mem[CUR_CHAR];
  op_cp(c, 0x00);
  if (!c->zf) goto LA776;
  c->a = c->mem[YOUR_MOTHER];
  set_hl(c, MSG_B11B);
  op_cp(c, 0xFF);
  if (c->zf) return tail(c, R_PRINT_MSG);
  return tail(c, R_ACTION_FAILS);
LA776:
  c->a = c->mem[CUR_ACTION];
  op_cp(c, 0x0C);
  if (c->zf) return tail(c, R_9138);
  op_cp(c, 0x10);
  if (c->zf) return tail(c, R_910E);
}

/* $A784 (object list, actions $1E $0B $10): for you, only from inside
 * something; then action $1E $8F3B, $0B $92ED, $18 $8EEC. */
static void p_a784(Cpu *c) {
  c->a = c->mem[CUR_CHAR];
  op_cp(c, 0x00);
  if (!c->zf) goto LA797;
  c->a = c->mem[YOUR_MOTHER];
  op_cp(c, 0xFF);
  set_hl(c, MSG_B11B);
  if (c->zf) return tail(c, R_PRINT_MSG);
LA797:
  c->a = c->mem[CUR_ACTION];
  op_cp(c, 0x1E);
  if (c->zf) return tail(c, R_8F3B);
  op_cp(c, 0x0B);
  if (c->zf) return tail(c, R_92ED);
  op_cp(c, 0x18);
  if (c->zf) return tail(c, R_8EEC);
}

/* $A7AA (timed): print $B153; you die unless $CAA1 is set, which is
 * cleared first if you are at location $1D. */
static void p_a7aa(Cpu *c) {
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, 0x1D);
  if (c->zf) {
    sub_a(c);
    c->mem[VAR_CAA1] = c->a;
  }
  msg(c, 0xA7B8, MSG_B153);
  c->a = c->mem[VAR_CAA1];
  op_cp(c, 0x00);
  if (!c->zf) return;
  tail(c, R_YOU_ARE_DEAD);
}

/* $A7C4 (the map, action $1C read): for character $41 (?), the map's clue: "<dir> go from
 * <place> to get to <place>", from the record at MAP_RECORD (6 bytes:
 * place, pointer, 3 bytes for it, place), copied in the first time. Other
 * objects: $93DA. */
static void p_a7c4(Cpu *c) {
  c->a = c->mem[CUR_CHAR];
  op_cp(c, 0x41);
  if (!c->zf) return tail(c, R_HELP);
  if (!check_phase(c, 0xA7CF)) return;
  c->iy = rd16(c, MAP_RECORD);
  c->l = c->mem[iy_at(c, 1)];
  c->h = c->mem[iy_at(c, 2)];
  c->a = c->mem[MAP_DRAWN];
  op_cp(c, 0x00);
  if (c->zf) {
    c->b = 0x03;
    do {
      c->a = c->mem[iy_at(c, 3)];
      c->mem[get_hl(c)] = c->a;
      set_hl(c, get_hl(c) + 1);
      c->iy++;
    } while (--c->b);
    c->iy = rd16(c, MAP_RECORD);
  }
  c->a = c->mem[iy_at(c, 5)];
  call(c, 0xA7F2, R_FIND_LOCATION);
  c->ix += 2;
  push16(c, c->ix);
  c->a = c->mem[iy_at(c, 0)];
  call(c, 0xA7FE, R_FIND_LOCATION);
  c->ix += 2;
  push16(c, c->ix);
  c->a = c->mem[iy_at(c, 3)];
  call(c, 0xA80A, R_TOKEN_NAME);
  push16(c, get_de(c));
  /* The message takes its three arguments off the stack. */
  tail_msg(c, MSG_B1D0, R_SAY);
}

/* $A86E: C (half the time) from a random number below $64. */
static void p_a86e(Cpu *c) {
  c->a = 0x64;
  call(c, 0xA870, R_RANDOM);
  op_cp(c, 0x32);
}

/* $A814 (object list, action $2B): throw or swing at the second object(?)
 * with chances of missing; may set $C61C to $12, or move the first object
 * to the second's location. */
static void p_a814(Cpu *c) {
  c->a = c->mem[CUR_OBJECT2];
  call(c, 0xA817, R_9F28);
  op_cp(c, 0xFF);
  if (c->zf) return tail(c, R_ACTION_FAILS);
  if (!check_phase(c, 0xA822)) return;
  msg(c, 0xA825, MSG_AF66);
  c->a = c->mem[OBJ_C62B];
  op_cp(c, c->mem[ix_at(c, 2)]);
  if (!c->zf) goto LA84C;
  call(c, 0xA830, 0xA86E);
  if (c->cf) goto LA842;
  set_hl(c, MSG_AF76);
  call(c, 0xA838, 0xA86E);
  if (c->cf) goto LA86B;
  set_hl(c, MSG_AF82);
  goto LA86B;
LA842:
  c->a = 0x12;
  c->mem[OBJ_C61C] = c->a;
  set_hl(c, MSG_AF8B);
  goto LA86B;
LA84C:
  set_hl(c, MSG_AF76);
  call(c, 0xA84F, 0xA86E);
  if (c->cf) goto LA86B;
  c->a = c->mem[ix_at(c, 2)];
  c->ix = rd16(c, OBJ1_PTR);
  c->mem[ix_at(c, 0x10)] = c->a;
  c->mem[ix_at(c, 1)] = 0xFF;
  c->a = c->mem[CUR_OBJECT];
  call(c, 0xA865, R_MOVE_OBJECT);
  set_hl(c, MSG_AF6D);
LA86B:
  tail(c, R_PRINT_MSG);
}

/* $A882: print HL, then object $29 moves to location $42 or $43 (the other
 * one from $C62B), and $C61C is cleared. */
static void la882(Cpu *c) {
  call(c, 0xA882, R_PRINT_MSG);
  c->a = c->mem[OBJ_C62B];
  op_cp(c, 0x42);
  c->a = 0x42;
  if (c->zf) c->a = 0x43;
  c->mem[OBJ_C62B] = c->a;
  c->b = c->a;
  c->a = 0xFF;
  c->mem[OBJ_C61C] = c->a;
  c->a = 0x29;
  tail(c, R_MOVE_OBJECT);
}

/* $A876 (object list, action $31): if $C61C is $12, print $AF92 and
 * $A882. */
static void p_a876(Cpu *c) {
  if (!check_phase(c, 0xA879)) return;
  c->a = c->mem[OBJ_C61C];
  op_cp(c, 0x12);
  if (!c->zf) return;
  set_hl(c, MSG_AF92);
  la882(c);
}

/* $A89E (object list, action 0): for you, print $AFA4 and $A882. */
static void p_a89e(Cpu *c) {
  if (!check_phase2(c, 0xA8A1)) return;
  c->a = c->mem[CUR_CHAR];
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) return;
  set_hl(c, MSG_AFA4);
  la882(c);
}

/* $A8AB (script, two-phase): once $B6F4 is 1, clear it, and if $7F1A
 * (with A=1) gives NZ, rewrite the script op at $C9E2 as $42 with
 * $B6E6 and the current objects. */
static void p_a8ab(Cpu *c) {
  c->a = c->mem[VAR_B6F4];
  op_cp(c, 0x01);
  if (!c->zf) return;
  sub_a(c);
  c->mem[VAR_B6F4] = c->a;
  c->a = op_inc(c, c->a);
  call(c, 0xA8B6, R_TALK);
  if (c->zf) return;
  sub_a(c);
  c->mem[PHASE_OK] = c->a;
  c->a = c->mem[VAR_B6E6];
  c->mem[SCRIPT_C9E2 + 1] = c->a;
  c->c = c->mem[CUR_OBJECT];
  c->b = c->mem[CUR_OBJECT2];
  c->mem[SCRIPT_C9E2 + 2] = c->c;
  c->mem[SCRIPT_C9E2 + 3] = c->b;
  c->a = 0x42;
  c->mem[SCRIPT_C9E2] = c->a;
}

/* $A8D2 (script): when the character is at $C4C1's location and you are
 * visible, say the message of the current script record (+2), and set
 * $B6F9. */
static void p_a8d2(Cpu *c) {
  c->a = c->mem[OBJ_C4C1];
  set_hl(c, CHAR_LOCATION);
  op_cp(c, c->mem[get_hl(c)]);
  if (!c->zf) return;
  set_hl(c, YOUR_ATTRS);
  if (!op_bit(c, 7, c->mem[get_hl(c)])) return;
  if (!check_phase(c, 0xA8E3)) return;
  uint16_t hl = (uint16_t)(rd16(c, CUR_SCRIPT) + 2);
  c->e = c->mem[hl];
  hl++;
  c->d = c->mem[hl];
  set_hl(c, get_de(c));
  call(c, 0xA8ED, R_SAY);
  c->a = 0x01;
  c->mem[VAR_B6F9] = c->a;
}

/* CPIR */
static void cpir(Cpu *c) {
  uint16_t hl = get_hl(c), bc = get_bc(c);
  uint8_t r, v;
  do {
    v = c->mem[hl++];
    r = (uint8_t)(c->a - v);
    bc--;
  } while (bc != 0 && r != 0);
  c->sf = r >> 7;
  c->zf = r == 0;
  c->hf = (c->a & 0x0F) < (v & 0x0F);
  c->pf = bc != 0;
  c->nf = 1;
  set_hl(c, hl), set_bc(c, bc);
}

/* $A8F6 (script, two-phase): clear $B6F9; unless you answered with the
 * two bytes at the current script record (searched for in the speech
 * record from $7F1A), someone strangles you from behind. */
static void p_a8f6(Cpu *c) {
  if (!check_phase(c, 0xA8F9)) return;
  sub_a(c);
  c->mem[VAR_B6F9] = c->a;
  call(c, 0xA8FD, R_TALK);
  if (c->zf) goto LA915;
  set_bc(c, 0x0018);
  set_de(c, rd16(c, CUR_SCRIPT));
  c->a = c->mem[get_de(c)];
LA90A:
  cpir(c);
  if (!c->zf) goto LA915;
  set_de(c, get_de(c) + 1);
  c->a = c->mem[get_de(c)];
  set_de(c, get_de(c) - 1);
  op_cp(c, c->mem[get_hl(c)]);
  if (!c->zf) goto LA90A;
  return;
LA915:
  if (!check_phase(c, 0xA918)) return;
  c->a = 0x01;
  c->mem[NIGHT_DAY] = c->a;
  msg(c, 0xA920, MSG_B1DB);
  tail(c, R_YOU_ARE_DEAD);
}

/* $A926 (script): when at $C4C1's location, say $B2D9 (7 times in 8), or
 * $B2E7 unless $C31A is in object $44. */
static void p_a926(Cpu *c) {
  c->a = c->mem[OBJ_C4C1];
  set_hl(c, CHAR_LOCATION);
  op_cp(c, c->mem[get_hl(c)]);
  if (!c->zf) return;
  if (!check_phase(c, 0xA931)) return;
  set_hl(c, MSG_B2D9);
  c->a = 0x08;
  call(c, 0xA936, R_RANDOM);
  if (!c->cf) return tail(c, R_SAY);
  c->ix = OBJ_C31A;
  c->a = 0x44;
  op_cp(c, c->mem[ix_at(c, 1)]);
  if (c->zf) return tail(c, R_SAY);
  tail_msg(c, MSG_B2E7, R_SAY);
}

/* $A94E (trolls' script): if the character is where you are(?), it eats
 * you (action $1B on object 0) and you are dead. */
static void p_a94e(Cpu *c) {
  c->a = c->mem[CHAR_LOCATION2];
  set_hl(c, CHAR_LOCATION);
  op_cp(c, c->mem[get_hl(c)]);
  if (!c->zf) return;
  if (!check_phase(c, 0xA959)) return;
  c->a = 0x1B;
  c->mem[CUR_ACTION] = c->a;
  c->a = 0x00;
  c->mem[CUR_OBJECT] = c->a;
  c->a = 0xFF;
  c->mem[CUR_OBJECT2] = c->a;
  call(c, 0xA968, R_ACTION_FAILS);
  call(c, 0xA96B, R_92B5);
  tail(c, R_YOU_ARE_DEAD);
}

/* $A971 (trolls' script): day dawns, and the trolls ($47, $48) turn to
 * stone: they die and are no longer visible, the clearing ($05) gets its
 * new description and a cyan paper colour. */
static void p_a971(Cpu *c) {
  if (!check_phase(c, 0xA974)) return;
  c->a = 0x47;
  call(c, 0xA976, R_KILL);
  c->a = 0x48;
  call(c, 0xA97B, R_KILL);
  c->mem[TROLL1_ATTRS] &= ~0x80;
  c->mem[TROLL2_ATTRS] &= ~0x80;
  set_hl(c, MSG_B262);
  wr16(c, TROLLS_DESC, get_hl(c));
  set_hl(c, TROLLS_LOCATION);
  c->mem[TROLLS_LOCATION] &= ~0x40;
  c->a = 0x47;
  call(c, 0xA995, R_DESCRIBE);
  c->a = 0x48;
  call(c, 0xA99A, R_DESCRIBE);
  set_hl(c, MSG_B30B);
  c->a = 0x01;
  c->mem[NIGHT_DAY] = c->a;
  call(c, 0xA9A5, R_PRINT_MSG);
  c->ix = PICTURES;
  c->a = 0x05;
  call(c, 0xA9AE, R_FIND_3BYTE);
  c->l = c->mem[ix_at(c, 1)];
  c->h = c->mem[ix_at(c, 2)];
  c->mem[get_hl(c)] = 0x05;
  set_hl(c, get_hl(c) + 1);
  c->mem[get_hl(c)] = 0x28;
}

/* $A9BD (trolls' script): at location 5, a troll says "blimey, look at
 * this!! Can yer cook'em?" (the troll $47) or "yer can try, but he
 * wouldn't make above a mouthfull.". */
static void p_a9bd(Cpu *c) {
  c->a = c->mem[CHAR_LOCATION];
  op_cp(c, 0x05);
  if (!c->zf) return;
  if (!check_phase(c, 0xA9C6)) return;
  c->a = c->mem[CUR_CHAR];
  set_hl(c, MSG_B238);
  op_cp(c, 0x47);
  if (!c->zf) set_hl(c, MSG_B24C);
  tail(c, R_SAY);
}

/* $A9D6 (called by $96B3 each turn): when $C5CE is at location $25, print
 * $B3D9, wait for a key and restart (the end of the game). */
static void p_a9d6(Cpu *c) {
  c->a = c->mem[OBJ_C5CE];
  op_cp(c, 0x25);
  if (!c->zf) return;
  msg(c, 0xA9DF, MSG_B3D9);
  tail(c, R_WAIT_RESTART);
}

/* $A9E5 (script): when the character is where you are(?) and object $C663
 * is nowhere or in object $41, it gives it ($26) to you: action $1D. */
static void p_a9e5(Cpu *c) {
  c->a = c->mem[CHAR_LOCATION2];
  set_hl(c, CHAR_LOCATION);
  op_cp(c, c->mem[get_hl(c)]);
  if (!c->zf) return;
  c->ix = OBJ_C663;
  c->a = c->mem[ix_at(c, 0x10)];
  op_cp(c, 0x00);
  if (!c->zf) {
    c->a = 0x41;
    op_cp(c, c->mem[ix_at(c, 1)]);
    if (!c->zf) return;
  }
  if (!check_phase(c, 0xAA01)) return;
  c->a = c->mem[CHAR_LOCATION2];
  c->mem[ix_at(c, 0x10)] = c->a;
  c->mem[ix_at(c, 1)] = 0x41;
  set_bc(c, 0x0026);
  c->mem[CUR_OBJECT] = c->c;
  c->mem[CUR_OBJECT2] = c->b;
  c->a = 0x1D;
  c->mem[CUR_ACTION] = c->a;
  wr16(c, OBJ1_PTR, c->ix);
  set_hl(c, YOU);
  wr16(c, OBJ2_PTR, get_hl(c));
  call(c, 0xAA21, R_ACTION_FAILS);
  tail(c, R_939E);
}

/* $AA27 (object list, action $38): go through the object to where it
 * leads ($9F2D on its location); if that is not a $0A exit, the original
 * jumps to $B301, a message, as code. */
static void p_aa27(Cpu *c) {
  c->iy = rd16(c, OBJ1_PTR);
  c->a = c->mem[iy_at(c, 0x10)];
  c->b = c->a;
  call(c, 0xAA2F, R_9F2D);
  op_cp(c, 0xFF);
  set_hl(c, MSG_B301);
  if (c->zf) return tail(c, R_PRINT_MSG);
  c->a = c->mem[ix_at(c, 0)];
  op_cp(c, 0x0A);
  if (!c->zf) return tail(c, MSG_B301); /* sic */
  if (!check_phase(c, 0xAA45)) return;
  c->a = c->mem[CUR_OBJECT];
  c->ix = rd16(c, CHAR_PTR);
  c->mem[ix_at(c, 1)] = c->a;
  call(c, 0xAA4F, R_MOVE_OBJECT);
  c->a = c->mem[CUR_CHAR];
  op_cp(c, 0x00);
  if (!c->zf) return;
  c->a = c->b;
  tail(c, R_9630);
}

/* $AA5C (timed): object $C205 is no longer broken or open, its strength
 * doubles, and its +$0A,+$0B become $0623. */
static void p_aa5c(Cpu *c) {
  c->ix = OBJ_C205;
  c->mem[ix_at(c, 7)] &= ~0x08;
  c->mem[ix_at(c, 7)] &= ~0x20;
  c->mem[ix_at(c, 5)] = op_sla(c, c->mem[ix_at(c, 5)]);
  set_de(c, 0x0623);
  wr16(c, OBJ_C20F, get_de(c));
}

/* $AA74 (timed): unless the hole ($0B) is open, copy $CAC3 to $CAC4 and
 * make it invisible; "the hole vanishes" if you are at location $2A. */
static void p_aa74(Cpu *c) {
  set_hl(c, OBJ_C2BC);
  if (op_bit(c, 5, c->mem[get_hl(c)])) return;
  c->a = c->mem[VAR_CAC3];
  c->mem[VAR_CAC4] = c->a;
  set_hl(c, OBJ_C2BC);
  c->mem[OBJ_C2BC] &= ~0x80;
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, 0x2A);
  if (!c->zf) return;
  tail_msg(c, MSG_B2A4, R_PRINT_MSG); /* $AA8B */
}

/* $AA91 (timed): the hole appears; $B277 if you are at location $2A. */
static void p_aa91(Cpu *c) {
  set_hl(c, OBJ_C2BC);
  c->mem[OBJ_C2BC] |= 0x80;
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, 0x2A);
  if (!c->zf) return;
  tail_msg(c, MSG_B277, R_PRINT_MSG);
}

/* $AAA2 (object list, action 0): the hole closes (timer $CAC4 = 6,
 * locked, invisible): "the hole vanishes" if you are at $2A. */
static void p_aaa2(Cpu *c) {
  if (!check_phase2(c, 0xAAA5)) return;
  c->a = 0x06;
  c->mem[VAR_CAC4] = c->a;
  set_hl(c, OBJ_C2BC);
  c->mem[OBJ_C2BC] |= 0x01;
  c->mem[OBJ_C2BC] &= ~0x80;
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, 0x2A);
  if (!c->zf) return;
  tail_msg(c, MSG_B2A4, R_PRINT_MSG);
}

/* $AAC7: print HL if you are at location $1E or $1C (either side of the
 * magic door). */
static void p_aac7(Cpu *c) {
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, 0x1E);
  if (c->zf) return tail(c, R_PRINT_MSG);
  op_cp(c, 0x1C);
  if (c->zf) return tail(c, R_PRINT_MSG);
}

/* $AAE0 (timed, and after the door opens): if $C31A is inside something,
 * that becomes the current character, and unless it is visible, $A3BC. */
static void p_aae0(Cpu *c) {
  c->ix = OBJ_C31A;
  c->a = c->mem[ix_at(c, 1)];
  op_cp(c, 0xFF);
  if (c->zf) return;
  call(c, 0xAAEA, R_FIND_OBJECT);
  wr16(c, CHAR_PTR, c->ix);
  if (!op_bit_at(c, 7, c->mem[ix_at(c, 7)], ix_at(c, 7))) return tail(c, R_A3BC);
}

/* $AAB3 (timed): the magic door opens. */
static void p_aab3(Cpu *c) {
  set_hl(c, OBJ_C388);
  c->mem[OBJ_C388] |= 0x20;
  set_hl(c, MSG_B037);
  call(c, 0xAABB, 0xAAC7);
  set_hl(c, MSG_B113);
  call(c, 0xAAC1, 0xAAC7);
  p_aae0(c);
}

/* $AAD5 (timed): the magic door closes. */
static void p_aad5(Cpu *c) {
  set_hl(c, OBJ_C388);
  c->mem[OBJ_C388] &= ~0x20;
  set_hl(c, MSG_B03F);
  p_aac7(c);
}

/* $AAF9 (object list, action 0): for you, set $B700 and copy $CAB5 to
 * $CAB6 (start the timer of the event $AB0B). */
static void p_aaf9(Cpu *c) {
  c->a = c->mem[CUR_CHAR];
  op_cp(c, 0x00);
  if (!c->zf) return;
  c->a = 0x01;
  c->mem[VAR_B700] = c->a;
  c->a = c->mem[VAR_CAB5];
  c->mem[VAR_CAB6] = c->a;
}

/* $AB0B (timed): clear $B700. */
static void p_ab0b(Cpu *c) {
  sub_a(c);
  c->mem[VAR_B700] = c->a;
}

/* $AB10 (timed): at location $1A, the spider web smothers you. */
static void p_ab10(Cpu *c) {
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, 0x1A);
  if (!c->zf) return;
  msg(c, 0xAB19, MSG_B0FD);
  tail(c, R_YOU_ARE_DEAD);
}

static void lab4a(Cpu *c) {
  msg(c, 0xAB4D, MSG_B31F);
  tail(c, R_YOU_ARE_DEAD);
}

/* $AB1F (timed, second): pale bulbous eyes stare at you; something stings
 * you unless you stay where you were, or are at location 2 or 3 (3 if
 * you were at 2). */
static void p_ab1f(Cpu *c) {
  msg(c, 0xAB22, MSG_B311);
  c->a = c->mem[YOUR_LOCATION];
  c->c = c->a;
  set_hl(c, PREV_LOCATION);
  op_cp(c, c->mem[get_hl(c)]);
  if (c->zf) return;
  c->b = c->mem[get_hl(c)];
  c->a = 0x02;
  op_cp(c, c->b);
  if (!c->zf) c->a = 0x02;
  else c->a = 0x03;
  op_cp(c, c->c);
  if (c->zf) return;
  lab4a(c);
}

/* $AB3A (timed): at locations 2 and 3, pale bulbous eyes, and something
 * drops from above and stings: you are dead. */
static void p_ab3a(Cpu *c) {
  c->a = c->mem[YOUR_LOCATION];
  op_cp(c, 0x02);
  if (!c->zf) {
    op_cp(c, 0x03);
    if (!c->zf) return;
  }
  msg(c, 0xAB47, MSG_B311);
  lab4a(c);
}

/* ---------- location events ($C7A4-$C7FB, in the data area) ----------
 * Reached through the table at $C78E (3-byte records: location, handler)
 * by $8E24-$8E36 when you arrive somewhere, through $9B6C. */

/* $C7A4 (Beorn's house, $16): unless the butler ($42) is dead (broken),
 * make him visible and start his script (the character table's slot at
 * $CAE7). */
static void p_c7a4(Cpu *c) {
  set_hl(c, 0xC437); /* the butler's attributes */
  if (op_bit(c, 3, c->mem[0xC437])) return;
  c->a = 0x42;
  c->mem[0xCAE7] = c->a;
  c->mem[0xC437] |= 0x80;
}

/* $C7B2 (the spider threads place, $1A): start the timer of $AB10 (the web
 * smothers you). */
static void p_c7b2(Cpu *c) {
  c->a = c->mem[0xCA92];
  c->mem[0xCA93] = c->a;
}

/* $C7B9 (the deep bog, $1D): start the timer of $A7AA. */
static void p_c7b9(Cpu *c) {
  c->a = c->mem[0xCAA0];
  c->mem[VAR_CAA1] = c->a;
}

/* $C7C0 (the elvenking's cellar, $20): start the side door timer (3), and
 * start the scripts of the dragon ($3C, slot $CB03) and Bard ($46, slot
 * $CAFC) unless they are dead. */
static void p_c7c0(Cpu *c) {
  c->a = 0x03;
  c->mem[VAR_CAC4] = c->a;
  set_hl(c, 0xC13A); /* the dragon's attributes */
  if (!op_bit(c, 3, c->mem[0xC13A])) {
    c->a = 0x3C;
    c->mem[0xCB03] = c->a;
  }
  set_hl(c, 0xC4CA); /* Bard's attributes */
  if (op_bit(c, 3, c->mem[0xC4CA])) return;
  c->a = 0x46;
  c->mem[0xCAFC] = c->a;
}

/* $C7DD (the forest, $02 and $03): remember where you came in ($8D9B, the
 * location being entered) and start the eyes' timer ($AB3A/$AB1F). */
static void p_c7dd(Cpu *c) {
  c->a = c->mem[0x8D9B];
  c->mem[PREV_LOCATION] = c->a;
  c->a = c->mem[0xCABC];
  c->mem[0xCABD] = c->a;
}

/* $C7EA (the forest river, $21): unless you are in the barrel, you are
 * swept against the rocks and die. */
static void p_c7ea(Cpu *c) {
  c->a = c->mem[YOUR_MOTHER];
  op_cp(c, 0x13);
  if (c->zf) return;
  call(c, 0xC7F0, 0x8E39);
  msg(c, 0xC7F6, 0xB26D); /* "You are swept forcefully against the..." */
  tail(c, R_YOU_ARE_DEAD);
}

/* $8EF8: the second half of LookThrough ($8EEC, in actions1.c), which the
 * rivers ($09, $2A) list as their own handler for action $19 (look
 * across): look into the location beyond. */
static void p_8ef8(Cpu *c) {
  c->a = c->mem[CUR_CHAR];
  call(c, 0x8EFB, 0x8ED2);
  if (!c->zf) return;
  call(c, 0x8EFF, 0x9F25);
  op_cp(c, 0xFF);
  if (c->zf) return tail(c, R_ACTION_FAILS);
  c->a = c->mem[ix_at(c, 2)];
  op_cp(c, 0x00);
  if (c->zf) return tail(c, R_ACTION_FAILS);
  if (!check_phase(c, 0x8F12)) return;
  push16(c, c->ix);
  call(c, 0x8F14, R_FIND_LOCATION);
  bool lit = op_bit_at(c, 7, c->mem[ix_at(c, 0)], ix_at(c, 0));
  c->ix = pop16(c);
  if (!lit) return tail_msg(c, 0xAE1F, R_PRINT_MSG); /* "it is dark" */
  c->iy = rd16(c, CHAR_PTR);
  c->a = c->mem[iy_at(c, 0x10)];
  cpu_push_af(c);
  c->a = c->mem[ix_at(c, 2)];
  c->mem[iy_at(c, 0x10)] = c->a;
  call(c, 0x8F2D, 0x962B);
  cpu_pop_af(c);
  c->mem[iy_at(c, 0x10)] = c->a;
}

#define OUT_DEFAULT (OUT_REGS | OUT_ZF | OUT_CF)
const PortRoutine events_routines[] = {
    {0xA1E3, "Say", p_say, OUT_DEFAULT},
    {0xA541, "ClimbOut", p_climb_out, OUT_DEFAULT},
    {0xA55F, "ClimbIn", p_climb_in, OUT_DEFAULT},
    {0xA5CA, "IsOpen", p_a5ca, OUT_DEFAULT},
    {0xA5D1, "WargHowls", p_a5d1, OUT_DEFAULT},
    {0xA5E2, "BarrelTimer", p_a5e2, OUT_DEFAULT},
    {0xA5FB, "BarrelAshore", p_a5fb, OUT_DEFAULT},
    {0xA640, "WheresThief", p_a640, OUT_DEFAULT},
    {0xA657, "ThorinTalks", p_a657, OUT_DEFAULT},
    {0xA67E, "Ev_A67E", p_a67e, OUT_DEFAULT},
    {0xA698, "Ev_A698", p_a698, OUT_DEFAULT},
    {0xA6C2, "DragonTalks", p_a6c2, OUT_DEFAULT},
    {0xA6DC, "DragonBurns", p_a6dc, OUT_DEFAULT},
    {0xA71E, "Ev_A71E", p_a71e, OUT_DEFAULT},
    {0xA73B, "Ev_A73B", p_a73b, OUT_DEFAULT},
    {0xA761, "Ev_A761", p_a761, OUT_DEFAULT},
    {0xA784, "Ev_A784", p_a784, OUT_DEFAULT},
    {0xA7AA, "Ev_A7AA", p_a7aa, OUT_DEFAULT},
    {0xA7C4, "ReadMap", p_a7c4, OUT_DEFAULT},
    {0xA814, "Ev_A814", p_a814, OUT_DEFAULT},
    {0xA86E, "Chance50", p_a86e, OUT_DEFAULT},
    {0xA876, "Ev_A876", p_a876, OUT_DEFAULT},
    {0xA89E, "Ev_A89E", p_a89e, OUT_DEFAULT},
    {0xA8AB, "Ev_A8AB", p_a8ab, OUT_DEFAULT},
    {0xA8D2, "Ev_A8D2", p_a8d2, OUT_DEFAULT},
    {0xA8F6, "Strangle", p_a8f6, OUT_DEFAULT},
    {0xA926, "Ev_A926", p_a926, OUT_DEFAULT},
    {0xA94E, "TrollsEat", p_a94e, OUT_DEFAULT},
    {0xA971, "DayDawns", p_a971, OUT_DEFAULT},
    {0xA9BD, "TrollsTalk", p_a9bd, OUT_DEFAULT},
    {0xA9D6, "GameWon", p_a9d6, OUT_DEFAULT},
    {0xA9E5, "Ev_A9E5", p_a9e5, OUT_DEFAULT},
    {0xAA27, "Ev_AA27", p_aa27, OUT_DEFAULT},
    {0xAA5C, "Ev_AA5C", p_aa5c, OUT_DEFAULT},
    {0xAA74, "HoleVanishes", p_aa74, OUT_DEFAULT},
    {0xAA91, "HoleAppears", p_aa91, OUT_DEFAULT},
    {0xAAA2, "HoleCloses", p_aaa2, OUT_DEFAULT},
    {0xAAB3, "DoorOpens", p_aab3, OUT_DEFAULT},
    {0xAAC7, "AtTheDoor", p_aac7, OUT_DEFAULT},
    {0xAAD5, "DoorCloses", p_aad5, OUT_DEFAULT},
    {0xAAE0, "Ev_AAE0", p_aae0, OUT_DEFAULT},
    {0xAAF9, "Ev_AAF9", p_aaf9, OUT_DEFAULT},
    {0xAB0B, "Ev_AB0B", p_ab0b, OUT_DEFAULT},
    {0xAB10, "WebSmothers", p_ab10, OUT_DEFAULT},
    {0xAB1F, "EyesStare", p_ab1f, OUT_DEFAULT},
    {0xAB3A, "SomethingStings", p_ab3a, OUT_DEFAULT},
    {0xC7A4, "EvButler", p_c7a4, OUT_DEFAULT},
    {0xC7B2, "EvSpiderPlace", p_c7b2, OUT_DEFAULT},
    {0xC7B9, "EvDeepBog", p_c7b9, OUT_DEFAULT},
    {0xC7C0, "EvCellar", p_c7c0, OUT_DEFAULT},
    {0xC7DD, "EvForest", p_c7dd, OUT_DEFAULT},
    {0xC7EA, "EvForestRiver", p_c7ea, OUT_DEFAULT},
    {0x8EF8, "LookAcross", p_8ef8, OUT_DEFAULT},
    {0, NULL, NULL, 0},
};
