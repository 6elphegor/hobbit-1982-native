/* Actions: examine; lighting; describing locations; the other
 * characters' turns ($93DA-$9A84).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * Calls. Every CALL is made with cpu_call_at on the original CALL
 * instruction, so the callee sees the real return address. Many callees
 * do not come back the normal way: $9D44 (and $8C9B, $8D25) pop their
 * return address to leave the caller as well when an action is only being
 * tried (DOING = 0); action handlers reached through $9B6C can kill the
 * player ($90D2 waits for a key and restarts the game). cpu_call_at then
 * abandons the routine, as the original was. A CALL at a routine's own
 * entry address cannot be run that way (the hook would run the routine
 * again): those use cpu_call, or are translated ($9D44, $8C9B, $8D25).
 *
 * Messages. PrintMsg ($72DD) takes the arguments of the codes $00, $01 and
 * $04 in a message from the stack, from under its return address
 * ($9771: two names for "[0x04] is carrying[0x04]").
 *
 * Labels keep the original addresses. */
#include <stddef.h>

#include "cpu.h"
#include "routines.h"

/* Variables. */
#define ACTION 0xB6E7     /* the action being carried out (see #ACTION in the disassembly) */
#define OBJ1 0xB6E8       /* its first object (direct), $FF none */
#define OBJ2 0xB6E9       /* its second object (indirect), $FF none */
#define ACTOR 0xB6EA      /* the character acting: $00 the player */
#define VAR_B6F0 0xB6F0   /* characters woken by a countdown this turn: at most one */
#define VAR_B6F1 0xB6F1   /* unknown: cleared at the start of the game */
#define VAR_B6F4 0xB6F4   /* the acting character can see the player: 1 */
#define PLAYER_LOC 0xB6F5 /* the player's location ($9B02) */
#define ACTOR_LOC 0xB6F6  /* the acting character's location */
#define VAR_B6F9 0xB6F9   /* unknown: cleared at the start of the game */
#define DOING 0xB6FA      /* 1: carry the action out; 0: only try it (see $9D44) */
#define DONE 0xB6FB       /* the action succeeded / was allowed */
#define OBJ1_FLAG 0xB6FE  /* unknown: 1 for a special first object (no object routine) */
#define OBJ2_FLAG 0xB6FF  /* the same for the second object */
#define QUIET 0xB702      /* 1: messages are not printed (another character, out of sight) */
#define VAR_B711 0xB711   /* unknown: nonzero skips the "can I see it" test of $950F */
#define OBJ1_PTR 0xB708   /* the first object's data */
#define OBJ2_PTR 0xB70A   /* the second object's data */
#define ACTOR_PTR 0xB70C  /* the acting character's data */
#define VAR_B6EE 0xB6EE   /* pointer into the table at $C7FC, chosen at random at the start */
#define LIT_STATE 0x980C  /* (in code space) 0 light, 1 dark, 2 dark and "you hear" already said */
#define ACTS 0x980B       /* (in code space) actions done by this character this turn, at most 6 */
#define OBJ1_LOC 0x980D   /* (in code space) location of the first object, $FF if many */
#define PICTURE_NONE 0x7F77 /* $FF: the last location had no picture drawn */
#define SM_948B 0x948B    /* operand of SET/RES 0,(IX+$07) at $9488, written at $9475 */
#define SM_A7D1 0xA7D1    /* operand of LD IY,nn at $A7CF, set at the start */
#define PLAYER 0xC11B     /* the player's object data */
#define SWORD 0xC305      /* the sword (glowing: lights dark places), object $0E */
#define SWORD_ATTR 0xC30C
#define PLAYER_FLAGS 0xC122 /* player attributes: bit 7 */
#define PLAYER_ALIVE 0xC12B /* nonzero while the player lives (the player's location) */
#define CHARACTERS 0xCACB /* the characters, 7 bytes each: id, countdown, script pointer,
                           * table of scripts; $FF ends */
#define COUNTDOWNS 0xCA84 /* timed events, 7 bytes: ?, countdown, routine at zero,
                           * threshold, routine below it; $FF ends */
#define ACTIONS 0xC730    /* action table: id, routine; $FF ends */
#define OBJECTS 0xC063
#define PREPOSITIONS 0xBA80 /* words for "in", "on", "at"..., by location bits 1-3 */

/* Messages. */
#define MSG_YOU_ARE_IN 0xAFFC /* "You are in[0x16]": the preposition is written at $AFFD */
#define MSG_YOU_SEE 0xB000    /* "You see[0x16]" */
#define MSG_SEE_NOTHING 0xAFC4 /* "i see nothing here" */
#define MSG_DARK 0xAE1F       /* "it is dark" */
#define MSG_SWEPT_AWAY 0xADFC /* "and it get(s) swept away" */
#define MSG_YOU_HEAR 0xB027   /* "you hear a noise": once a turn in the dark when others act */
#define MSG_CARRYING 0xAFE4   /* "[0x04] is carrying[0x04][0x15]" */

/* Routines (original, or someone else's). */
#define R_PRINT_MSG 0x72DD
#define R_CANNOT 0x72CE     /* "i cannot do that" */
#define R_NEWLINE 0x8583
#define R_DEAD 0x90D2       /* "You are dead", wait for a key, restart */
#define R_SAY_NO 0xA16C     /* "[object] ..." refusal, message A */
#define R_SAY_NO_IY 0xA164  /* the same, for the object at IY */
#define R_NOT_NOW 0x9F76    /* the action fails quietly (or prints the sentence) */
#define R_MOVE_OBJECT 0x9BDD
#define R_LIST_OBJECTS 0x9F94 /* "You see :" and the objects */

#define M(a) c->mem[(uint16_t)(a)]

/* A CALL at the routine's own entry address cannot be run as original
 * code (the hook would run this routine again): the callee (one that
 * always comes back) is run with cpu_call. `at` is the CALL's address. */
static void call_first(Cpu *c, uint16_t at, uint16_t target) {
  (void)target;
  cpu_call_at(c, at);
}

/* $9D44 at the start of a routine: when only trying the action (DOING
 * not 1), set DONE = DOING+1 and return from the routine, with BC = the
 * CALL's return address (POP BC). True if so. */
static bool only_trying(Cpu *c, uint16_t at) {
  wr16(c, (uint16_t)(c->sp - 2), (uint16_t)(at + 3));
  c->a = M(DOING);
  op_cp(c, 0x01);
  if (c->zf) return false;
  c->a = op_inc(c, c->a);
  M(DONE) = c->a;
  set_bc(c, (uint16_t)(at + 3));
  return true;
}


/* PUSH rr; POP rr': the value passes through the stack. */
static uint16_t via_stack(Cpu *c, uint16_t v) {
  wr16(c, (uint16_t)(c->sp - 2), v);
  return v;
}

/* PUSH AF / POP AF. The flags byte is approximate (bits 3 and 5 are not
 * kept), but it is only stack scratch; POP AF gives back what was saved. */
typedef struct {
  uint8_t a;
  bool sf, zf, hf, pf, nf, cf;
} SavedAF;
static SavedAF push_af(Cpu *c) {
  SavedAF s = {c->a, c->sf, c->zf, c->hf, c->pf, c->nf, c->cf};
  uint8_t f = (uint8_t)(c->sf << 7 | c->zf << 6 | c->hf << 4 | c->pf << 2 | c->nf << 1 | c->cf);
  push16(c, (uint16_t)(c->a << 8 | f));
  return s;
}
static void pop_af(Cpu *c, SavedAF s) {
  c->sp += 2;
  c->a = s.a, c->sf = s.sf, c->zf = s.zf, c->hf = s.hf, c->pf = s.pf, c->nf = s.nf, c->cf = s.cf;
}

static uint16_t ix_at(const Cpu *c, int d) { return (uint16_t)(c->ix + d); }
static uint16_t iy_at(const Cpu *c, int d) { return (uint16_t)(c->iy + d); }

/* ---------- object action routines ---------- */

/* $93DA Action_Examine: print the object's own description if it has
 * one, else "You see" and its name, and a full stop. */
static void p_examine(Cpu *c) {
  if (only_trying(c, 0x93DA)) return; /* $9D44 */
  c->a = M(OBJ1);
  cpu_call_at(c, 0x93E0); /* $9BCA: IX = the object */
  c->l = M(ix_at(c, 0x0E));
  c->h = M(ix_at(c, 0x0F));
  c->a = c->h;
  c->a = op_or(c, c->a, c->l);
  if (!c->zf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  set_hl(c, MSG_YOU_SEE);
  cpu_call_at(c, 0x93F1);
  c->iy = via_stack(c, c->ix);
  cpu_call_at(c, 0x93F8); /* $9EC7: the object's name */
  c->a = '.';
  cpu_call_at(c, 0x93FD);
  cpu_call_at(c, 0x9400);
}

/* $9404: object $13, action $22. Refused (message $05 from $A5CA) if the
 * first object's bit 5 is clear, or (message 2) if nothing is in it;
 * otherwise empties it ($9D50) and clears its bit 2. */
static void p_9404(Cpu *c) {
  c->ix = rd16(c, OBJ1_PTR);
  cpu_call_at(c, 0x9408); /* $A5CA */
  if (c->zf) {
    cpu_tail(c, R_SAY_NO);
    return;
  }
  c->a = M(OBJ1);
  cpu_call_at(c, 0x9411); /* $9D97: count the visible objects inside */
  op_cp(c, 0x00);
  if (c->zf) goto L9423;
  cpu_call_at(c, 0x9418); /* $9D44 */
  cpu_call_at(c, 0x941B); /* $9D50 */
  M(ix_at(c, 7)) &= ~0x04;
  return;
L9423:
  c->a = 0x02;
  cpu_tail(c, R_SAY_NO);
}

/* $9428: the river (objects $09, $2A), actions $11 (put in) and $0E: the
 * first object is swept away to the location after the actor's in the
 * river's list (or location 0 from the last), and the player dies if
 * that leaves him nowhere. */
static void p_river(Cpu *c) {
  /* $8D25: the second object ($9246) must have more than one location,
   * or bit 1 clear; else $9F76 and return from here (POP HL). */
  call_first(c, 0x9428, 0x9246);
  if (c->zf) {
    op_bit(c, 1, M(ix_at(c, 7)));
    if (c->zf) goto L942B;
  }
  set_hl(c, 0x942B);
  cpu_tail(c, R_NOT_NOW);
  return;
L942B:
  cpu_call_at(c, 0x942B); /* $9D44 */
  c->a = M(ACTOR_LOC);
  c->ix = rd16(c, OBJ2_PTR);
  c->b = M(c->ix);
L9438:
  op_cp(c, M(ix_at(c, 0x10)));
  if (c->zf) goto L9444;
  c->ix++;
  if (--c->b) goto L9438;
  cpu_tail(c, R_CANNOT);
  return;
L9444:
  c->a = M(ix_at(c, 0x11));
  c->b = op_dec(c, c->b);
  if (c->zf) c->a = op_xor(c, c->a, c->a);
  {
    SavedAF af = push_af(c);
    set_hl(c, MSG_SWEPT_AWAY);
    cpu_call_at(c, 0x944F);
    pop_af(c, af);
  }
  c->ix = rd16(c, OBJ1_PTR);
  M(ix_at(c, 0x10)) = c->a;
  M(ix_at(c, 0x01)) = 0xFF;
  c->b = c->a;
  c->a = M(OBJ1);
  cpu_call_at(c, 0x9462); /* $9BDD: move it there */
  c->a = M(PLAYER_ALIVE);
  c->a = op_and(c, c->a, c->a);
  if (c->zf) cpu_tail(c, R_DEAD);
}

/* $9475: write the bit operation at $948B (A = $C6 SET, $86 RES), check
 * the second object's bit 3 (refusal $83), and then do it to bit 0 of the
 * object at IX. */
static void set_or_res_bit0(Cpu *c) {
  M(SM_948B) = c->a;
  c->iy = rd16(c, OBJ2_PTR);
  op_bit(c, 3, M(iy_at(c, 7)));
  c->a = 0x83;
  if (!c->zf) {
    cpu_tail(c, R_SAY_NO_IY);
    return;
  }
  cpu_call_at(c, 0x9485); /* $9D44 */
  /* $9488: DD CB 07 xx, xx from $948B. Only $C6 (SET 0) and $86 (RES 0)
   * are ever written there; any SET/RES is done here. */
  {
    uint8_t op = M(SM_948B), bit = (uint8_t)(1 << ((op >> 3) & 7));
    if (op & 0x40)
      M(ix_at(c, 7)) |= bit;
    else
      M(ix_at(c, 7)) &= (uint8_t)~bit;
  }
}

/* $946D: set bit 0 of the first object (from $A33A, action $25). */
static void p_946d(Cpu *c) {
  call_first(c, 0x946D, 0xA1F9);
  if (!c->zf) {
    cpu_tail(c, R_SAY_NO);
    return;
  }
  c->a = 0xC6;
  set_or_res_bit0(c);
}

/* $948D: clear bit 0 of the first object (from $A33A), refused (message
 * 0) if it is clear. */
static void p_948d(Cpu *c) {
  c->ix = rd16(c, OBJ1_PTR);
  op_bit(c, 0, M(ix_at(c, 7)));
  c->a = 0x00;
  if (c->zf) {
    cpu_tail(c, R_SAY_NO);
    return;
  }
  cpu_call_at(c, 0x949A); /* $A204 */
  if (!c->zf) {
    cpu_tail(c, R_SAY_NO);
    return;
  }
  c->a = 0x86;
  set_or_res_bit0(c);
}

/* $94A4: action $2C on many objects: the first object must be carried
 * ($8C9B), and the second one found and with bit 5 set (else message 5);
 * then the first object goes to the second's location (IX+2 after $9F28),
 * out of anything. */
static void p_94a4(Cpu *c) {
  /* $8C9B: carrying the first object ($9C78)? Else "You are not
   * carrying it" and return from here (POP HL). */
  call_first(c, 0x94A4, 0x9C78);
  if (!c->cf) {
    set_hl(c, 0x94A7);
    set_hl(c, 0xADF1);
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  c->a = M(OBJ2);
  cpu_call_at(c, 0x94AA); /* $9F28 */
  op_cp(c, 0xFF);
  if (c->zf) {
    cpu_tail(c, R_NOT_NOW);
    return;
  }
  c->iy = rd16(c, OBJ2_PTR);
  op_bit(c, 5, M(iy_at(c, 7)));
  c->a = 0x05;
  if (c->zf) {
    cpu_tail(c, R_SAY_NO_IY);
    return;
  }
  cpu_call_at(c, 0x94BF); /* $9D44 */
  c->b = M(ix_at(c, 2));
  c->ix = rd16(c, OBJ1_PTR);
  M(ix_at(c, 0x01)) = 0xFF;
  M(ix_at(c, 0x10)) = c->b;
  c->a = M(OBJ1);
  cpu_tail(c, R_MOVE_OBJECT);
}

/* ---------- trying and doing actions ---------- */

/* $94D6: may the action be tried? DONE = 0 if $9B44 says no; 1 if it has
 * no routine in the action table, or is one of the five at $A20B, or the
 * object has no routine for it; else 0. Keeps IX and HL. */
static void p_94d6(Cpu *c) {
  push16(c, c->ix);
  push16(c, get_hl(c));
  cpu_call_at(c, 0x94D9); /* $9B44 */
  c->a = 0x00;
  if (c->zf) goto L9508;
  c->a = M(ACTION);
  c->ix = ACTIONS;
  cpu_call_at(c, 0x94E7); /* $9DBD */
  op_cp(c, 0xFF);
  c->a = 0x01;
  if (!c->zf) goto L9508;
  cpu_call_at(c, 0x94F0); /* $A1D0 */
  c->a = 0x01;
  if (c->zf) goto L9508;
  c->a = M(OBJ1);
  cpu_call_at(c, 0x94FA); /* $9BCA */
  c->a = M(ACTION);
  cpu_call_at(c, 0x9500); /* $9B81: the object's routine for the action */
  c->a = 0x01;
  if (c->cf) goto L9508;
  c->a = op_sub(c, c->a, c->a, 0);
L9508:
  M(DONE) = c->a;
  set_hl(c, pop16(c));
  c->ix = pop16(c);
}

/* $950F: carry out the action in ACTION/OBJ1/OBJ2 for ACTOR: check the
 * objects can be seen (or "i see nothing here"), say what the actor is
 * carrying ($9728), then run the objects' own routines for the action, or
 * the action table's. Afterwards, when doing it, say "it is dark" if it
 * is, and update light sources ($95DF). Keeps HL, IX and BC. */
static void p_do_action(Cpu *c) {
  push16(c, get_hl(c));
  push16(c, c->ix);
  push16(c, get_bc(c));
  cpu_call_at(c, 0x9513); /* $9B44 */
  if (c->zf) goto L95DA;
  cpu_call_at(c, 0x9519); /* $95ED: dark? */
  if (!c->cf) goto L9539;
  c->a = M(VAR_B711);
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) goto L9531;
  cpu_call_at(c, 0x9524); /* $9C78 */
  if (!c->cf) goto L9531;
  c->a = M(OBJ2);
  cpu_call_at(c, 0x952C); /* $9C7B */
  if (c->cf) goto L9539;
L9531:
  set_hl(c, MSG_SEE_NOTHING);
  cpu_call_at(c, 0x9534);
  goto L959B;
L9539:
  c->a = M(OBJ1_FLAG);
  op_cp(c, 0x01);
  if (c->zf) goto L95CC;
  c->a = M(OBJ1);
  op_cp(c, 0xFF);
  if (c->zf) goto L95CC;
  cpu_call_at(c, 0x9549); /* $9BCA */
  wr16(c, OBJ1_PTR, c->ix);
  c->a = M(OBJ1);
  cpu_call_at(c, 0x9553); /* $9728 */
  if (!c->zf) goto L959B;
  c->a = M(OBJ2);
  op_cp(c, 0xFF);
  if (c->zf) goto L957A;
  c->a = M(OBJ2_FLAG);
  op_cp(c, 0x01);
  if (c->zf) goto L95CC;
  c->a = M(OBJ2);
  cpu_call_at(c, 0x9569); /* $9BCA */
  wr16(c, OBJ2_PTR, c->ix);
  cpu_call_at(c, 0x9570); /* $9728 */
  if (!c->zf) goto L959B;
  cpu_call_at(c, 0x9575); /* $A1D0 */
  if (c->zf) goto L957E;
L957A:
  c->ix = rd16(c, OBJ1_PTR);
L957E:
  c->a = M(ACTION);
  cpu_call_at(c, 0x9581); /* $9B81 */
  if (!c->cf) goto L95CC;
L9586: /* run the routine, and those after it with action id 0 */
  c->l = M(ix_at(c, 1));
  c->h = M(ix_at(c, 2));
  cpu_call_at(c, 0x958C); /* $9B6C */
  c->ix += 3;
  c->a = op_sub(c, c->a, c->a, 0);
  op_cp(c, M(c->ix));
  if (c->zf) goto L9586;
L959B: /* CheckLit */
  c->a = M(DOING);
  op_cp(c, 0x01);
  if (!c->zf) goto L95C7;
  c->a = M(ACTOR);
  op_cp(c, 0x00);
  if (!c->zf) goto L95AF;
  cpu_call_at(c, 0x95A9); /* $95ED */
  cpu_call_at(c, 0x95AC); /* CALL C,$72DD: "it is dark" */
L95AF: /* CheckAction */
  c->a = M(ACTION);
  c->b = c->a;
  c->a = M(OBJ1);
  c->ix = rd16(c, OBJ1_PTR);
  cpu_call_at(c, 0x95BA); /* $95DF */
  c->a = M(OBJ2);
  c->ix = rd16(c, OBJ2_PTR);
  cpu_call_at(c, 0x95C4); /* $95DF */
L95C7: /* CheckReturn */
  set_bc(c, pop16(c));
  c->ix = pop16(c);
  set_hl(c, pop16(c));
  return;
L95CC: /* CheckValidAction */
  c->a = M(ACTION);
  c->ix = ACTIONS;
  cpu_call_at(c, 0x95D3); /* $9DBD */
  op_cp(c, 0xFF);
  if (!c->zf) goto L9586;
L95DA:
  cpu_call_at(c, 0x95DA); /* $72CE */
  goto L95C7;
}

/* $95DF: for a fluid object (bit 6 set) that gives no light (bit 3
 * clear), switch the owner character's script ($9AA0, with A and B). */
static void p_95df(Cpu *c) {
  op_bit(c, 6, M(ix_at(c, 7)));
  if (c->zf) return;
  op_bit(c, 3, M(ix_at(c, 7)));
  if (!c->zf) return;
  cpu_call_at(c, 0x95E9);
}

/* $95ED LocationLit: for the player (ACTOR 0), carry set (and HL = "it
 * is dark") if it is dark: the location is not lit and the sword is not
 * there glowing. Others: NZ, carry clear. */
static void p_location_lit(Cpu *c) {
  c->a = M(ACTOR);
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) return;
  push16(c, c->ix);
  push16(c, get_bc(c));
  c->ix = PLAYER;
  cpu_call_at(c, 0x95F9); /* $9E7A: what the player is inside */
  c->a = op_inc(c, c->a);
  if (!c->zf) goto L9608;
  cpu_call_at(c, 0x95FF); /* $9D37: IX = the location */
  op_bit(c, 7, M(c->ix));
  if (!c->zf) goto L9624;
L9608: /* LocationLit_Sword */
  push16(c, c->iy);
  c->a = 0x0E;
  c->iy = SWORD;
  cpu_call_at(c, 0x9610); /* $9E34: is it here? */
  c->iy = pop16(c);
  if (c->zf) goto L9620;
  c->a = M(SWORD_ATTR);
  c->a = op_xor(c, c->a, 0xF7);
  c->a = op_and(c, c->a, 0x1C);
  if (c->zf) goto L9628;
L9620: /* LocationLit_IsDark */
  set_hl(c, MSG_DARK);
  op_scf(c);
L9624:
  set_bc(c, pop16(c));
  c->ix = pop16(c);
  return;
L9628: /* LocationLit_IsLight */
  c->a = op_and(c, c->a, c->a);
  goto L9624;
}

/* ---------- describing a location ---------- */

/* $964D YouSeeWrapper: $965B keeping IX, IY and BC. */
static void you_see_wrapper(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  push16(c, get_bc(c));
  cpu_call_at(c, 0x9652); /* $965B */
  set_bc(c, pop16(c));
  c->iy = pop16(c);
  c->ix = pop16(c);
}

/* $962B YouSeeEntry: describe location A, starting "You see". */
static void p_you_see_entry(Cpu *c) {
  set_hl(c, MSG_YOU_SEE);
  you_see_wrapper(c);
}

/* $9630 YouSeePreposition: describe location A, starting "You are in"
 * with the location's own preposition. */
static void p_you_see_preposition(Cpu *c) {
  SavedAF af = push_af(c);
  cpu_call_at(c, 0x9631); /* $9BB1: IX = the location */
  c->a = M(c->ix);
  c->a = op_and(c, c->a, 0x0E);
  c->e = c->a;
  c->d = 0x00;
  set_hl(c, op_add16(c, PREPOSITIONS, get_de(c)));
  c->e = M(get_hl(c));
  set_hl(c, (uint16_t)(get_hl(c) + 1));
  c->d = M(get_hl(c));
  M(MSG_YOU_ARE_IN + 1) = c->d;
  M(MSG_YOU_ARE_IN + 2) = c->e;
  set_hl(c, MSG_YOU_ARE_IN + 2);
  pop_af(c, af);
  set_hl(c, MSG_YOU_ARE_IN);
  you_see_wrapper(c);
}

/* $964D */
static void p_you_see_wrapper(Cpu *c) { you_see_wrapper(c); }

/* $967F YouSeeExits: list the exits of location B, then the objects. */
static void you_see_exits(Cpu *c) {
  c->a = c->b;
  cpu_call_at(c, 0x9680); /* $A138 */
  cpu_tail(c, R_LIST_OBJECTS);
}

/* $965B YouSeeStart: print the message at HL, then the description of
 * location A, its picture (waiting for a key if one was drawn), the
 * characters there, the exits and the objects. */
static void p_you_see_start(Cpu *c) {
  c->b = c->a;
  cpu_call_at(c, 0x965C); /* $9BB1 */
  cpu_call_at(c, 0x965F); /* $72DD */
  c->l = M(ix_at(c, 8));
  c->h = M(ix_at(c, 9));
  c->a = c->h;
  c->a = op_or(c, c->a, c->l);
  cpu_call_at(c, 0x966A); /* $9686 */
  c->a = c->b;
  cpu_call_at(c, 0x966E); /* $7F78: the picture */
  c->a = M(PICTURE_NONE);
  c->a = op_inc(c, c->a);
  cpu_call_at(c, 0x9675); /* CALL NZ,$969A: wait for a key (left as original) */
  cpu_call_at(c, 0x9678); /* $8583 */
  c->a = c->b;
  cpu_call_at(c, 0x967C); /* $A0C8 */
  you_see_exits(c);
}

/* $9689 YouSeeNext: print the description words at IX+2 ($9ED6). */
static void you_see_next(Cpu *c) {
  set_de(c, 0x0002);
  push16(c, c->iy);
  c->iy = via_stack(c, c->ix);
  c->iy = op_add16(c, c->iy, get_de(c));
  cpu_call_at(c, 0x9694); /* $9ED6 */
  c->iy = pop16(c);
}

/* $9686 YouSeePrint: NZ: print the message at HL (the location's own
 * description); Z: its description words ($9689). */
static void p_you_see_print(Cpu *c) {
  if (!c->zf) {
    cpu_tail(c, R_PRINT_MSG);
    return;
  }
  you_see_next(c);
}

/* $9689 */
static void p_you_see_next(Cpu *c) { you_see_next(c); }

/* $96A8 YouSeeController: the short description of location A (B): its
 * words, the exits and the objects; no picture. */
static void p_you_see_controller(Cpu *c) {
  call_first(c, 0x96A8, 0x9BB1);
  cpu_call_at(c, 0x96AB); /* $9689 */
  cpu_call_at(c, 0x96AE); /* $8583 */
  you_see_exits(c);
}

/* ---------- the other characters ---------- */

/* $96B3: the end of a turn: $A9D6, the characters act ($980E), then the
 * timed events at $CA84 count down and run their routines (only one
 * reaching zero per turn; the others wait at 1). Keeps all registers. */
static void p_turn(Cpu *c) {
  push16(c, get_hl(c));
  push16(c, c->ix);
  push16(c, c->iy);
  push16(c, get_bc(c));
  push16(c, get_de(c));
  cpu_call_at(c, 0x96BA); /* $A9D6 */
  cpu_call_at(c, 0x96BD); /* $980E */
  c->a = op_sub(c, c->a, c->a, 0);
  M(VAR_B6F0) = c->a;
  c->a = op_inc(c, c->a);
  M(DONE) = c->a;
  M(DOING) = c->a;
  c->iy = COUNTDOWNS;
L96CF:
  c->a = M(c->iy);
  op_cp(c, 0xFF);
  if (c->zf) goto L971B;
  c->a = M(iy_at(c, 1));
  op_cp(c, 0x00);
  if (c->zf) goto L9713;
  c->a = op_dec(c, c->a);
  M(iy_at(c, 1)) = c->a;
  op_cp(c, 0x00);
  if (!c->zf) goto L96FE;
  c->a = M(VAR_B6F0);
  op_cp(c, 0x01);
  M(iy_at(c, 1)) = c->a;
  if (c->zf) goto L96FE;
  c->a = op_inc(c, c->a);
  M(VAR_B6F0) = c->a;
  c->l = M(iy_at(c, 2));
  c->h = M(iy_at(c, 3));
  cpu_call_at(c, 0x96F9); /* $9B6C */
  goto L9713;
L96FE:
  c->a = M(iy_at(c, 4));
  op_cp(c, 0x00);
  if (c->zf) goto L9713;
  op_cp(c, M(iy_at(c, 1)));
  if (c->cf) goto L9713;
  c->l = M(iy_at(c, 5));
  c->h = M(iy_at(c, 6));
  cpu_call_at(c, 0x9710); /* $9B6C */
L9713:
  set_de(c, 0x0007);
  c->iy = op_add16(c, c->iy, get_de(c));
  goto L96CF;
L971B:
  c->a = 0x01;
  M(QUIET) = c->a;
  set_de(c, pop16(c));
  set_bc(c, pop16(c));
  c->iy = pop16(c);
  c->ix = pop16(c);
  set_hl(c, pop16(c));
}

/* $9728: if object A is carried by a visible character other than the
 * player, and the player is ... (bit 7 of $C122), say "[character] is
 * carrying [object]" and return NZ. Else Z (or the flags of the test
 * that failed). Only for the player (ACTOR 0). */
static void p_9728(Cpu *c) {
  op_cp(c, 0xFF);
  if (c->zf) return;
  push16(c, c->ix);
  push16(c, c->iy);
  push16(c, get_bc(c));
  c->b = c->a;
  c->a = M(ACTOR);
  op_cp(c, 0x00);
  if (c->zf) goto L973B;
  c->a = op_xor(c, c->a, c->a);
  goto L9776;
L973B:
  c->a = c->b;
  cpu_call_at(c, 0x973C); /* $9BCA */
  c->a = M(ix_at(c, 1));
  op_cp(c, 0xFF);
  if (c->zf) goto L9776;
  c->a = c->b;
  c->iy = via_stack(c, c->ix);
  cpu_call_at(c, 0x974B); /* $9C7B */
  if (c->cf) goto L9776;
  cpu_call_at(c, 0x9750); /* $9BCA */
  op_bit(c, 6, M(ix_at(c, 7)));
  if (c->zf) goto L9776;
  c->a = M(PLAYER_FLAGS);
  op_bit(c, 7, c->a);
  if (c->zf) goto L9776;
  c->l = M(iy_at(c, 8));
  c->h = M(iy_at(c, 9));
  push16(c, get_hl(c));
  c->l = M(ix_at(c, 8));
  c->h = M(ix_at(c, 9));
  push16(c, get_hl(c));
  set_hl(c, MSG_CARRYING);
  cpu_call_at(c, 0x9771); /* $72DD: takes both names off the stack */
  c->a = op_or(c, c->a, 0x01);
L9776:
  set_bc(c, pop16(c));
  c->iy = pop16(c);
  c->ix = pop16(c);
}

/* $977F: the character or object A is killed (or destroyed): A = 0 is the
 * player ($90D2). Otherwise it is marked (bit 3), what it holds is
 * dropped ($9D53), it stops acting (its entry at $CACB cleared), and it
 * is reset ($A18C, $7F60). */
static void kill(Cpu *c) {
  c->a = op_and(c, c->a, c->a);
  if (c->zf) {
    cpu_tail(c, R_DEAD);
    return;
  }
  push16(c, get_bc(c));
  push16(c, c->iy);
  push16(c, c->ix);
  c->c = c->a;
  cpu_call_at(c, 0x9789); /* $9BCA */
  M(ix_at(c, 7)) |= 0x08;
  cpu_call_at(c, 0x9790); /* $9D53 */
  c->a = c->c;
  cpu_call_at(c, 0x9794); /* $9A85: IY = its entry at $CACB */
  op_cp(c, 0xFF);
  c->a = c->c;
  if (!c->zf) M(c->iy) = 0x00;
  cpu_call_at(c, 0x97A0); /* $A18C */
  c->a = c->c;
  cpu_call_at(c, 0x97A4); /* $7F60 */
  c->ix = pop16(c);
  c->iy = pop16(c);
  set_bc(c, pop16(c));
}

/* $977C: the first object is killed. */
static void p_977c(Cpu *c) {
  c->a = M(OBJ1);
  kill(c);
}

/* $977F */
static void p_kill(Cpu *c) { kill(c); }

/* $97AD: new game: the player acts, at a random one (1-4 or more) of the
 * entries at $C80E; three bytes cleared where it points; and a random
 * entry of the table at $C7FC. */
static void p_97ad(Cpu *c) {
  c->a = op_sub(c, c->a, c->a, 0);
  M(ACTOR) = c->a;
  M(VAR_B6F1) = c->a;
  M(VAR_B6F9) = c->a;
  set_hl(c, PLAYER);
  wr16(c, ACTOR_PTR, get_hl(c));
  c->a = 0x04;
  cpu_call_at(c, 0x97BF); /* $9C9F: random */
  c->a = op_inc(c, c->a);
  c->b = c->a;
  c->iy = 0xC808;
  set_de(c, 0x0006);
  do c->iy = op_add16(c, c->iy, get_de(c));
  while (--c->b);
  wr16(c, SM_A7D1, c->iy);
  c->l = M(iy_at(c, 1));
  c->h = M(iy_at(c, 2));
  c->b = 0x03;
  do {
    M(get_hl(c)) = 0x00;
    set_hl(c, (uint16_t)(get_hl(c) + 1));
  } while (--c->b);
  c->a = 0x03;
  cpu_call_at(c, 0x97E2); /* $9C9F */
  c->e = c->a;
  c->d = 0x00;
  c->e = op_sla(c, c->e);
  c->e = op_sla(c, c->e);
  set_hl(c, op_add16(c, 0xC7FC, get_de(c)));
  wr16(c, VAR_B6EE, get_hl(c));
}

/* $97F4: print the message at HL, a full stop and a newline. */
static void p_say(Cpu *c) {
  call_first(c, 0x97F4, R_PRINT_MSG);
  c->a = '.';
  cpu_call_at(c, 0x97F9);
  cpu_tail(c, R_NEWLINE);
}

/* $97FF: carry on only if both DOING and DONE are set; otherwise return
 * from the caller as well (with BC = the caller's return address). */
static void p_97ff(Cpu *c) {
  push16(c, get_bc(c));
  c->c = M(DOING);
  c->b = M(DONE);
  c->a = c->c;
  c->a = op_and(c, c->a, c->b);
  if (!c->zf) {
    set_bc(c, pop16(c));
    return;
  }
  set_bc(c, pop16(c));
  set_bc(c, pop16(c));
}

/* $9918: advance a character's script pointer (IY+2) to HL+DE, skipping
 * two more bytes if bit 4 of the command at IX is set. */
static void script_skip(Cpu *c) {
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  op_bit(c, 4, M(c->ix));
  if (!c->zf) set_hl(c, (uint16_t)(get_hl(c) + 2));
  M(iy_at(c, 2)) = c->l;
  M(iy_at(c, 3)) = c->h;
}

static void p_script_skip(Cpu *c) { script_skip(c); }

/* $99CE-$9A54 (the part of $99C6 after the parse): the acting character
 * does ACTION: unless it acts on an object the player cannot see, the
 * sentence is printed ($712B); then it is done ($950F), and where the
 * actor and the first object went is said ($9ACD). Ends NZ. */
static void character_acts(Cpu *c) {
  c->a = M(OBJ1_FLAG);
  op_cp(c, 0x01);
  if (c->zf) goto L9A32;
  c->a = M(ACTION);
  op_cp(c, 0x1E);
  if (!c->zf) goto L99E5;
  c->a = M(ACTOR_LOC);
  set_hl(c, PLAYER_ALIVE);
  op_cp(c, M(get_hl(c)));
  if (!c->zf) goto L9A32;
L99E5:
  c->a = M(OBJ1);
  op_cp(c, 0xFF);
  if (c->zf) goto L9A2B;
  cpu_call_at(c, 0x99EC); /* $9F82: its location, $FF if in many */
  M(OBJ1_LOC) = c->a;
  op_cp(c, 0xFF);
  if (!c->zf) goto L9A2B;
  c->c = M(PLAYER_LOC);
  c->b = M(ACTOR_LOC);
  c->a = c->c;
  op_cp(c, c->b);
  if (c->zf) goto L9A2B;
  c->b = M(c->ix);
L9A01:
  op_cp(c, M(ix_at(c, 0x10)));
  if (c->zf) goto L9A0C;
  c->ix++;
  if (--c->b) goto L9A01;
  goto L9A2B;
L9A0C: /* the object is where the player is: say it as "someone" */
  c->a = M(ACTOR);
  c->b = c->a;
  c->a = 0xFF;
  M(ACTOR) = c->a;
  c->a = 0x01;
  M(QUIET) = c->a;
  push16(c, c->iy);
  cpu_call_at(c, 0x9A1C); /* $712B */
  c->iy = pop16(c);
  c->a = op_sub(c, c->a, c->a, 0);
  M(QUIET) = c->a;
  c->a = c->b;
  M(ACTOR) = c->a;
  goto L9A32;
L9A2B:
  push16(c, c->iy);
  cpu_call_at(c, 0x9A2D); /* $712B */
  c->iy = pop16(c);
L9A32:
  cpu_call_at(c, 0x9A32); /* $950F */
  c->a = M(ACTOR);
  set_hl(c, ACTOR_LOC);
  set_de(c, 0xB017);
  cpu_call_at(c, 0x9A3E); /* $9ACD */
  c->a = M(OBJ1_FLAG);
  op_cp(c, 0x01);
  if (c->zf) goto L9A54;
  c->a = M(OBJ1);
  set_hl(c, OBJ1_LOC);
  set_de(c, 0xB01C);
  cpu_call_at(c, 0x9A51); /* $9ACD */
L9A54:
  c->a = op_or(c, c->a, 0x01);
}

/* $99C6: the acting character tries ACTION ($7AF5, which returns Z if it
 * cannot be done); if it can, does it (above). Keeps IX. */
static void p_99c6(Cpu *c) {
  push16(c, c->ix);
  cpu_call_at(c, 0x99C8); /* $7AF5 */
  if (c->zf) goto L9A56;
  character_acts(c);
L9A56:
  c->ix = pop16(c);
}

/* $9A68: point the script (IY+2) at entry E (at most IY+1) of the
 * character's script table at IY+4 (3 bytes each). */
static void script_choose(Cpu *c) {
  c->a = M(iy_at(c, 1));
  op_cp(c, c->e);
  if (c->cf) c->e = c->a;
  c->l = M(iy_at(c, 4));
  c->h = M(iy_at(c, 5));
  c->d = 0x00;
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  set_hl(c, (uint16_t)(get_hl(c) + 1));
  c->e = M(get_hl(c));
  set_hl(c, (uint16_t)(get_hl(c) + 1));
  c->d = M(get_hl(c));
  M(iy_at(c, 2)) = c->e;
  M(iy_at(c, 3)) = c->d;
}

/* $9A59: choose a random script: entry 0 to the smaller of IX+1 and IY+1. */
static void p_script_random(Cpu *c) {
  c->a = M(ix_at(c, 1));
  op_cp(c, M(iy_at(c, 1)));
  if (!c->cf) c->a = M(iy_at(c, 1));
  cpu_call_at(c, 0x9A64); /* $9C9F: random */
  c->e = c->a;
  script_choose(c);
}

static void p_script_choose(Cpu *c) { script_choose(c); }

/* $980E: each character in the table at $CACB takes its turn, running
 * its script: commands of 4 bytes (or 2 for type 4; bit 4: a jump address
 * follows): type in the low nibble, 0-3 an action (bit 0: a routine to
 * call instead), 4 a single action, $C, $E, $F script changes. A
 * character does at most 6 actions a turn. If the player is in the dark,
 * the first character there says $B027 once.
 *
 * $9B16, which the loop jumps to for a character inside something, is
 * part of this loop (it jumps back to $9870 or $9901) and is translated
 * here. */
static void p_characters(Cpu *c) {
  call_first(c, 0x980E, 0x9B02); /* PLAYER_LOC, LIT_STATE */
  c->iy = CHARACTERS;
L9815:
  c->a = op_xor(c, c->a, c->a);
  M(ACTS) = c->a;
  c->a = M(c->iy);
  op_cp(c, 0xFF);
  if (c->zf) goto L9909;
  op_cp(c, 0x00);
  if (c->zf) goto L9901;
  M(ACTOR) = c->a;
  cpu_call_at(c, 0x9829); /* $9F82: IX = the character, A = its location */
  wr16(c, ACTOR_PTR, c->ix);
  M(ACTOR_LOC) = c->a;
  c->a = op_sub(c, c->a, c->a, 0);
  M(QUIET) = c->a;
  c->a = M(c->iy);
  push16(c, c->iy);
  c->iy = PLAYER;
  cpu_call_at(c, 0x9840); /* $9E25: can the player see it? */
  c->iy = pop16(c);
  if (c->zf) goto L9868;
  c->a = M(LIT_STATE);
  op_cp(c, 0x02);
  if (c->zf) goto L9868;
  c->a = 0x01;
  M(QUIET) = c->a;
  c->a = M(LIT_STATE);
  op_cp(c, 0x01);
  if (!c->zf) goto L9868;
  c->a = op_inc(c, c->a);
  M(LIT_STATE) = c->a;
  set_hl(c, MSG_YOU_HEAR);
  cpu_call_at(c, 0x9861);
  c->a = op_sub(c, c->a, c->a, 0);
  M(QUIET) = c->a;
L9868:
  c->a = 0xFF;
  op_cp(c, M(ix_at(c, 1)));
  if (!c->zf) goto L9B16;
L9870:
  c->ix = rd16(c, ACTOR_PTR);
  cpu_call_at(c, 0x9874); /* $7F10 */
  c->a = 0x00;
  if (!c->zf) goto L987C;
  c->a = op_inc(c, c->a);
L987C:
  M(VAR_B6F4) = c->a;
L987F:
  c->l = M(iy_at(c, 2));
  c->h = M(iy_at(c, 3));
L9885:
  c->a = M(ACTS);
  op_cp(c, 0x06);
  if (c->zf) goto L9901;
  c->a = M(get_hl(c));
  set_de(c, 0x0004);
  c->ix = via_stack(c, get_hl(c));
  c->a = op_and(c, c->a, 0x0F);
  op_cp(c, 0x05);
  if (!c->cf) goto L98CB;
  c->a = M(VAR_B6F4);
  op_cp(c, 0x01);
  if (!c->zf) goto L98BF;
  op_bit(c, 6, M(get_hl(c)));
  if (!c->zf) goto L98BF;
  c->a = op_sub(c, c->a, c->a, 0);
  M(VAR_B6F4) = c->a;
  c->a = op_inc(c, c->a);
  cpu_call_at(c, 0x98A9); /* $7F1A */
  if (c->zf) goto L98BF;
  c->a = 0x01;
  M(DOING) = c->a;
  M(DONE) = c->a;
  /* PUSH $9901; PUSH IX; JP $99CE: $99C6 past its parse, returning to
   * $9901. */
  set_hl(c, 0x9901);
  push16(c, get_hl(c));
  push16(c, c->ix);
  character_acts(c);
  c->ix = pop16(c);
  pop16(c);
  goto L9901;
L98BF:
  c->a = M(get_hl(c));
  c->a = op_and(c, c->a, 0x0F);
  op_cp(c, 0x04);
  if (c->zf) goto L9974;
  if (c->cf) goto L9928;
  goto L9901;
L98CB:
  op_cp(c, 0x0E); /* jump */
  if (!c->zf) goto L98DD;
  c->e = M(ix_at(c, 1));
  M(iy_at(c, 2)) = c->e;
  c->e = M(ix_at(c, 2));
  M(iy_at(c, 3)) = c->e;
  goto L987F;
L98DD:
  op_cp(c, 0x0C);
  if (!c->zf) goto L98EC;
  c->b = M(ix_at(c, 1));
  c->a = M(c->iy);
  cpu_call_at(c, 0x98E7); /* $9AA0 */
  goto L987F;
L98EC:
  op_cp(c, 0x0F);
  if (!c->zf) goto L98F5;
  cpu_call_at(c, 0x98F0); /* $9A59 */
  goto L987F;
L98F5: /* (A is 5 or more here: never taken) */
  op_cp(c, 0x00);
  if (!c->zf) goto L98FC;
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  goto L9885;
L98FC:
  c->a = op_sub(c, c->a, c->a, 0);
  c->e = c->a;
  cpu_call_at(c, 0x98FE); /* $9A68 */
L9901:
  set_de(c, 0x0007);
  c->iy = op_add16(c, c->iy, get_de(c));
  goto L9815;
L9909:
  c->a = op_sub(c, c->a, c->a, 0);
  M(ACTOR) = c->a;
  c->a = op_inc(c, c->a);
  M(QUIET) = c->a;
  set_hl(c, PLAYER);
  wr16(c, ACTOR_PTR, get_hl(c));
  return;
L9928: /* types 0-3: an action */
  cpu_call_at(c, 0x9928); /* $9918 */
  op_bit(c, 0, M(c->ix));
  if (!c->zf) goto L994A;
  c->a = M(ix_at(c, 1));
  M(ACTION) = c->a;
  c->a = M(ix_at(c, 2));
  M(OBJ1) = c->a;
  c->a = M(ix_at(c, 3));
  M(OBJ2) = c->a;
  cpu_call_at(c, 0x9943); /* $99C6 */
  if (c->zf) goto L99AA;
  goto L9967;
L994A: /* a routine: tried, then done if it may be */
  c->l = M(ix_at(c, 1));
  c->h = M(ix_at(c, 2));
  c->a = op_sub(c, c->a, c->a, 0);
  M(DOING) = c->a;
  M(DONE) = c->a;
  cpu_call_at(c, 0x9957); /* $9B6C */
  c->a = M(DONE);
  op_cp(c, 0x01);
  if (!c->zf) goto L99AA;
  M(DOING) = c->a;
  cpu_call_at(c, 0x9964); /* $9B6C */
L9967: /* bit 5: once only */
  op_bit(c, 5, M(c->ix));
  if (c->zf) goto L9901;
  M(c->ix) = 0x00;
  goto L9901;
L9974: /* type 4: an action with no objects */
  set_de(c, 0x0002);
  cpu_call_at(c, 0x9977); /* $9918 */
  c->a = M(ix_at(c, 1));
  op_cp(c, 0xFF);
  if (c->zf) goto L9994;
  M(ACTION) = c->a;
  c->a = 0xFF;
  M(OBJ1) = c->a;
  M(OBJ2) = c->a;
  cpu_call_at(c, 0x998C); /* $99C6 */
  if (c->zf) goto L99AA;
  goto L9901;
L9994:
  op_bit(c, 4, M(c->ix));
  if (c->zf) goto L9901;
  c->l = M(ix_at(c, 2));
  c->h = M(ix_at(c, 3));
  M(iy_at(c, 2)) = c->l;
  M(iy_at(c, 3)) = c->h;
  goto L9901;
L99AA: /* not done: count it, and take the jump if there is one */
  set_hl(c, ACTS);
  M(get_hl(c)) = op_inc(c, M(get_hl(c)));
  op_bit(c, 4, M(c->ix));
  if (c->zf) goto L987F;
  c->ix = op_add16(c, c->ix, get_de(c));
  c->h = M(ix_at(c, 1));
  c->l = M(c->ix);
  M(iy_at(c, 2)) = c->l;
  M(iy_at(c, 3)) = c->h;
  goto L9885;
L9B16: /* the character is inside something */
  c->a = 0xFF;
  M(OBJ2) = c->a;
  c->a = M(ix_at(c, 1));
  M(OBJ1) = c->a;
  cpu_call_at(c, 0x9B21); /* $9BCA */
  op_bit(c, 3, M(ix_at(c, 7)));
  if (!c->zf) goto L9870;
  op_bit(c, 6, M(ix_at(c, 7)));
  if (!c->zf) goto L9870;
  op_bit(c, 5, M(ix_at(c, 7)));
  if (c->zf) goto L9901;
  c->a = 0x37; /* climb out of */
  M(ACTION) = c->a;
  cpu_call_at(c, 0x9B3E); /* $99C6 */
  goto L9901;
}

const PortRoutine actions2_routines[] = {
    {0x93DA, "Examine", p_examine, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9404, "Act9404", p_9404, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9428, "River", p_river, OUT_REGS | OUT_ZF | OUT_CF},
    {0x946D, "SetBit0", p_946d, OUT_REGS | OUT_ZF | OUT_CF},
    {0x948D, "ResBit0", p_948d, OUT_REGS | OUT_ZF | OUT_CF},
    {0x94A4, "Act94A4", p_94a4, OUT_REGS | OUT_ZF | OUT_CF},
    {0x94D6, "MayTry", p_94d6, OUT_REGS | OUT_ZF | OUT_CF},
    {0x950F, "DoAction", p_do_action, OUT_REGS | OUT_ZF | OUT_CF},
    {0x95DF, "Fluid95DF", p_95df, OUT_REGS | OUT_ZF | OUT_CF},
    {0x95ED, "LocationLit", p_location_lit, OUT_REGS | OUT_ZF | OUT_CF},
    {0x962B, "YouSeeEntry", p_you_see_entry, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9630, "YouSeePrep", p_you_see_preposition, OUT_REGS | OUT_ZF | OUT_CF},
    {0x964D, "YouSeeWrap", p_you_see_wrapper, OUT_REGS | OUT_ZF | OUT_CF},
    {0x965B, "YouSeeStart", p_you_see_start, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9686, "YouSeePrint", p_you_see_print, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9689, "YouSeeNext", p_you_see_next, OUT_REGS | OUT_ZF | OUT_CF},
    {0x96A8, "YouSeeCtrl", p_you_see_controller, OUT_REGS | OUT_ZF | OUT_CF},
    {0x96B3, "Turn", p_turn, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9728, "Carrying", p_9728, OUT_REGS | OUT_ZF | OUT_CF},
    {0x977C, "KillObj1", p_977c, OUT_REGS | OUT_ZF | OUT_CF},
    {0x977F, "Kill", p_kill, OUT_REGS | OUT_ZF | OUT_CF},
    {0x97AD, "NewGame97AD", p_97ad, OUT_REGS | OUT_ZF | OUT_CF},
    {0x97F4, "Say", p_say, OUT_REGS | OUT_ZF | OUT_CF},
    {0x97FF, "IfDone", p_97ff, OUT_REGS | OUT_ZF | OUT_CF},
    {0x980E, "Characters", p_characters, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9918, "ScriptSkip", p_script_skip, OUT_REGS | OUT_ZF | OUT_CF},
    {0x99C6, "CharAct", p_99c6, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9A59, "ScriptRandom", p_script_random, OUT_REGS | OUT_ZF | OUT_CF},
    {0x9A68, "ScriptChoose", p_script_choose, OUT_REGS | OUT_ZF | OUT_CF},
    {0, NULL, NULL, 0},
};
