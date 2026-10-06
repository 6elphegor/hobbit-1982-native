/* Actions: exits, tie, untie, burn, capture ($A138-$A540).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * Most of these are action handlers: the address is in the action table
 * ($C730, by verb) or in an object's own list (after its locations: action
 * id, handler), or in a character's script, and is reached through
 * TriggerAction ($9B6C: CALL $9B80 = JP (HL)), so with a return address on
 * top of the stack. Each handler runs twice for a command: first with
 * DOING ($B6FA) = 0 to see whether it would work, then with DOING = 1 to do
 * it. The test is CALL $9D44 (check_only below): when only checking, it
 * records success in OK ($B6FB) and abandons the handler there.
 *
 * Labels keep the original addresses. */
#include <stddef.h>
#include <stdlib.h>

#include "cpu.h"
#include "routines.h"

/* Variables. */
#define ACTION 0xB6E7     /* the action (verb) being done, $01-$3B (table at $AB53) */
#define OBJ1 0xB6E8       /* its first object (id), $FF for none */
#define OBJ2 0xB6E9       /* its second object */
#define ACTOR 0xB6EA      /* who is doing it: 0 = you, $3C the dragon, $40 the wood elf... */
#define VAR_B6F5 0xB6F5   /* unknown: compared with $B6F6 by $A525 */
#define VAR_B6F6 0xB6F6   /* unknown: a location id (where a captured character is not sent) */
#define DOING 0xB6FA      /* 1: carry the action out; 0: only check that it can be done */
#define OK 0xB6FB         /* result of the check */
#define OBJ1_PTR 0xB708   /* object data of OBJ1 */
#define OBJ2_PTR 0xB70A   /* object data of OBJ2 */
#define ACTOR_PTR 0xB70C  /* object data of ACTOR */
#define ROPE_MOTHER 0xC367 /* the rope ($12, data at $C366): what it is tied to */
#define PLAYER 0xC11B     /* your own object data (object $00) */
#define PLAYER_LOC 0xC12B /* your location */
#define RING 0xC31A       /* the golden ring (object $10) */
#define GOBLIN_DOOR_ATTR 0xC34B /* attributes of the goblins' door (object $11) */
#define VAR_CA8B 0xCA8B   /* timed events ($CA84, 7 bytes each): */
#define VAR_CA8C 0xCA8C   /* $CA8B/$CA8C: the spider event's period and count */
#define VAR_CA99 0xCA99   /* $CA99/$CA9A: the goblins' door event (closes it, $A4D9) */
#define VAR_CA9A 0xCA9A
#define RING_TIMER 0xCAAF /* turns of invisibility left while wearing the ring */
#define ACTIONS_EXITS 0xA20B /* 5 actions that are checked by $A1D0 */
#define WORD_TABLE 0xA20E /* dictionary offsets by direction (used through $A0BA) */
#define WORD_TABLE_A224 0xA224 /* ... words for $A172, by A (bit 7 clear) */
#define WORD_TABLE_A234 0xA234 /* ... and with bit 7 set */
#define ESCAPES 0xA49C    /* 6 bytes a goblin: id, address to store it, location, 2 bytes */

/* Object data: +$01 mother object, +$04 size flags (?), +$05 strength,
 * +$07 attributes (7 visible, 6 animal, 5 open, 4 light, 3 broken/dead,
 * 2 full, 1 fluid, 0 locked), +$08 name (3 words), +$0A/+$0B a word
 * written by some actions, +$0C/+$0D, +$10 location (first of +$00 of them),
 * then the object's own actions (id, handler) up to $FF. */

/* Messages. */
#define MSG_IS 0xAFBB         /* "[object] is[word]." (two arguments on the stack) */
#define MSG_EXITS 0xB020      /* "visible exits are:" */
#define MSG_ALREADY_TIED 0xB07D /* "[object] is already tied." */
#define MSG_NOT_TIED 0xB092
#define MSG_B0B4 0xB0B4
#define MSG_ADA9 0xADA9
#define MSG_B0F8 0xB0F8
#define MSG_B02D 0xB02D
#define MSG_B099 0xB099
#define MSG_B074 0xB074
#define MSG_AFFC 0xAFFC
#define MSG_B068 0xB068
#define MSG_B018 0xB018
#define MSG_B0A6 0xB0A6
#define MSG_B047 0xB047
#define MSG_B04B 0xB04B
#define MSG_B052 0xB052
#define MSG_B05B 0xB05B
#define MSG_B05F 0xB05F
#define MSG_B061 0xB061

/* Routines. */
#define R_PRINT_MSG 0x72DD   /* print the message at HL */
#define R_CANNOT 0x72CE      /* "i cannot do that" */
#define R_PRINT_WORD 0x74C1  /* print the dictionary word DE */
#define R_NEWLINE 0x8583
#define R_TAKE 0x8D33        /* the TAKE action */
#define R_GO_8DAB 0x8DAB     /* inside the GO action ($8D9D) */
#define R_910E 0x910E        /* the OPEN action for doors */
#define R_9117 0x9117
#define R_9F4A 0x9F4A        /* run HL with the two objects swapped */
#define R_FAIL 0x9F76        /* the action is not possible */
#define R_964D 0x964D
#define R_LOCK_946D 0x946D
#define R_UNLOCK_948D 0x948D
#define R_KILL_OBJ1 0x977C   /* OBJ1 dies or breaks */
#define R_KILL_A 0x977F      /* object A dies or breaks */
#define R_97FF 0x97FF
#define R_LOCATE_OBJECT 0x9BCA /* IX = data of object A */
#define R_9BDD 0x9BDD        /* moves object A (and what is on it) */
#define R_9C78 0x9C78
#define R_RANDOM 0x9C9F      /* random number below A */
#define R_COUNT_ON 0x9D97    /* objects inside OBJ1 */
#define R_9F25 0x9F25
#define R_LOCATION_LIT 0x95ED /* carry if your location is dark */
#define R_EXITS_START 0xA0AE  /* IX = exits of location A, BC = 3 */
#define R_NEXT_EXIT 0xA124    /* next exit; Z at the end */
#define R_EXIT_WORD 0xA0BA    /* DE = word for direction A */
#define R_WORD_IN 0xA0BD      /* DE = word A (bit 7 ignored) of the table at HL */
#define R_SAY 0xA1E3          /* 'You say "' message HL '".' */

static uint16_t ix_at(const Cpu *c, int off) { return (uint16_t)(c->ix + off); }
static uint16_t iy_at(const Cpu *c, int off) { return (uint16_t)(c->iy + off); }

/* Jump to original code (a JP to a routine outside this file): it returns
 * to our caller. */
#define TAIL(addr) \
  do { \
    cpu_tail(c, addr); \
    return; \
  } while (0)

/* CALL $9D44 at site, done here: when only checking (DOING != 1), it sets
 * OK = DOING + 1, pops its return address into BC and returns from the
 * handler; returns false and the handler must return at once. Otherwise A
 * = 1 with the flags of CP 1, and the handler carries on. */
static bool check_only(Cpu *c, uint16_t site) {
  wr16(c, (uint16_t)(c->sp - 2), (uint16_t)(site + 3)); /* the CALL's return address, either way */
  c->a = c->mem[DOING];
  op_cp(c, 0x01);
  if (c->zf) return true;
  c->a = op_inc(c, c->a);
  c->mem[OK] = c->a;
  set_bc(c, (uint16_t)(site + 3));
  return false;
}

/* CALL target at site: the original CALL instruction is run
 * (cpu_call_at), so the stack gets the real return address, and a callee
 * that never comes back (a death restarts the game) abandons this routine
 * as it does the original. target is checked against the instruction. */
static void call_at(Cpu *c, uint16_t site, uint16_t target) {
  if (c->mem[site] != 0xCD) abort();
  if (rd16(c, (uint16_t)(site + 1)) != target) abort();
  cpu_call_at(c, site);
}
#define CALL(site, target) call_at(c, site, target)

/* CALL $97FF at site, done here: unless DOING and OK are both set, it
 * pops its return address into BC and returns from the handler (false).
 * Otherwise A = DOING AND OK, with its flags. */
static bool if_done(Cpu *c, uint16_t site) {
  wr16(c, (uint16_t)(c->sp - 2), (uint16_t)(site + 3)); /* the CALL's return address, either way */
  wr16(c, (uint16_t)(c->sp - 4), get_bc(c));            /* PUSH BC, either way */
  c->a = op_and(c, c->mem[DOING], c->mem[OK]);
  if (!c->zf) return true;
  set_bc(c, (uint16_t)(site + 3));
  return false;
}

/* $A138: DisplayExits. List the exits of location A ("visible exits
 * are: north east ...", a newline), if there are any. Keeps IX IY DE BC. */
static void display_exits(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  push16(c, get_de(c));
  push16(c, get_bc(c));
  CALL(0xA13E, R_EXITS_START);
  CALL(0xA141, R_NEXT_EXIT);
  if (c->zf) goto LA15D;
  set_hl(c, MSG_EXITS);
  CALL(0xA149, R_PRINT_MSG);
LA14C:
  c->a = c->mem[ix_at(c, 0)];
  CALL(0xA14F, R_EXIT_WORD);
  CALL(0xA152, R_PRINT_WORD);
  CALL(0xA155, R_NEXT_EXIT);
  if (!c->zf) goto LA14C;
  CALL(0xA15A, R_NEWLINE);
LA15D:
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  c->iy = pop16(c);
  c->ix = pop16(c);
}

/* $A172: "[object HL] is [word A]." with the word from the table at
 * $A224 (or $A234 if bit 7 of A is set). The message takes the object and
 * the word from the stack, under its return address. */
static void object_is(Cpu *c) {
  push16(c, get_de(c));
  push16(c, get_hl(c));
  set_hl(c, WORD_TABLE_A224);
  if (op_bit(c, 7, c->a)) set_hl(c, WORD_TABLE_A234);
  CALL(0xA17E, R_WORD_IN);
  set_hl(c, pop16(c));
  push16(c, get_de(c));
  push16(c, get_hl(c));
  set_hl(c, MSG_IS);
  CALL(0xA187, R_PRINT_MSG); /* takes the object and the word off the stack */
  set_de(c, pop16(c));
}

/* $A164: object_is for the object at IY. */
static void object_is_iy(Cpu *c) {
  c->l = c->mem[iy_at(c, 8)];
  c->h = c->mem[iy_at(c, 9)];
  object_is(c);
}

/* $A16C: object_is for the object at IX. */
static void object_is_ix(Cpu *c) {
  c->l = c->mem[ix_at(c, 8)];
  c->h = c->mem[ix_at(c, 9)];
  object_is(c);
}

/* $A18C: reset object A: clear its +$0C/+$0D, and set +$0A/+$0B to $00CD,
 * or $0192 for an animal. Keeps IX. */
static void reset_object(Cpu *c) {
  push16(c, c->ix);
  CALL(0xA18E, R_LOCATE_OBJECT);
  c->mem[ix_at(c, 0x0C)] = 0x00;
  c->mem[ix_at(c, 0x0D)] = 0x00;
  set_de(c, 0x00CD);
  if (op_bit(c, 6, c->mem[ix_at(c, 7)])) set_de(c, 0x0192);
  c->mem[ix_at(c, 0x0A)] = c->e;
  c->mem[ix_at(c, 0x0B)] = c->d;
  c->ix = pop16(c);
}

/* $A1C8: Z if the object at IX is an animal and not dead (bits 6 and 3 of
 * its attributes are 1 and 0). */
static void is_alive(Cpu *c) {
  c->a = op_and(c, c->mem[ix_at(c, 7)], 0x48);
  op_cp(c, 0x40);
}

/* $A1D0: Z if ACTION is one of the 5 at $A20B. Keeps HL BC. */
static void action_in_list(Cpu *c) {
  push16(c, get_hl(c));
  push16(c, get_bc(c));
  c->b = 0x05;
  uint16_t hl = ACTIONS_EXITS;
  c->a = c->mem[ACTION];
  for (;;) {
    op_cp(c, c->mem[hl]);
    if (c->zf) break;
    hl++;
    if (--c->b == 0) break;
  }
  set_hl(c, hl);
  set_bc(c, pop16(c));
  set_hl(c, pop16(c));
}

/* $A204: A = $85, NZ if the object at IX is open. */
static void open_word(Cpu *c) {
  op_bit(c, 5, c->mem[ix_at(c, 7)]);
  c->a = 0x85;
}

/* $A1F9: IX = OBJ1's data; A = $80, NZ if it is locked; otherwise as
 * $A204. */
static void locked_word(Cpu *c) {
  c->ix = rd16(c, OBJ1_PTR);
  bool locked = op_bit(c, 0, c->mem[ix_at(c, 7)]);
  c->a = 0x80;
  if (locked) return;
  open_word(c);
}

/* $A244: an action that does nothing (DRINK the water $17). */
static void do_nothing(Cpu *c) {
  if (!check_only(c, 0xA244)) return;
}

/* $A248: ActionTie. TIE [rope] TO [object], or TIE [object] TO [rope]
 * (run again with the objects swapped, through $9F4A). The rope's mother
 * becomes the object; the rope is then taken, if it can be, or else left
 * with the actor (or nowhere, $FF). */
static void action_tie(Cpu *c) {
  c->a = c->mem[OBJ1];
  op_cp(c, 0x12);
  if (c->zf) goto LA2AE;
  c->a = c->mem[OBJ2];
  op_cp(c, 0x12);
  if (!c->zf) TAIL(R_FAIL);
  c->ix = rd16(c, OBJ1_PTR);
  if (op_bit(c, 1, c->mem[ix_at(c, 7)])) TAIL(R_FAIL);
  CALL(0xA262, R_COUNT_ON);
  op_cp(c, 0x00);
  set_hl(c, MSG_ALREADY_TIED);
  if (!c->zf) TAIL(R_PRINT_MSG);
  if (op_bit(c, 3, c->mem[ix_at(c, 7)])) goto LA27A;
  if (op_bit(c, 6, c->mem[ix_at(c, 7)])) TAIL(R_FAIL);
LA27A:
  if (!check_only(c, 0xA27A)) return;
  c->iy = rd16(c, OBJ2_PTR);
  CALL(0xA281, R_9C78);
  c->a = c->mem[OBJ2];
  c->mem[ix_at(c, 1)] = c->a;
  if (c->cf) goto LA2A2;
  c->a = op_sub(c, c->a, c->a, 0);
  c->mem[DOING] = c->a;
  CALL(0xA290, R_TAKE);
  c->a = c->mem[OK];
  op_cp(c, 0x00);
  c->a = 0x01;
  c->mem[DOING] = c->a;
  c->mem[OK] = c->a;
  if (c->zf) goto LA2A9;
LA2A2:
  c->a = c->mem[ACTOR];
  c->mem[iy_at(c, 1)] = c->a;
  return;
LA2A9:
  c->mem[iy_at(c, 1)] = 0xFF;
  return;
LA2AE:
  set_hl(c, 0xA248);
  TAIL(R_9F4A);
}

/* $A2B4: ActionUntie. If OBJ1 is tied to the rope, it is put where the
 * rope is. */
static void action_untie(Cpu *c) {
  c->ix = rd16(c, OBJ1_PTR);
  c->a = c->mem[ix_at(c, 1)];
  op_cp(c, 0x12);
  set_hl(c, MSG_NOT_TIED);
  if (!c->zf) TAIL(R_PRINT_MSG);
  if (!check_only(c, 0xA2C3)) return;
  c->a = c->mem[ROPE_MOTHER];
  c->mem[ix_at(c, 1)] = c->a;
}

/* $A2CD: SWIM in the fast river ($2A): go where its exit leads, as GO
 * does, with OBJ1 marked open for the move. */
static void swim_river(Cpu *c) {
  cpu_call_at(c, 0xA2CD);
  c->a = c->mem[ix_at(c, 2)];
  op_cp(c, 0x00);
  if (!c->zf) goto LA2DB;
  check_only(c, 0xA2D7);
  return;
LA2DB:
  c->a = c->mem[ix_at(c, 0)];
  c->l = c->mem[ACTION];
  c->h = c->mem[OBJ1];
  push16(c, get_hl(c));
  c->mem[ACTION] = c->a;
  c->ix = rd16(c, OBJ1_PTR);
  c->mem[ix_at(c, 7)] |= 0x20;
  push16(c, c->ix);
  c->a = 0xFF;
  c->mem[OBJ1] = c->a;
  CALL(0xA2F4, R_GO_8DAB);
  c->ix = pop16(c);
  c->mem[ix_at(c, 7)] &= ~0x20;
  set_hl(c, pop16(c));
  c->mem[ACTION] = c->l;
  c->mem[OBJ1] = c->h;
}

/* $A302: ActionBurn. Only the dragon ($3C) can: OBJ1 dies. */
static void action_burn(Cpu *c) {
  c->a = c->mem[ACTOR];
  op_cp(c, 0x3C);
  if (!c->zf) TAIL(R_FAIL);
  if (!check_only(c, 0xA30A)) return;
  TAIL(R_KILL_OBJ1);
}

/* F as PUSH AF stores it (bits 3 and 5 are not kept). */
static uint8_t get_f(const Cpu *c) {
  return (uint8_t)(c->sf << 7 | c->zf << 6 | c->hf << 4 | c->pf << 2 | c->nf << 1 | c->cf);
}
static void set_f(Cpu *c, uint8_t f) {
  c->sf = f >> 7 & 1, c->zf = f >> 6 & 1, c->hf = f >> 4 & 1;
  c->pf = f >> 2 & 1, c->nf = f >> 1 & 1, c->cf = f & 1;
}

/* $A316: print HL; for you, also $ADA9; then the actor dies. */
static void drown(Cpu *c) {
  CALL(0xA316, R_PRINT_MSG);
  c->a = c->mem[ACTOR];
  push16(c, (uint16_t)(c->a << 8 | get_f(c)));
  c->a = op_and(c, c->a, c->a);
  set_hl(c, MSG_ADA9);
  cpu_call_at(c, 0xA321); /* CALL Z,$72DD */
  uint16_t af = pop16(c);
  c->a = af >> 8;
  set_f(c, (uint8_t)af);
  TAIL(R_KILL_A);
}

/* $A310: SWIM in the black river ($09): it kills. */
static void swim_black_river(Cpu *c) {
  if (!check_only(c, 0xA310)) return;
  set_hl(c, MSG_B0B4);
  drown(c);
}

/* $A328: DRINK the black water: it kills. */
static void drink_black_water(Cpu *c) {
  if (!check_only(c, 0xA328)) return;
  set_hl(c, MSG_B0F8);
  drown(c);
}

/* $A358: the message at $B02D (wrong key). */
static void wrong_key(Cpu *c) {
  set_hl(c, MSG_B02D);
  TAIL(R_PRINT_MSG);
}

/* $A33A: LOCK/UNLOCK a door with key B: with the right key, $946D (lock,
 * ACTION $25) or $948D (unlock); with another key, wrong_key; with
 * anything else "i cannot do that". */
static void lock_with(Cpu *c) {
  c->a = c->mem[OBJ2];
  op_cp(c, c->b);
  if (!c->zf) goto LA34B;
  c->a = c->mem[ACTION];
  op_cp(c, 0x25);
  if (c->zf) TAIL(R_LOCK_946D);
  TAIL(R_UNLOCK_948D);
LA34B:
  op_cp(c, 0x02);
  if (c->zf) goto LA358;
  op_cp(c, 0x04);
  if (c->zf) goto LA358;
  op_cp(c, 0x0F);
  if (!c->zf) TAIL(R_CANNOT);
LA358:
  wrong_key(c);
}

/* $A330: the side door's key ($02). */
static void lock_with_02(Cpu *c) {
  c->b = 0x02;
  lock_with(c);
}

/* $A334: the red door's key ($0F). */
static void lock_with_0f(Cpu *c) {
  c->b = 0x0F;
  lock_with(c);
}

/* $A338: the rock door's key ($04). */
static void lock_with_04(Cpu *c) {
  c->b = 0x04;
  lock_with(c);
}

/* $A35E: every turn for the side door ($0B). */
static void side_door(Cpu *c) {
  if (!if_done(c, 0xA35E)) return;
  c->ix = rd16(c, OBJ1_PTR);
  TAIL(R_9117);
}

/* $A368: OPEN the crack: only from location $0F. */
static void open_crack(Cpu *c) {
  c->ix = rd16(c, ACTOR_PTR);
  c->a = c->mem[ix_at(c, 0x10)];
  op_cp(c, 0x0F);
  if (!c->zf) TAIL(R_FAIL);
  TAIL(R_910E);
}

/* $A377: every turn for the web: once broken, open it and restart the
 * spider event's count. */
static void web(Cpu *c) {
  c->ix = rd16(c, OBJ1_PTR);
  if (!op_bit(c, 3, c->mem[ix_at(c, 7)])) return;
  c->mem[ix_at(c, 7)] |= 0x20;
  c->a = c->mem[VAR_CA8B];
  c->mem[VAR_CA8C] = c->a;
  set_hl(c, MSG_B099);
  TAIL(R_PRINT_MSG);
}

/* $A390: WEAR the ring: the actor becomes invisible and a quarter as
 * strong, the ring is worn (invisible, carried by the actor) for 2-9
 * turns. */
static void wear_ring(Cpu *c) {
  if (!check_only(c, 0xA390)) return;
  c->iy = rd16(c, ACTOR_PTR);
  c->ix = RING;
  c->mem[iy_at(c, 7)] &= ~0x80;
  c->mem[iy_at(c, 5)] = op_srl(c, c->mem[iy_at(c, 5)]);
  c->mem[iy_at(c, 5)] = op_srl(c, c->mem[iy_at(c, 5)]);
  c->mem[ix_at(c, 7)] &= ~0x80;
  c->a = c->mem[ACTOR];
  c->mem[ix_at(c, 1)] = c->a;
  c->a = 0x08;
  CALL(0xA3B3, R_RANDOM);
  c->a = op_add(c, c->a, 0x02, 0);
  c->mem[RING_TIMER] = c->a;
}

/* $A3BC: TAKE OFF the ring (also when its time runs out): undo
 * wear_ring. */
static void take_off_ring(Cpu *c) {
  c->ix = rd16(c, ACTOR_PTR);
  c->iy = RING;
  bool visible = op_bit(c, 7, c->mem[iy_at(c, 7)]);
  set_hl(c, MSG_B074);
  if (visible) TAIL(R_PRINT_MSG);
  if (!check_only(c, 0xA3CE)) return;
  c->mem[iy_at(c, 7)] |= 0x80;
  c->mem[ix_at(c, 7)] |= 0x80;
  c->mem[ix_at(c, 5)] = op_sla(c, c->mem[ix_at(c, 5)]);
  c->mem[ix_at(c, 5)] = op_sla(c, c->mem[ix_at(c, 5)]);
  c->a = op_sub(c, c->a, c->a, 0);
  c->mem[RING_TIMER] = c->a;
}

/* $A3E6: ActionCapture. An animal OBJ1 no bigger (?) than the actor is
 * put in the elvenking's dungeon ($1F) by the wood elf ($40) or the
 * butler ($42), or in the goblins' dungeon ($0D) by anyone else, unless
 * the actor is there already ($B6F6). When it is you, you are told where
 * you are. */
static void action_capture(Cpu *c) {
  c->ix = rd16(c, OBJ1_PTR);
  c->a = op_and(c, c->mem[ix_at(c, 4)], 0x70);
  push16(c, c->ix);
  c->ix = rd16(c, ACTOR_PTR);
  c->a = op_and(c, c->a, c->mem[ix_at(c, 4)]);
  c->ix = pop16(c);
  if (!c->zf) TAIL(R_FAIL);
  if (!op_bit(c, 6, c->mem[ix_at(c, 7)])) TAIL(R_FAIL);
  c->a = c->mem[ACTOR];
  c->b = 0x1F;
  op_cp(c, 0x40);
  if (c->zf) goto LA413;
  op_cp(c, 0x42);
  if (c->zf) goto LA413;
  c->b = 0x0D;
LA413:
  c->a = c->mem[VAR_B6F6];
  op_cp(c, c->b);
  if (c->zf) TAIL(R_FAIL);
  if (!check_only(c, 0xA41A)) return;
  c->ix = rd16(c, OBJ1_PTR);
  c->mem[ix_at(c, 0x10)] = c->b;
  c->mem[ix_at(c, 1)] = 0xFF;
  c->a = c->mem[OBJ1];
  CALL(0xA42B, R_9BDD);
  c->a = c->mem[OBJ1];
  op_cp(c, 0x00);
  if (!c->zf) return;
  c->mem[ACTOR] = c->a;
  set_hl(c, PLAYER);
  wr16(c, ACTOR_PTR, PLAYER);
  CALL(0xA43D, R_LOCATION_LIT);
  if (c->cf) return;
  c->a = c->b;
  set_hl(c, MSG_AFFC);
  TAIL(R_964D);
}

/* $A448: every turn for a goblin: once dead, it is cleared to come back
 * later: its entry in the table at $A49C gives where its id is stored, its
 * new location and +$0A/+$0B. If you are at location B, a message. (No end
 * to the table: the goblins are all in it.) */
static void goblin(Cpu *c) {
  c->ix = rd16(c, OBJ1_PTR);
  if (!op_bit(c, 3, c->mem[ix_at(c, 7)])) return;
  c->mem[ix_at(c, 7)] &= ~0x08;
  set_de(c, 0x0006);
  c->iy = ESCAPES;
  c->a = c->mem[OBJ1];
  for (;;) {
    op_cp(c, c->mem[iy_at(c, 0)]);
    if (c->zf) break;
    c->iy = op_add16(c, c->iy, get_de(c));
  }
  push16(c, c->iy);
  uint16_t hl = pop16(c);
  hl++;
  c->e = c->mem[hl];
  hl++;
  c->d = c->mem[hl];
  c->mem[get_de(c)] = c->a;
  hl++;
  c->a = c->mem[hl];
  c->mem[ix_at(c, 0x10)] = c->a;
  hl++;
  c->a = c->mem[hl];
  c->mem[ix_at(c, 0x0A)] = c->a;
  hl++;
  c->a = c->mem[hl];
  c->mem[ix_at(c, 0x0B)] = c->a;
  set_hl(c, MSG_B068);
  CALL(0xA482, R_PRINT_MSG);
  c->a = c->mem[PLAYER_LOC];
  op_cp(c, c->b);
  if (!c->zf) return;
  set_de(c, 0x005E);
  CALL(0xA48D, R_PRINT_WORD);
  set_de(c, 0x02E2);
  CALL(0xA493, R_PRINT_WORD);
  set_hl(c, MSG_B018);
  TAIL(R_PRINT_MSG);
}

/* $A4C0: OPEN the goblins' door: only from location $10; it stays open
 * for a while (the event at $CA99). */
static void open_goblin_door(Cpu *c) {
  c->ix = rd16(c, ACTOR_PTR);
  c->a = c->mem[ix_at(c, 0x10)];
  op_cp(c, 0x10);
  if (!c->zf) TAIL(R_FAIL);
  CALL(0xA4CC, R_910E);
  if (!if_done(c, 0xA4CF)) return;
  c->a = c->mem[VAR_CA99];
  c->mem[VAR_CA9A] = c->a;
}

/* $A4D9: the goblins' door closes (an event). */
static void close_goblin_door(Cpu *c) {
  set_hl(c, GOBLIN_DOOR_ATTR);
  c->mem[GOBLIN_DOOR_ATTR] &= ~0x20;
}

/* $A4DF, $A4F5, $A4FE, $A51C, $A525: characters' sayings. */
static void say_a4df(Cpu *c) {
  if (!check_only(c, 0xA4DF)) return;
  c->a = 0x0A;
  CALL(0xA4E4, R_RANDOM);
  set_hl(c, MSG_B0A6);
  op_cp(c, 0x08);
  if (!c->cf) TAIL(R_SAY);
  set_hl(c, MSG_B047);
  TAIL(R_SAY);
}

static void say_a4f5(Cpu *c) {
  if (!check_only(c, 0xA4F5)) return;
  set_hl(c, MSG_B04B);
  TAIL(R_SAY);
}

static void say_a4fe(Cpu *c) {
  if (!check_only(c, 0xA4FE)) return;
  c->a = 0x02;
  CALL(0xA503, R_RANDOM);
  set_hl(c, MSG_B052);
  op_cp(c, 0x00);
  if (c->zf) TAIL(R_SAY);
  set_hl(c, MSG_B05B);
  op_cp(c, 0x01);
  if (c->zf) TAIL(R_SAY);
  set_hl(c, MSG_B05F);
  TAIL(R_SAY);
}

static void say_a51c(Cpu *c) {
  if (!check_only(c, 0xA51C)) return;
  set_hl(c, MSG_B061);
  TAIL(R_SAY);
}

static void say_a525(Cpu *c) {
  c->a = c->mem[VAR_B6F6];
  set_hl(c, VAR_B6F5);
  op_cp(c, c->mem[VAR_B6F5]);
  if (!c->zf) return;
  if (!check_only(c, 0xA52D)) return;
  set_hl(c, MSG_B05F);
  TAIL(R_SAY);
}

#define DEFAULT (OUT_REGS | OUT_ZF | OUT_CF)

const PortRoutine actions3_routines[] = {
    {0xA138, "DisplayExits", display_exits, DEFAULT},
    {0xA164, "ObjectIsIY", object_is_iy, DEFAULT},
    {0xA16C, "ObjectIsIX", object_is_ix, DEFAULT},
    {0xA18C, "ResetObject", reset_object, DEFAULT},
    {0xA1C8, "IsAlive", is_alive, DEFAULT},
    {0xA1D0, "ActionInList", action_in_list, DEFAULT},
    {0xA1F9, "LockedWord", locked_word, DEFAULT},
    {0xA204, "OpenWord", open_word, DEFAULT},
    {0xA244, "DoNothing", do_nothing, DEFAULT},
    {0xA248, "ActionTie", action_tie, DEFAULT},
    {0xA2B4, "ActionUntie", action_untie, DEFAULT},
    {0xA2CD, "SwimRiver", swim_river, DEFAULT},
    {0xA302, "ActionBurn", action_burn, DEFAULT},
    {0xA310, "SwimBlackRiver", swim_black_river, DEFAULT},
    {0xA328, "DrinkBlackWater", drink_black_water, DEFAULT},
    {0xA330, "LockWith02", lock_with_02, DEFAULT},
    {0xA334, "LockWith0F", lock_with_0f, DEFAULT},
    {0xA338, "LockWith04", lock_with_04, DEFAULT},
    {0xA358, "WrongKey", wrong_key, DEFAULT},
    {0xA35E, "SideDoor", side_door, DEFAULT},
    {0xA368, "OpenCrack", open_crack, DEFAULT},
    {0xA377, "Web", web, DEFAULT},
    {0xA390, "WearRing", wear_ring, DEFAULT},
    {0xA3BC, "TakeOffRing", take_off_ring, DEFAULT},
    {0xA3E6, "ActionCapture", action_capture, DEFAULT},
    {0xA448, "Goblin", goblin, DEFAULT},
    {0xA4C0, "OpenGoblinDoor", open_goblin_door, DEFAULT},
    {0xA4D9, "CloseGoblinDoor", close_goblin_door, DEFAULT},
    {0xA4DF, "SayA4DF", say_a4df, DEFAULT},
    {0xA4F5, "SayA4F5", say_a4f5, DEFAULT},
    {0xA4FE, "SayA4FE", say_a4fe, DEFAULT},
    {0xA51C, "SayA51C", say_a51c, DEFAULT},
    {0xA525, "SayA525", say_a525, DEFAULT},
    {0, NULL, NULL, 0},
};
