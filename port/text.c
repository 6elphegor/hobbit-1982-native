/* Helpers and text output: tables, the keyboard with the idle timer,
 * messages and their tokens ($70E2-$7573, not the parser).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * Messages are strings of bytes read by PrintMsg ($72DD):
 *   $20-$5F  a character, printed as it is;
 *   $60-$7F  one of 32 common words (table at $AD3D);
 *   $80-     with the next byte, a dictionary token: the high nibble of
 *            the first byte (bit 7 off) says how it is printed (see
 *            PrintToken), and $2x, $3x, $6x end the message after it
 *            (nothing, '.' and a new line, a new line);
 *   $00-$1F  control codes, dispatched through the table at $7295: print
 *            the actor, an object, an argument taken from the stack, a
 *            sub-message, a relative jump, end the message...
 * A dictionary token is an offset from $6000 (12 bits); its high nibble
 * picks an article or an inflection ("s", "es", "ies", "d", "ing", from
 * the table at $B71F) when it is printed. Words are collected in the
 * output buffer ($74A6) and printed as a whole, after a space and, if
 * they do not fit, a new line.
 *
 * Some control codes take their argument from the stack, below
 * PrintMsg's return address (the caller pushes it). Their handlers pop
 * PrintMsg's return address, which leaves its frame, so the rest of them
 * (and of the message) are continuation entries ($736D, $7378, $7396,
 * $742F, $7430), reached with cpu_tail.
 *
 * Other code is reached with cpu_call_at at the original CALL, so the
 * stack holds the real return addresses (conditional CALLs too, with
 * the condition also tested in C). */
#include <stddef.h>

#include "addrs.h"
#include "cpu.h"
#include "routines.h"

/* Routines elsewhere. */
#define R_NEWLINE 0x8583       /* new line (A: B704, the capital flag, kept) */
#define R_GET_KEY 0x8B93       /* scan the keyboard: A = key or 0 */
#define R_CLEAR_LINE 0x6E8B    /* ClearLine: delete what was typed */
#define R_LOCATE_PLACE 0x9BB1  /* A = location ID -> IX = its data */
#define R_LOCATE_OBJECT 0x9BCA /* A = object ID -> IX = its data */
#define R_IS_DARK 0x95ED       /* carry set if the player cannot see */
#define R_PRINT_NOUN 0x9ED6    /* print the noun phrase at IY */

/* Scratch for PrintMsg: A, DE and IX on entry, restored at the end. */
#define SAVED_A 0x70DC
#define SAVED_DE 0x70DD
#define SAVED_IX 0x70E0
#define IN_ACTION 0x70DF /* set while ActionMsg prints: articles become "the" */

#define WAIT_TEXT 0x7291   /* "WAIT", typed by the idle timer */
#define PRINT_TABLE 0x7295 /* the control codes' handlers */
#define OUT_BUF 0x74A6     /* the word being printed (20 bytes) */
#define DICT_TOKENS 0x6000 /* tokens are offsets from here */
#define ACTION_INDEX 0xAB4B /* action records, 8 bytes, ID 1 at $AB53 */
#define ARTICLES 0xAD2D    /* 8 tokens: the a an some the the the some */
#define ARTICLES_ALT 0xAD35 /* the same, for the input line and actions */
#define COMMON_WORDS 0xAD3D /* 32 tokens (high byte - $50): a and are ... you */
#define MSG_CANNOT 0xAFBF  /* "i cannot do that" */
#define WORD_YOU 0x07A8    /* token "you" */

#define VAR_B6DF 0xB6DF   /* unknown: set by MatchPair when the first pair matches */
#define ACTION 0xB6E7     /* the action being done (ID) */
#define OBJECT1 0xB6E8    /* its first object ($FF: none) */
#define OBJECT2 0xB6E9    /* its second object ($FF: none) */
#define ACTOR 0xB6EA      /* who does it: 0 the player, $FF "someone" */
#define VAR_B6FB 0xB6FB   /* unknown: output state, with $B6FA (VAR_B6FA) */
#define TOKEN_ARG 0xB6FC  /* token printed by control code $03 */
#define OBJ1_PLACE 0xB6FE /* OBJECT1 is a location, not an object */
#define OBJ2_PLACE 0xB6FF /* OBJECT2 is a location */
#define ARTICLE 0xB703    /* print an article before the next noun */
#define CAPITAL 0xB704    /* start of a sentence (LAST_KEY in addrs.h) */
#define IN_QUOTES 0xB71B  /* inside "..." */
#define ACTION_FLAGS1 0xB71D /* from the action record: bytes 1 and 3 */
#define ACTION_FLAGS2 0xB71E /* bytes 5 and 7 */
#define INFLECTIONS 0xB71F /* 8 suffixes of 4 bytes: s es ies \bies d ing */
#define ROOM_INPUT 0x85B3  /* unknown: room left on the input line */
#define ROOM_WINDOW 0x869B /* unknown: room left on the text window's line */
#define MID_LINE 0x86A0    /* unknown: nonzero when not at the start of a line */

/* CALL to a routine of this file, with its return address on the stack. */
#define CALL(ret, fn) \
  do {                \
    push16(c, ret);   \
    fn(c);            \
    c->sp += 2;       \
  } while (0)

static uint16_t ix_at(const Cpu *c, int off) { return (uint16_t)(c->ix + off); }
static uint16_t iy_at(const Cpu *c, int off) { return (uint16_t)(c->iy + off); }
static void xor_a(Cpu *c) { c->a = op_xor(c, c->a, c->a); }
static void and_a(Cpu *c) { c->a = op_and(c, c->a, c->a); }
static void ex_de_hl(Cpu *c) {
  uint16_t t = get_de(c);
  set_de(c, get_hl(c));
  set_hl(c, t);
}

/* PUSH AF / POP AF, with all of F. */
static void push_af(Cpu *c) { cpu_push_af(c); }
static void pop_af(Cpu *c) { cpu_pop_af(c); }

/* ---------- small helpers ---------- */

/* $70E2 Blanker: zero B bytes from HL (B=0: 256). */
static void t_blanker(Cpu *c) { blank(c); }

/* $70E8 IndexAction: HL = $AB4B + 8*A, the record of action A. */
static void index_action(Cpu *c) {
  c->l = c->a;
  c->h = 0;
  uint16_t hl = get_hl(c);
  hl = op_add16(c, hl, hl);
  hl = op_add16(c, hl, hl);
  hl = op_add16(c, hl, hl);
  set_de(c, ACTION_INDEX);
  set_hl(c, op_add16(c, hl, get_de(c)));
}

/* $70F3: from the action record at IX, ACTION_FLAGS2 = high nibble of
 * byte 7 + high nibble of byte 5 moved down; ACTION_FLAGS1 the same from
 * bytes 3 and 1 (and in A). */
static void action_flags(Cpu *c) {
  c->a = c->mem[ix_at(c, 5)];
  op_rrca(c), op_rrca(c), op_rrca(c), op_rrca(c);
  c->a = op_and(c, c->a, 0x0F);
  c->c = c->a;
  c->a = op_and(c, c->mem[ix_at(c, 7)], 0xF0);
  c->a = op_add(c, c->a, c->c, 0);
  c->mem[ACTION_FLAGS2] = c->a;
  c->a = c->mem[ix_at(c, 1)];
  op_rrca(c), op_rrca(c), op_rrca(c), op_rrca(c);
  c->a = op_and(c, c->a, 0x0F);
  c->c = c->a;
  c->a = op_and(c, c->mem[ix_at(c, 3)], 0xF0);
  c->a = op_add(c, c->a, c->c, 0);
  c->mem[ACTION_FLAGS1] = c->a;
}

/* $71D5: HL = the data of location A, + 2 (its name). IX kept. */
static void locate_place(Cpu *c) {
  push16(c, c->ix);
  cpu_call_at(c, 0x71D7); /* CALL $9BB1 */
  push16(c, c->ix);
  set_hl(c, pop16(c));
  set_hl(c, get_hl(c) + 2);
  c->ix = pop16(c);
}

/* $71E2: HL = the data of object A, + 8 (its name). DE, IX kept. */
static void locate_object(Cpu *c) {
  push16(c, get_de(c));
  push16(c, c->ix);
  cpu_call_at(c, 0x71E5); /* CALL $9BCA */
  push16(c, c->ix);
  set_hl(c, pop16(c));
  set_de(c, 0x0008);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  c->ix = pop16(c);
  set_de(c, pop16(c));
}

/* $722E: compare the word at HL with the one at IY: Z if HL's is empty,
 * or if their dictionary offsets are equal (12 bits). Moves both on by 2. */
static void match_word(Cpu *c) {
  push16(c, get_hl(c));
  uint16_t hl = get_hl(c);
  c->a = c->mem[hl];
  hl++;
  c->a = op_or(c, c->a, c->mem[hl]);
  if (c->zf) goto L7241;
  c->a = c->mem[iy_at(c, 1)];
  c->a = op_xor(c, c->a, c->mem[hl]);
  c->a = op_and(c, c->a, 0x0F);
  if (!c->zf) goto L7241;
  hl--;
  c->a = c->mem[hl];
  op_cp(c, c->mem[c->iy]);
L7241:
  set_hl(c, pop16(c) + 2);
  c->iy += 2;
}

/* $71F3: does the phrase at IY match the action's words at HL (two pairs
 * of two words)? Z if so. A=0 or 1 on some paths; HL, DE, IY kept. */
static void match_pair(Cpu *c) {
  push16(c, get_de(c));
  push16(c, get_hl(c));
  push16(c, c->iy);
  CALL(0x71FA, match_word);
  if (!c->zf) goto L7229;
  c->a = 0x01;
  c->mem[VAR_B6DF] = c->a;
  CALL(0x7204, match_word);
  if (!c->zf) goto L720D;
  CALL(0x7209, match_word);
  c->a = 0x00;
  if (c->zf) goto L7229;
L720D:
  c->iy = pop16(c);
  set_hl(c, pop16(c));
  push16(c, get_hl(c));
  push16(c, c->iy);
  set_de(c, 0x0004);
  c->iy = op_add16(c, c->iy, get_de(c));
  set_hl(c, get_hl(c) + 2);
  CALL(0x721D, match_word);
  if (!c->zf) goto L7229;
  set_de(c, 0xFFFC);
  c->iy = op_add16(c, c->iy, get_de(c));
  CALL(0x7227, match_word);
  c->a = 0x01;
L7229:
  c->iy = pop16(c);
  set_hl(c, pop16(c));
  set_de(c, pop16(c));
}

/* $728B: compare HL with DE (C if HL < DE, Z if equal). */
static void compare_hl_de(Cpu *c) {
  c->a = op_sub(c, c->h, c->d, 0);
  if (!c->zf) return;
  c->a = op_sub(c, c->l, c->e, 0);
}

/* $7249: GetKey with the idle timer. The timer counts keyboard scans
 * down from IDLE_TIMER; if it runs out, the line typed so far is cleared
 * and "WAIT" typed into the buffer at HL, and ENTER returned (B=$7C,
 * HL after it). A key adds 500 to the timer, at most 3000. */
static void get_input_key(Cpu *c) {
  push16(c, get_hl(c));
  set_hl(c, rd16(c, IDLE_TIMER));
L724D:
  cpu_call_at(c, 0x724D); /* CALL $8B93 */
  and_a(c);
  if (!c->zf) goto L7273;
  set_hl(c, get_hl(c) - 1);
  c->a = op_or(c, c->h, c->l);
  if (!c->zf) goto L724D;
  set_hl(c, pop16(c));
  push16(c, get_hl(c));
  cpu_call_at(c, 0x725A); /* CALL $6E8B */
  set_de(c, WAIT_TEXT);
  c->b = 0x04;
  do {
    c->a = c->mem[get_de(c)];
    c->mem[get_hl(c)] = c->a;
    set_hl(c, get_hl(c) + 1);
    set_de(c, get_de(c) + 1);
    cpu_call_at(c, 0x7266); /* CALL $858B */
  } while (--c->b);
  ex_sp_hl(c);
  c->b = 0x7C;
  c->a = 0x0D;
  set_hl(c, 0xFE0C);
L7273:
  push_af(c);
  xor_a(c);
  set_de(c, 0x01F4);
  set_hl(c, op_adc16(c, get_hl(c), get_de(c)));
  set_de(c, 0x0BB8);
  if (c->cf) goto L7284;
  CALL(0x7282, compare_hl_de);
  if (c->cf) goto L7285;
L7284:
  ex_de_hl(c);
L7285:
  wr16(c, IDLE_TIMER, get_hl(c));
  pop_af(c);
  set_hl(c, pop16(c));
}

/* ---------- tokens ---------- */

/* $74C1 PrintToken: print dictionary token DE (12-bit offset from $6000;
 * 0 prints nothing). The high nibble of D: $5x as it is; $4x with the
 * entry's inflection; $1x with it if OBJECT1 is set, others if ACTOR is
 * (a verb agreeing with its subject); $7x starts a sentence after it.
 * The word goes to OUT_BUF, then is printed after a space (when not at
 * the start of a line) and a new line if it does not fit. HL, BC, DE
 * kept. */
static void print_token(Cpu *c) {
  c->a = op_and(c, c->d, 0x0F);
  c->a = op_or(c, c->a, c->e);
  if (c->zf) return;
  push16(c, get_hl(c));
  push16(c, get_bc(c));
  push16(c, get_de(c));
  c->c = c->d;
  c->a = op_and(c, c->d, 0x0F);
  c->d = c->a;
  set_hl(c, op_add16(c, DICT_TOKENS, get_de(c)));
  set_de(c, OUT_BUF);
  push16(c, get_hl(c));
  c->b = 0x00;
L74D8: /* decode the letters */
  c->a = op_and(c, c->mem[get_hl(c)], 0x1F);
  if (c->zf) goto L74F9;
  c->b = op_inc(c, c->b);
  c->a = op_add(c, c->a, 0x60, 0);
  c->mem[get_de(c)] = c->a;
  set_de(c, get_de(c) + 1);
  op_bit(c, 7, c->mem[get_hl(c)]);
  set_hl(c, get_hl(c) + 1);
  if (c->zf) goto L74D8;
  c->a = c->b; /* the entry runs to at least 3 letters */
  op_cp(c, 0x02);
  if (c->zf) goto L74D8;
  op_cp(c, 0x03);
  if (!c->zf) goto L74F9;
  c->a = c->mem[(uint16_t)(get_hl(c) - 2)];
  op_bit(c, 7, c->a);
  if (!c->zf) goto L74D8;
L74F9: /* inflection? */
  set_hl(c, pop16(c));
  c->a = op_and(c, c->c, 0xF0);
  op_cp(c, 0x50);
  if (c->zf) goto L7530;
  op_cp(c, 0x40);
  if (c->zf) goto L7512;
  op_cp(c, 0x10);
  {
    bool z = c->zf;
    c->a = c->mem[OBJECT1];
    if (!z) c->a = c->mem[ACTOR];
  }
  and_a(c);
  if (c->zf) goto L7530;
L7512: /* bit 7 of the second byte: the third's bits 5-7 pick a suffix */
  set_hl(c, get_hl(c) + 1);
  op_bit(c, 7, c->mem[get_hl(c)]);
  if (c->zf) goto L7530;
  set_hl(c, get_hl(c) + 1);
  c->a = op_and(c, c->mem[get_hl(c)], 0xE0);
  op_rrca(c), op_rrca(c), op_rrca(c);
  c->l = c->a;
  c->h = 0x00;
  set_bc(c, INFLECTIONS);
  set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
  c->b = 0x04;
  do {
    c->a = c->mem[get_hl(c)];
    and_a(c);
    if (c->zf) goto L7530;
    set_hl(c, get_hl(c) + 1);
    c->mem[get_de(c)] = c->a;
    set_de(c, get_de(c) + 1);
  } while (--c->b);
L7530: /* B = its length; a space, and a new line if it does not fit */
  set_hl(c, OUT_BUF);
  ex_de_hl(c);
  and_a(c);
  set_hl(c, op_sbc16(c, get_hl(c), get_de(c)));
  c->b = c->l;
  c->a = c->mem[PRINT_TO_INPUT];
  and_a(c);
  push_af(c);
  if (!c->zf) goto L7543;
  c->a = c->mem[MID_LINE];
  and_a(c);
L7543:
  c->a = 0x20;
  if (!c->zf) cpu_call_at(c, 0x7545); /* CALL NZ,$858B */
  pop_af(c);
  c->a = c->mem[ROOM_INPUT];
  if (!c->zf) goto L7551;
  c->a = c->mem[ROOM_WINDOW];
L7551:
  set_hl(c, CAPITAL);
  op_cp(c, c->b);
  if (!c->cf) goto L755C;
  c->a = c->mem[get_hl(c)];
  cpu_call_at(c, 0x7558); /* CALL $8583 */
  c->mem[get_hl(c)] = c->a;
L755C:
  set_de(c, pop16(c));
  c->a = op_and(c, c->d, 0xF0);
  op_cp(c, 0x70);
  c->a = 0x01;
  if (c->zf) c->mem[get_hl(c)] = c->a;
  set_hl(c, OUT_BUF);
  do {
    c->a = c->mem[get_hl(c)];
    cpu_call_at(c, 0x756B); /* CALL $858B */
    set_hl(c, get_hl(c) + 1);
  } while (--c->b);
  set_bc(c, pop16(c));
  set_hl(c, pop16(c));
}

/* $74BA PrintAction: print the token at HL (low 12 bits), HL += 2. */
static void print_action(Cpu *c) {
  c->e = c->mem[get_hl(c)];
  set_hl(c, get_hl(c) + 1);
  c->a = c->mem[get_hl(c)];
  set_hl(c, get_hl(c) + 1);
  c->a = op_and(c, c->a, 0x0F);
  c->d = c->a;
  print_token(c);
}

/* $7434: print the noun phrase at HL ($9ED6 with IY=HL). IY kept. */
static void print_noun(Cpu *c) {
  push16(c, c->iy);
  push16(c, get_hl(c));
  c->iy = pop16(c);
  cpu_call_at(c, 0x7439); /* CALL $9ED6 */
  c->iy = pop16(c);
}

/* $743F: before noun token DE: a proper noun (bit 7 of D) other than
 * "you" starts with a capital (CAPITAL=1); otherwise print the article
 * picked by bits 4-6 of D. E is left as PRINT_TO_INPUT on that path. */
static void print_article(Cpu *c) {
  op_bit(c, 7, c->d);
  if (c->zf) goto L7455;
  c->a = c->e;
  set_hl(c, WORD_YOU);
  op_cp(c, c->l);
  if (!c->zf) goto L744F;
  c->a = op_and(c, c->d, 0x0F);
  op_cp(c, c->h);
  if (c->zf) return;
L744F:
  c->a = 0x01;
  c->mem[CAPITAL] = c->a;
  return;
L7455:
  set_hl(c, ARTICLES);
  c->a = c->mem[PRINT_TO_INPUT];
  c->e = c->a;
  c->a = c->mem[IN_ACTION];
  c->a = op_or(c, c->a, c->e);
  if (!c->zf) set_hl(c, ARTICLES_ALT);
  c->a = c->d;
  op_rrca(c), op_rrca(c), op_rrca(c);
  c->a = op_and(c, c->a, 0x1E);
  push16(c, get_de(c));
  c->e = c->a;
  c->d = 0x00;
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  c->e = c->mem[get_hl(c)];
  set_hl(c, get_hl(c) + 1);
  c->d = c->mem[get_hl(c)];
  CALL(0x7476, print_token);
  set_de(c, pop16(c));
}

/* $7478: print noun token DE, after its article if ARTICLE is set. */
static void print_noun_token(Cpu *c) {
  push16(c, get_de(c));
  c->a = c->mem[ARTICLE];
  and_a(c);
  if (!c->zf) CALL(0x7480, print_article);
  set_de(c, pop16(c));
  c->a = op_and(c, c->d, 0x0F);
  c->d = c->a;
  print_token(c);
}

/* $73CE (Z: object, NZ: location) / $73D6: print the name of thing A
 * (with an article if ARTICLE is set). HL kept, A=0. */
static void print_thing(Cpu *c, bool place) {
  push16(c, get_hl(c));
  if (place)
    CALL(0x73D4, locate_place);
  else
    CALL(0x73DA, locate_object);
  CALL(0x73DD, print_noun);
  set_hl(c, pop16(c));
  xor_a(c);
}

/* $7488: print character A ($FF: "someone"). */
static void print_actor_a(Cpu *c) {
  op_cp(c, 0xFF);
  if (!c->zf) {
    print_thing(c, false);
    return;
  }
  set_de(c, 0x0AE3);
  print_token(c);
}

/* $73A7: print the actor. A=0. */
static void print_actor(Cpu *c) {
  c->a = c->mem[ACTOR];
  CALL(0x73AD, print_actor_a);
  xor_a(c);
}

/* $73B4: print the first object (location or object). */
static void print_object1(Cpu *c) {
  c->a = c->mem[OBJ1_PLACE];
  and_a(c);
  c->a = c->mem[OBJECT1];
  print_thing(c, !c->zf);
}

/* $73C7: print the second object. */
static void print_object2(Cpu *c) {
  c->a = c->mem[OBJ2_PLACE];
  and_a(c);
  c->a = c->mem[OBJECT2];
  print_thing(c, !c->zf);
}

/* ---------- messages ---------- */

/* $72C3: print A; after a new line ($0D), CAPITAL=0 (Z). */
static void print_char_cr(Cpu *c) {
  cpu_call_at(c, 0x72C3); /* CALL $858B */
  op_cp(c, 0x0D);
  if (!c->zf) return;
  xor_a(c);
  c->mem[CAPITAL] = c->a;
}

/* $73FC: DE = "his" if A (a character) is nonzero, else "your"; NZ. */
static void his_or_your(Cpu *c) {
  set_de(c, 0x0990);
  and_a(c);
  if (!c->zf) return;
  set_de(c, 0x0BEA);
  c->a = op_or(c, c->a, 0x01);
}

/* $740C/$7425/$742D: "is" (DE=$039B, NZ) after printing character A, or
 * "are" (DE=$0065) if A is 0; ARTICLE = article. The original keeps A and
 * F in AF' across setting ARTICLE (EX AF,AF' twice), which comes to the
 * same thing here: af is AF as it was at the first EX, pushed as it is.
 * AF' itself is left as it was; the original leaves A'=article in it,
 * but nothing reads AF' before writing it ($7F46, $820E, the ROM's
 * LD-BYTES). */
static void is_are(Cpu *c, uint8_t article, uint16_t af) {
  c->mem[ARTICLE] = article;
  push16(c, af);
  CALL(0x7419, print_actor_a);
  pop_af(c);
  and_a(c);
  set_de(c, 0x039B);
  if (!c->zf) return;
  set_de(c, 0x0065);
  c->a = op_or(c, c->a, 0x01);
}

static uint16_t af_word(const Cpu *c) {
  return (uint16_t)(c->a << 8 | cpu_f(c));
}

static void msg_loop(Cpu *c);

/* $73E0, control code $0B: print the sub-message at the relative offset
 * in the next byte, then carry on after that byte. */
static void sub_message(Cpu *c) {
  c->ix++;
  push16(c, c->ix);
  set_hl(c, pop16(c));
  push16(c, get_hl(c));
  c->e = c->mem[c->ix];
  c->d = 0x00;
  op_bit(c, 7, c->e);
  if (!c->zf) c->d = 0xFF;
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  push16(c, 0x73F5);
  msg_loop(c);
  if (c->returned) return;
  c->sp += 2;
  c->ix = pop16(c);
  xor_a(c);
}

/* $7326/$733F: run the handler of control code A (< $20) at HL, which
 * was read from PRINT_TABLE. Codes below $14 are CALLed (return 733B) and
 * return Z to carry on or NZ to print token DE; the others end the
 * message. Returns false if the original code took over. */
static bool control_code(Cpu *c, bool called) {
  uint16_t target = get_hl(c);
  if (!called) {
    switch (target) {
    case 0x7340: /* end with a new line */
    case 0x7344: /* end with '.' and a new line */
    case 0x735B: /* end */
      return true;
    default:
      cpu_tail(c, target);
      return false;
    }
  }
  push16(c, 0x733B);
  switch (target) {
  /* Codes taking an argument from the stack: the handler pops its return
   * address and PrintMsg's, which leaves PrintMsg's frame; the rest is a
   * continuation entry of its own. */
  case 0x7367: /* $00: print the noun phrase whose address is pushed */
    xor_a(c);
    c->mem[ARTICLE] = c->a;
    set_de(c, pop16(c));
    set_hl(c, pop16(c));
    cpu_tail(c, 0x736D);
    return false;
  case 0x7376: /* $01: print the pushed token */
    set_de(c, pop16(c));
    set_hl(c, pop16(c));
    cpu_tail(c, 0x7378);
    return false;
  case 0x7394: /* $04: print the pushed noun token, with an article */
    set_de(c, pop16(c));
    set_hl(c, pop16(c));
    cpu_tail(c, 0x7396);
    return false;
  case 0x742D: /* $13: "is"/"are" for the pushed object */
    set_de(c, pop16(c));
    set_hl(c, pop16(c));
    cpu_tail(c, 0x742F);
    return false;
  case 0x740C: /* $10: "is"/"are" for the actor */
    c->a = c->mem[ACTOR];
    is_are(c, 0, af_word(c));
    break;
  case 0x7425: /* $11: the same for the first object */
    c->a = c->mem[OBJECT1];
    is_are(c, 1, af_word(c));
    break;
  case 0x737E: /* $02: relative jump by the next byte */
    c->e = c->mem[ix_at(c, 1)];
    c->d = 0x00;
    op_bit(c, 7, c->e);
    if (!c->zf) c->d = 0xFF;
    c->ix = op_add16(c, c->ix, get_de(c));
    xor_a(c);
    break;
  case 0x738D: /* $03: the token in TOKEN_ARG */
    set_de(c, rd16(c, TOKEN_ARG));
    c->a = op_or(c, c->a, 0x01);
    break;
  case 0x738B: /* $05 $0A $0F $12: nothing */
    xor_a(c);
    break;
  case 0x73A3: /* $06: the actor */
    xor_a(c);
    c->mem[ARTICLE] = c->a;
    print_actor(c);
    break;
  case 0x73AF: /* $07: the first object, with its article */
    c->a = 0x01;
    c->mem[ARTICLE] = c->a;
    print_object1(c);
    break;
  case 0x73BD: /* $08: backspace */
    cpu_call_at(c, 0x73BD); /* CALL $858B */
    xor_a(c);
    break;
  case 0x73C2: /* $09: the second object, with its article */
    c->a = 0x01;
    c->mem[ARTICLE] = c->a;
    print_object2(c);
    break;
  case 0x73E0: /* $0B: a sub-message */
    sub_message(c);
    if (c->returned) return false;
    break;
  case 0x73F9: /* $0C: "his"/"your" for the actor */
    c->a = c->mem[ACTOR];
    his_or_your(c);
    break;
  case 0x72C3: /* $0D: new line */
    print_char_cr(c);
    break;
  case 0x7407: /* $0E: "his"/"your" for the first object */
    c->a = c->mem[OBJECT1];
    his_or_your(c);
    break;
  default:
    cpu_tail(c, target);
    return false;
  }
  c->sp += 2;
  return true;
}

/* $72F1: print the message at HL (the loop of PrintMsg), and restore A,
 * DE and IX from PrintMsg's scratch at its end. Leaves c->returned set
 * if the original code took over. */
static void msg_loop_at(Cpu *c, bool after_code);
static void msg_loop(Cpu *c) { msg_loop_at(c, false); }

/* after_code: carry on as at $733B, after a CALLed control code. */
static void msg_loop_at(Cpu *c, bool after_code) {
  if (after_code) goto L733B;
  push16(c, get_hl(c));
  c->ix = pop16(c);
L72F4:
  c->a = c->mem[c->ix];
  op_bit(c, 7, c->a);
  if (c->zf) goto L7318;
  /* a token */
  c->a = op_and(c, c->a, 0x7F);
  c->d = c->a;
  c->e = c->mem[ix_at(c, 1)];
  c->ix++;
  c->a = op_and(c, c->a, 0xF0);
  op_cp(c, 0x30);
  if (c->zf) goto L7348;
  op_cp(c, 0x20);
  if (c->zf) goto L7348;
  op_cp(c, 0x60);
  if (c->zf) goto L7348;
L7311:
  CALL(0x7314, print_token);
L7314:
  c->ix++;
  goto L72F4;
L7318:
  op_cp(c, 0x20);
  if (c->cf) goto L7326;
  op_cp(c, 0x60);
  if (!c->cf) goto L7493;
  CALL(0x7324, print_char_cr);
  goto L7314;
L7326: /* a control code */
  push16(c, get_de(c));
  c->e = c->a;
  c->d = 0x00;
  set_hl(c, PRINT_TABLE);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  c->e = c->mem[get_hl(c)];
  set_hl(c, get_hl(c) + 1);
  c->d = c->mem[get_hl(c)];
  ex_de_hl(c);
  set_de(c, pop16(c));
  op_cp(c, 0x14);
  if (!c->cf) {
    if (!control_code(c, false)) return;
    switch (get_hl(c)) {
    case 0x7340:
      c->d = 0x60;
      goto L734B;
    case 0x7344:
      c->d = 0x30;
      goto L734B;
    default:
      goto L735B;
    }
  }
  if (!control_code(c, true)) return;
L733B:
  if (c->zf) goto L7314;
  goto L7311;
L7348: /* the last token */
  CALL(0x734B, print_token);
L734B:
  c->a = 0x2E;
  op_bit(c, 6, c->d);
  if (!c->zf) goto L7358;
  op_bit(c, 4, c->d);
  if (!c->zf) cpu_call_at(c, 0x7353); /* CALL NZ,$858B */
  op_bit(c, 4, c->d);
L7358:
  if (!c->zf) cpu_call_at(c, 0x7358); /* CALL NZ,$8583 */
L735B:
  set_de(c, rd16(c, SAVED_DE));
  c->ix = rd16(c, SAVED_IX);
  c->a = c->mem[SAVED_A];
  return;
L7493: /* $7493 GetCommonWord */
  c->a = op_sub(c, c->a, 0x60, 0);
  c->e = c->a;
  c->d = 0x00;
  set_hl(c, COMMON_WORDS);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  c->e = c->mem[get_hl(c)];
  set_hl(c, get_hl(c) + 1);
  c->a = c->mem[get_hl(c)];
  c->a = op_add(c, c->a, 0x50, 0);
  c->d = c->a;
  goto L7311;
}

/* $72DD PrintMsg: print the message at HL. A, DE and IX are kept (in
 * SAVED_A..SAVED_IX, so a nested message restores the outer's). If
 * VAR_B6FA is 0, VAR_B6FB is cleared. */
static void print_msg(Cpu *c) {
  wr16(c, SAVED_DE, get_de(c));
  wr16(c, SAVED_IX, c->ix);
  c->mem[SAVED_A] = c->a;
  c->a = c->mem[VAR_B6FA];
  and_a(c);
  if (c->zf) c->mem[VAR_B6FB] = c->a;
  msg_loop(c);
}

/* $72CE: "i cannot do that". */
static void cannot_do_that(Cpu *c) {
  set_hl(c, MSG_CANNOT);
  print_msg(c);
}

/* $72D3: PrintMsg, with VAR_B6FA cleared first inside quotes. */
static void print_msg_quoted(Cpu *c) {
  c->a = c->mem[IN_QUOTES];
  and_a(c);
  if (!c->zf) {
    xor_a(c);
    c->mem[VAR_B6FA] = c->a;
  }
  print_msg(c);
}

/* ---------- describing an action ---------- */

/* $712B ActionMsg: describe the action being done (ACTION by ACTOR on
 * OBJECT1/OBJECT2) as a sentence, from the action record: the actor, the
 * verb (inflected), "cannot" if VAR_B6FB was 0, the objects with their
 * prepositions, and a full stop. In the dark the player sees only
 * "somewhere" for the lower actions. Not printed if flag bit 4 of the
 * record is set. Registers kept; A=0. */
static void action_msg(Cpu *c) {
  c->a = 0x01;
  c->mem[IN_ACTION] = c->a;
  xor_a(c);
  c->mem[ARTICLE] = c->a;
  push16(c, c->iy);
  push16(c, get_bc(c));
  c->a = c->mem[VAR_B6FB];
  c->b = c->a;
  and_a(c);
  c->a = 0x01;
  if (!c->zf) xor_a(c);
  c->mem[PRINT_TO_INPUT] = c->a;
  push16(c, c->ix);
  push16(c, get_hl(c));
  push16(c, get_de(c));
  c->a = c->mem[ACTION];
  CALL(0x714E, index_action);
  push16(c, get_hl(c));
  c->ix = pop16(c);
  xor_a(c);
  op_cp(c, c->b);
  if (c->zf) goto L715C;
  c->a = c->mem[ACTOR];
  and_a(c);
  if (c->zf) cpu_call_at(c, 0x7159); /* CALL Z,$8583 */
L715C:
  CALL(0x715F, action_flags);
  op_bit(c, 4, c->a);
  c->c = c->a;
  if (!c->zf) goto L71C9;
  CALL(0x7168, print_actor);
  set_de(c, 0x00EE); /* "cannot" */
  xor_a(c);
  op_cp(c, c->b);
  if (c->zf) CALL(0x7170, print_token);
  push16(c, get_hl(c));
  set_de(c, 0x0006);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  CALL(0x7178, print_action);
  cpu_call_at(c, 0x7178); /* CALL $95ED */
  set_hl(c, pop16(c));
  if (!c->cf) goto L718F;
  c->a = c->mem[ACTION];
  op_cp(c, 0x0B);
  if (!c->cf) goto L718F;
  set_de(c, 0x0AEA); /* "somewhere" */
  set_hl(c, get_hl(c) + 2);
  CALL(0x718D, print_token);
  goto L7192;
L718F:
  CALL(0x7192, print_action);
L7192:
  op_bit(c, 3, c->c);
  if (c->zf) goto L71A6;
  op_bit(c, 5, c->c);
  if (!c->zf) CALL(0x719B, print_action);
  c->a = c->mem[ACTION_FLAGS2];
  op_bit(c, 7, c->a);
  if (!c->zf) CALL(0x71A3, print_action);
  CALL(0x71A6, print_object1);
L71A6:
  c->a = c->mem[OBJECT2];
  op_cp(c, 0xFF);
  if (c->zf) goto L71C1;
  op_bit(c, 2, c->c);
  if (c->zf) goto L71C1;
  op_bit(c, 5, c->c);
  if (c->zf) CALL(0x71B6, print_action);
  c->a = c->mem[ACTION_FLAGS2];
  op_bit(c, 7, c->a);
  if (c->zf) CALL(0x71BE, print_action);
  CALL(0x71C1, print_object2);
L71C1:
  c->a = 0x2E;
  cpu_call_at(c, 0x71C3); /* CALL $858B */
  cpu_call_at(c, 0x71C6); /* CALL $8583 */
L71C9:
  xor_a(c);
  c->mem[IN_ACTION] = c->a;
  set_de(c, pop16(c));
  set_hl(c, pop16(c));
  c->ix = pop16(c);
  set_bc(c, pop16(c));
  c->iy = pop16(c);
}

/* $711A: describe the action (unless inside quotes), as said by the
 * game: VAR_B6FB=0, VAR_B6FA=1. A=0, Z. */
static void action_msg_said(Cpu *c) {
  xor_a(c);
  c->mem[VAR_B6FB] = c->a;
  c->a = op_inc(c, c->a);
  c->mem[VAR_B6FA] = c->a;
  c->a = c->mem[IN_QUOTES];
  and_a(c);
  if (c->zf) CALL(0x7129, action_msg);
  xor_a(c);
}

/* ---------- the stack-argument control codes, from where they leave
 * PrintMsg's frame (continuation entries: the top of the stack is the
 * argument, the caller's return address having been popped into HL). */

/* The handler's RET, to $733B in the message loop. */
static void handler_ret(Cpu *c) {
  uint16_t ret = pop16(c);
  if (ret == 0x733B)
    msg_loop_at(c, true);
  else
    cpu_tail(c, ret);
}

/* $736D (code $00): print the noun phrase at the argument. */
static void cont_noun(Cpu *c) {
  ex_sp_hl(c);
  push16(c, get_de(c));
  c->a = op_or(c, c->h, c->l);
  if (!c->zf) CALL(0x7374, print_noun);
  xor_a(c);
  handler_ret(c);
}

/* $7378 (code $01): print the argument as a token. */
static void cont_token(Cpu *c) {
  ex_sp_hl(c);
  push16(c, get_de(c));
  ex_de_hl(c);
  c->a = op_or(c, c->a, 0x01);
  handler_ret(c);
}

/* $7396 (code $04): print the argument as a noun token, with its article. */
static void cont_noun_token(Cpu *c) {
  ex_sp_hl(c);
  push16(c, get_de(c));
  ex_de_hl(c);
  c->a = 0x01;
  c->mem[ARTICLE] = c->a;
  CALL(0x73A1, print_noun_token);
  xor_a(c);
  handler_ret(c);
}

/* $742F (code $13): POP AF takes the argument, leaving the frame again. */
static void cont_is_are_pop(Cpu *c) {
  pop_af(c);
  cpu_tail(c, 0x7430);
}

/* $7430: "is"/"are" for character A (the argument's high byte). */
static void cont_is_are(Cpu *c) {
  uint16_t af = af_word(c);
  push16(c, get_hl(c));
  push16(c, get_de(c));
  is_are(c, 1, af);
  handler_ret(c);
}

#define DEFAULT_OUT (OUT_REGS | OUT_ZF | OUT_CF)

const PortRoutine text_routines[] = {
    {0x70E2, "Blanker", t_blanker, DEFAULT_OUT},
    {0x70E8, "IndexAction", index_action, DEFAULT_OUT},
    {0x70F3, "ActionFlags", action_flags, DEFAULT_OUT},
    {0x711A, "ActionMsgSaid", action_msg_said, DEFAULT_OUT},
    {0x712B, "ActionMsg", action_msg, DEFAULT_OUT},
    {0x71D5, "LocatePlace", locate_place, DEFAULT_OUT},
    {0x71E2, "LocateObject", locate_object, DEFAULT_OUT},
    {0x71F3, "MatchPair", match_pair, DEFAULT_OUT},
    {0x722E, "MatchWord", match_word, DEFAULT_OUT},
    {0x7249, "GetInputKey", get_input_key, DEFAULT_OUT},
    {0x728B, "CompareHLDE", compare_hl_de, DEFAULT_OUT},
    {0x72C3, "PrintCharCR", print_char_cr, DEFAULT_OUT},
    {0x72CE, "ICannotDoThat", cannot_do_that, DEFAULT_OUT},
    {0x72D3, "PrintMsgQ", print_msg_quoted, DEFAULT_OUT},
    {0x72DD, "PrintMsg", print_msg, DEFAULT_OUT},
    {0x72F1, "PrintMsgLoop", msg_loop, DEFAULT_OUT},
    {0x73A7, "PrintActor", print_actor, DEFAULT_OUT},
    {0x73B4, "PrintObject1", print_object1, DEFAULT_OUT},
    {0x73C7, "PrintObject2", print_object2, DEFAULT_OUT},
    {0x736D, "Msg00Cont", cont_noun, DEFAULT_OUT},
    {0x7378, "Msg01Cont", cont_token, DEFAULT_OUT},
    {0x7396, "Msg04Cont", cont_noun_token, DEFAULT_OUT},
    {0x742F, "Msg13Pop", cont_is_are_pop, DEFAULT_OUT},
    {0x7430, "Msg13Cont", cont_is_are, DEFAULT_OUT},
    {0x7434, "PrintNoun", print_noun, DEFAULT_OUT},
    {0x743F, "PrintArticle", print_article, DEFAULT_OUT},
    {0x7478, "PrintNounTok", print_noun_token, DEFAULT_OUT},
    {0x7488, "PrintActorA", print_actor_a, DEFAULT_OUT},
    {0x74BA, "PrintAction", print_action, DEFAULT_OUT},
    {0x74C1, "PrintToken", print_token, DEFAULT_OUT},
    {0, NULL, NULL, 0},
};
