/* The executor: carrying out parsed sentences ($7960-$7F77).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * After the parser ($7585) has built its noun-phrase records (at $B9C8,
 * going down, $18 bytes each, PHRASE_COUNT of them), Execute ($7960) takes
 * them one sentence at a time: RunSentence ($79B6) finds the verb in the
 * action table ($AB53, 8 bytes an entry), works out which objects the noun
 * phrases mean (with the object matchers at $9DD9/$9EA0, reached through
 * the JP (IY) at $7CC9), and either carries the action out (through the
 * game code at $950F, $712B, $96B3) or asks "which ...?" and keeps the
 * record at $B9C8 with VAR_B71A set, so that the next line is read as the
 * answer.
 *
 * Stack quirks of the original, kept here:
 * - RunSentence can leave its caller's frame: $7C16 (POP HL; POP HL;
 *   JP $798B: after "I don't understand" outside a quoted command), $7DC7
 *   (POP HL; JP $7994: next object of an ALL) and $7DF5 (JP $798E with
 *   the return address still on the stack, which a later RET in Execute
 *   then returns to: $7973). Execute calls the C version directly and
 *   follows these; the registered entry hands them to the original.
 * - Messages printed through $72D3 take parameters pushed on the stack and
 *   remove them (print_msg below).
 * - $7E78/$7E7C return with two words pushed (parameters for a message).
 *
 * The alternate AF (EX AF,AF' at $7F46/$7F4F) is not part of Cpu: after
 * $7F1A it is not set as the original sets it. No code in the game reads
 * AF' before writing it. */
#include <stddef.h>

#include "addrs.h"
#include "cpu.h"
#include "routines.h"

#define M(a) c->mem[(uint16_t)(a)]

/* Routines elsewhere (original code, or other files). */
#define R_INDEX_ACTION 0x70E8 /* HL = $AB4B + 8*A: the action table entry for action A */
#define R_ACTION_FLAGS 0x70F3 /* ACTION_FLAGS1/2 from the action entry at IX */
#define R_RUN_ACTION 0x711A   /* tail of the game turn: clears $B6FB, runs $712B */
#define R_TURN 0x712B
#define R_OBJ_NAME_CHAR 0x71D5 /* HL = the name record of character A */
#define R_OBJ_NAME 0x71E2     /* HL = the name record of object A */
#define R_MATCH_VERB 0x71F3   /* Z if the action entry at IY has the verb at HL */
#define R_PRINT_MSG_ARGS 0x72D3 /* print message HL, with parameters on the stack */
#define R_CHECK_ACTION 0x94D6
#define R_DO_ACTION 0x950F    /* sets $B6FB */
#define R_ACTORS 0x96B3       /* the other characters' turns */
#define R_MATCH_OBJECT 0x9DD9 /* next object in the list at IX matching the phrase at HL */
#define R_MATCH_CHAR 0x9EA0   /* the same for characters */
#define R_VAR_9E95 0x9E95

/* The noun-phrase records built by the parser. */
#define RECORDS 0xB9C8      /* the first record; the rest go down by $18 */
#define PHRASE_COUNT 0xB706 /* records left */
#define VAR_B705 0xB705     /* unknown: more sentences on the line */

/* Game state used here. */
#define IN_QUOTES 0xB71B    /* parsing, or carrying out, a quoted command */
#define ALL_MODE 0xB71C     /* repeating the action for each object of ALL */
#define ACTION_FLAGS1 0xB71D /* from the action table entry */
#define ACTION_FLAGS2 0xB71E
#define ACTION 0xB6E7       /* the action, 1 based */
#define ACTION_B6E6 0xB6E6  /* a copy of it */
#define OBJ1 0xB6E8         /* the object of the action ($FF none) */
#define OBJ2 0xB6E9         /* the second object */
#define ACTOR 0xB6EA        /* the character acting */
#define OBJ1_CHAR 0xB6FE    /* 1: OBJ1 is a character */
#define OBJ2_CHAR 0xB6FF
#define ACTION_DONE 0xB6FB  /* set by $950F */
#define IT_PHRASE 0xB6E0    /* 6 bytes: the phrase IT refers to */
#define VAR_B6DF 0xB6DF     /* unknown: set by $71F3 when a verb matched but not its form */
#define VAR_B70F 0xB70F
#define VAR_B710 0xB710     /* unknown: object matcher mode */
#define VAR_B711 0xB711
#define SPEECH 0xB738       /* 8 slots of $19 bytes: commands said to characters */
#define SPEECH_COUNT 0xB737
#define OBJECTS 0xC060      /* object list */
#define OBJECTS_C063 0xC063 /* ... from its 4th byte */
#define ACTIONS 0xAB53      /* action table: 8 bytes an entry, ending with a zero word */

/* The executor's own variables, $793D-$795F. */
#define N1_FLAGS 0x793D   /* bit 0: the first noun phrase was given; bit 1: matched */
#define N2_FLAGS 0x793E   /* the same for the second */
#define N1_TRIES 0x793F   /* objects tried for the first */
#define N1_COUNT 0x7940   /* objects that fit the first noun phrase */
#define N2_COUNT 0x7941
#define N1_PHRASE 0x7942  /* 6 bytes: the first noun phrase (adjectives, noun) */
#define N2_PHRASE 0x7948
#define N1_LIST 0x794E    /* where in the object list the first search is */
#define N2_LIST 0x7950
#define ALL_FLAG 0x7952   /* bit 7 of the verb's record: ALL */
#define VAR_7953 0x7953   /* 1: one object is enough */
#define N1_FIELD 0x7954   /* the record offset ($08/$12) of the first noun phrase */
#define N2_FIELD 0x7955
#define N1_LAST 0x7956    /* the last object that fitted */
#define N2_LAST 0x7957
#define VERB 0x7958       /* the verb (dictionary offset) */
#define PREP1 0x795A      /* two words from the record (prepositions) */
#define PREP2 0x795C
#define ACTION_ENTRY 0x795E /* the action table entry found */

/* Messages. */
#define MSG_DONT_UNDERSTAND 0xADA3 /* with the verb */
#define MSG_CANT_VERB 0xADB0       /* with the verb and its two words */
#define MSG_WHICH 0xADCC
#define MSG_CANT_SEE 0xADD3
#define MSG_ADE1 0xADE1
#define MSG_ADC4 0xADC4
#define MSG_ADE7 0xADE7
#define MSG_ADC0 0xADC0

/* Ways the original leaves RunSentence other than by returning. */
enum { OK, ESC_7C16, ESC_7DC7, ESC_798E };

static void xor_a(Cpu *c) { c->a = op_xor(c, c->a, c->a); }
static void and_a(Cpu *c) { c->a = op_and(c, c->a, c->a); }

/* The CALL addr whose return address is ret (the CALL is at ret-3), run
 * as the original's own instruction (cpu_call_at), so that the stack is
 * as the original has it. A callee may pop parameters pushed before the
 * call ($72D3 does); if it abandons the frame (dying restarts the game),
 * this routine is abandoned with it. addr is for the reader. */
static void call(Cpu *c, uint16_t addr, uint16_t ret) {
  (void)addr;
  cpu_call_at(c, (uint16_t)(ret - 3));
}

/* $72D3 with return address ret: print message HL, whose parameter codes
 * pop the words pushed above the return address. */
static void print_msg(Cpu *c, uint16_t ret) { call(c, R_PRINT_MSG_ARGS, ret); }

/* $7D90 (and the RET after it): print message HL with its parameters. */
static int print_and_return(Cpu *c) {
  cpu_call_at(c, 0x7D90); /* 0x7D6B */
  print_msg(c, 0x7D96);
  xor_a(c);
  return OK;
}

/* ---------- small routines ---------- */

/* $79A9: clear the executor's variables $793D-$794D. */
static void p_clear_vars(Cpu *c) {
  xor_a(c);
  M(PRINT_TO_INPUT) = c->a;
  set_hl(c, N1_FLAGS);
  c->b = 0x11;
  cpu_call_at(c, 0x79B2); /* Blanker */
}

/* $7ACC: IX = the object list: from $C060 if A&3, else $C063. */
static void p_object_list(Cpu *c) {
  c->ix = OBJECTS_C063;
  c->a = op_and(c, c->a, 0x03);
  if (c->zf) return;
  c->ix = OBJECTS;
}

/* $7AA6: start the search for the first object. */
static void start_search1(Cpu *c) {
  c->a = M(ACTION_FLAGS2);
  op_rrca(c);
  op_rrca(c);
  cpu_call_at(c, 0x7AAB); /* 0x7ACC */
  c->a = M(OBJ1_CHAR);
  and_a(c);
  if (!c->zf) cpu_call_at(c, 0x7AB2); /* R_VAR_9E95 */
  wr16(c, N1_LIST, c->ix);
}
static void p_start_search1(Cpu *c) { start_search1(c); }

/* $7AA1: the same, unless repeating for ALL. */
static void p_start_search1_once(Cpu *c) {
  c->a = M(ALL_MODE);
  and_a(c);
  if (!c->zf) return;
  start_search1(c);
}

/* $7ABA: start the search for the second object. */
static void p_start_search2(Cpu *c) {
  c->a = M(ACTION_FLAGS2);
  cpu_call_at(c, 0x7ABD); /* 0x7ACC */
  c->a = M(OBJ2_CHAR);
  and_a(c);
  if (!c->zf) cpu_call_at(c, 0x7AC4); /* R_VAR_9E95 */
  wr16(c, N2_LIST, c->ix);
}

/* $7AD8: NZ if the action needs a second object (flags bit 2) that was not
 * given and bit 1 is clear. */
static void p_need_obj2(Cpu *c) {
  c->a = M(ACTION_FLAGS1);
  op_bit(c, 2, c->a);
  if (c->zf) return;
  set_hl(c, N2_FLAGS);
  op_bit(c, 0, M(N2_FLAGS));
  if (!c->zf) return;
  op_bit(c, 1, c->a);
  if (!c->zf) {
    xor_a(c);
    return;
  }
  c->a = op_or(c, c->a, 0x01);
}

/* $7AED: try the action; NZ if it was done. */
static void p_try_action(Cpu *c) {
  call(c, R_DO_ACTION, 0x7AF0);
  c->a = M(ACTION_DONE);
  and_a(c);
}

/* $7B63: unless B is $FF, copy the 6-byte name of object B (a character
 * if A is nonzero) to DE. */
static void p_copy_name(Cpu *c) {
  c->b = op_inc(c, c->b);
  if (c->zf) return;
  c->b = op_dec(c, c->b);
  and_a(c);
  c->a = c->b;
  if (c->zf)
    cpu_call_at(c, 0x7B6F); /* R_OBJ_NAME */
  else
    cpu_call_at(c, 0x7B6A); /* R_OBJ_NAME_CHAR */
  set_bc(c, 6);
  ldir(c);
}

/* $7B78: decode the action flags into $B711, $B70F, OBJ1_CHAR, OBJ2_CHAR. */
static void p_decode_flags(Cpu *c) {
  c->a = op_and(c, M(ACTION_FLAGS2), 0x40);
  M(VAR_B711) = c->a;
  c->a = M(ACTION_FLAGS1);
  c->b = c->a;
  c->a = op_and(c, c->a, 0x01);
  M(VAR_B70F) = c->a;
  c->a = op_and(c, c->b, 0x80);
  if (!c->zf) c->a = 1;
  M(OBJ1_CHAR) = c->a;
  c->a = op_and(c, c->b, 0x40);
  if (!c->zf) c->a = 1;
  M(OBJ2_CHAR) = c->a;
}

/* $7C91: copy the 6-byte phrase at IY+A to DE; if it is not empty, set
 * bit 0 of (HL). */
static void p_copy_phrase(Cpu *c) {
  push16(c, get_bc(c));
  c->c = c->a;
  c->b = 0;
  push16(c, get_hl(c));
  push16(c, c->iy); /* PUSH IY; POP HL */
  set_hl(c, pop16(c));
  set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
  set_bc(c, 6);
  ldir(c);
  xor_a(c);
  c->b = 6;
  do {
    set_hl(c, get_hl(c) - 1);
    c->a = op_or(c, c->a, M(get_hl(c)));
  } while (--c->b);
  set_hl(c, pop16(c));
  set_bc(c, pop16(c));
  if (c->zf) return;
  M(get_hl(c)) |= 0x01;
}

/* $7CAC: while B > 0, copy the word at IY+E to (HL); if it is not zero,
 * count it (B-1) and move HL on. */
static void p_copy_word(Cpu *c) {
  xor_a(c);
  op_cp(c, c->b);
  if (c->zf) return;
  c->d = 0;
  push16(c, c->iy);
  c->iy = op_add16(c, c->iy, get_de(c));
  uint16_t hl = get_hl(c);
  c->a = M(c->iy);
  M(hl) = c->a;
  hl++;
  c->a = M(c->iy + 1);
  M(hl) = c->a;
  hl--;
  c->a = op_or(c, c->a, M(c->iy));
  c->iy = pop16(c);
  set_hl(c, hl);
  if (c->zf) return;
  c->b = op_dec(c, c->b);
  set_hl(c, hl + 2);
}

/* $7CC9: JP (IY), the object matcher chosen by $7CCB/$7D17. */
static void p_call_iy(Cpu *c) { cpu_tail(c, c->iy); }

/* $7CFC: find the next object for the first noun phrase that the action
 * accepts; A = $FF if none. */
static void p_next_obj1(Cpu *c) {
L7CFC:
  set_hl(c, N1_PHRASE);
  cpu_call_at(c, 0x7CFF); /* 0x7CC9 */
  op_cp(c, 0xFF);
  if (c->zf) return;
  M(OBJ1) = c->a;
  set_hl(c, N1_FLAGS);
  M(N1_FLAGS) |= 0x02;
  call(c, R_CHECK_ACTION, 0x7D10);
  c->a = M(ACTION_DONE);
  and_a(c);
  if (c->zf) goto L7CFC;
}

/* $7CCB: the next object for the first noun phrase, from N1_LIST, among
 * objects or characters. NZ if found. */
static void p_find_obj1(Cpu *c) {
  push16(c, c->iy);
  c->ix = rd16(c, N1_LIST);
  c->a = op_dec(c, M(OBJ1_CHAR));
  if (c->zf) {
    c->iy = R_MATCH_CHAR;
    call(c, 0x7CFC, 0x7CF8);
    op_cp(c, 0xFF);
  } else {
    c->iy = R_MATCH_OBJECT;
    c->a = M(ACTION_FLAGS2);
    op_rrca(c);
    op_rrca(c);
    c->a = op_and(c, c->a, 0x03);
    M(VAR_B710) = c->a;
    call(c, 0x7CFC, 0x7CE8);
    op_cp(c, 0xFF);
  }
  wr16(c, N1_LIST, c->ix);
  c->iy = pop16(c);
}

/* $7D54: as $7CFC, for the second noun phrase. */
static void p_next_obj2(Cpu *c) {
L7D54:
  set_hl(c, N2_PHRASE);
  cpu_call_at(c, 0x7D57); /* 0x7CC9 */
  op_cp(c, 0xFF);
  if (c->zf) return;
  M(OBJ2) = c->a;
  set_hl(c, N2_FLAGS);
  M(N2_FLAGS) |= 0x02;
  call(c, 0x7AED, 0x7D68);
  if (c->zf) goto L7D54;
}

/* $7D17: as $7CCB, for the second noun phrase. */
static void p_find_obj2(Cpu *c) {
  xor_a(c);
  M(VAR_B70F) = c->a;
  push16(c, c->iy);
  c->ix = rd16(c, N2_LIST);
  c->a = op_dec(c, M(OBJ2_CHAR));
  if (c->zf) {
    c->iy = R_MATCH_CHAR;
    call(c, 0x7D54, 0x7D50);
    op_cp(c, 0xFF);
  } else {
    c->iy = R_MATCH_OBJECT;
    c->a = op_and(c, M(ACTION_FLAGS2), 0x03);
    M(VAR_B710) = c->a;
    call(c, 0x7D54, 0x7D36);
    op_cp(c, 0xFF);
  }
  wr16(c, N2_LIST, c->ix);
  c->iy = pop16(c);
  cpu_push_af(c);
  c->a = op_and(c, M(ACTION_FLAGS1), 0x01);
  M(VAR_B70F) = c->a;
  cpu_pop_af(c);
}

/* $7D6B: output on, to the text window. */
static void p_output_on(Cpu *c) {
  c->a = 1;
  M(VAR_B6FA) = c->a;
  M(PRINT_TO_INPUT) = c->a;
}

/* $7D74: ask about the record at IY: keep it at $B9C8, with VAR_B71A = A
 * (the offset of the phrase in question), for the answer to complete. */
static void p_keep_question(Cpu *c) {
  M(VAR_B71A) = c->a;
  push16(c, c->iy); /* PUSH IY; POP HL */
  set_hl(c, pop16(c));
  set_de(c, RECORDS);
  set_bc(c, 0x18);
  ldir(c);
}

/* $7E7E: with A the opcode of JR NZ ($20, from $7E7C) or JR Z ($28, from
 * $7E78), written into $7E92 and $7EA1: push two words from the action
 * entry, IX+4 if bit 7 of IX+7 is set (NZ) / clear (Z), and IX+2 if bit 5
 * of IX+3 is set / clear, or 0. Leaves HL = ret. */
static void push_entry_words(Cpu *c, uint16_t ret) {
  M(0x7E92) = c->a;
  M(0x7EA1) = c->a;
  c->ix = rd16(c, ACTION_ENTRY);
  set_hl(c, rd16(c, c->ix + 4));
  op_bit(c, 7, M(c->ix + 7));
  if (!(M(0x7E92) == 0x20 ? !c->zf : c->zf)) set_hl(c, 0);
  push16(c, get_hl(c));
  set_hl(c, rd16(c, c->ix + 2));
  op_bit(c, 5, c->h);
  if (!(M(0x7EA1) == 0x20 ? !c->zf : c->zf)) set_hl(c, 0);
  push16(c, get_hl(c));
  set_hl(c, ret);
}

static void entry_words_called(Cpu *c) {
  uint16_t ret = pop16(c);
  push_entry_words(c, ret);
  push16(c, ret); /* for the RET */
}
static void p_entry_words_z(Cpu *c) { /* $7E78 */
  c->a = 0x28;
  entry_words_called(c);
}
static void p_entry_words_nz(Cpu *c) { /* $7E7C */
  c->a = 0x20;
  entry_words_called(c);
}

/* ---------- matching the noun phrases ---------- */

/* $7A14: find objects for the noun phrases: Z when the action is settled
 * (done, or nothing more to try), NZ when it could not be. */
static void p_match_objects(Cpu *c) {
L7A14:
  call(c, 0x7CCB, 0x7A17);
  if (!c->zf) goto L7A2E;
  c->a = M(N1_COUNT);
  op_cp(c, 0x01);
  if (!c->zf) return;
  c->a = M(N1_LAST);
  M(OBJ1) = c->a;
  cpu_call_at(c, 0x7A25); /* 0x7ABA */
  cpu_call_at(c, 0x7A28); /* 0x7AD8 */
  if (!c->zf) goto L7A55;
  return;
L7A2E:
  set_hl(c, N1_TRIES);
  M(N1_TRIES) = op_inc(c, M(N1_TRIES));
  cpu_call_at(c, 0x7A32); /* 0x7AD8 */
  if (c->zf) goto L7A50;
  cpu_call_at(c, 0x7A37); /* 0x7ABA */
  call(c, 0x7D17, 0x7A3D);
L7A3D:
  if (c->zf) goto L7A14;
  c->a = op_dec(c, M(VAR_7953));
  if (c->zf) return;
  c->a = M(OBJ1);
  M(N1_LAST) = c->a;
  set_hl(c, N1_COUNT);
  M(N1_COUNT) = op_inc(c, M(N1_COUNT));
  goto L7A14;
L7A50:
  call(c, 0x7AED, 0x7A53);
  goto L7A3D;
L7A55:
  call(c, 0x7D17, 0x7A58);
  if (!c->zf) {
    c->a = M(OBJ2);
    M(N2_LAST) = c->a;
    set_hl(c, N2_COUNT);
    M(N2_COUNT) = op_inc(c, M(N2_COUNT));
    goto L7A55;
  }
  c->a = M(N2_COUNT);
  op_cp(c, 0x01);
  if (!c->zf) return;
  c->a = M(N2_LAST);
  M(OBJ2) = c->a;
}

/* $7A73: for ALL: NZ if a record below IY (with bit 6 of byte 1 set, a
 * continuation) names an object of the list that is OBJ1, moving IY to
 * it; Z when there are no more. Keeps IY, DE, HL. */
static void p_next_all(Cpu *c) {
  push16(c, c->iy);
  push16(c, get_de(c));
  push16(c, get_hl(c));
L7A77:
  set_de(c, 0xFFE8);
  c->iy = op_add16(c, c->iy, get_de(c));
  op_bit(c, 6, M(c->iy + 1));
  if (c->zf) goto L7A9C;
  c->ix = OBJECTS;
L7A86:
  push16(c, c->iy); /* PUSH IY; POP HL */
  set_hl(c, pop16(c));
  set_de(c, 8);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  cpu_call_at(c, 0x7A8D); /* R_MATCH_OBJECT */
  op_cp(c, 0xFF);
  if (c->zf) goto L7A77;
  set_hl(c, OBJ1);
  op_cp(c, M(OBJ1));
  if (!c->zf) goto L7A86;
  c->a = op_or(c, c->a, 0x01);
L7A9C:
  set_hl(c, pop16(c));
  set_de(c, pop16(c));
  c->iy = pop16(c);
}

/* $7C23: having found the action entry (IX) for the record at IY: swap
 * the two prepositions if A is nonzero, take the action's flags, decide
 * which of the record's phrases ($08 or $12) is the direct object (by
 * the prepositions and flags bit 5), and copy both into N1/N2_PHRASE
 * (and the first into IT_PHRASE). */
static void p_take_phrases(Cpu *c) {
  and_a(c);
  if (!c->zf) {
    uint16_t hl = rd16(c, PREP1), de = rd16(c, PREP2);
    wr16(c, PREP1, de);
    wr16(c, PREP2, hl);
    set_hl(c, hl);
    set_de(c, de);
  }
  cpu_call_at(c, 0x7C34); /* R_ACTION_FLAGS */
  {
    uint16_t hl = PREP1;
    c->a = M(hl);
    hl++;
    c->a = op_or(c, c->a, M(hl));
    if (!c->zf) goto L7C44;
    set_hl(c, hl);
    c->a = M(ACTION_FLAGS2);
    goto L7C63;
  L7C44:
    hl--;
    c->a = M(hl);
    op_cp(c, M(c->iy + 0x0E));
    if (!c->zf) goto L7C53;
    hl++;
    c->a = M(hl);
    op_cp(c, M(c->iy + 0x0F));
    if (c->zf) goto L7C5E;
    hl--;
  L7C53:
    c->a = M(hl);
    op_cp(c, M(c->iy + 0x10));
    if (!c->zf) goto L7C5E;
    hl++;
    c->a = M(hl);
    op_cp(c, M(c->iy + 0x11));
  L7C5E:
    set_hl(c, hl);
    c->a = M(ACTION_FLAGS1);
    if (!c->zf) goto L7C65;
  }
L7C63:
  c->a = op_xor(c, c->a, 0x20);
L7C65:
  op_bit(c, 5, c->a);
  set_bc(c, 0x1208);
  if (!c->zf) set_bc(c, 0x0812);
  M(N1_FIELD) = c->b;
  M(N2_FIELD) = c->c;
  c->a = c->b;
  set_de(c, N1_PHRASE);
  set_hl(c, N1_FLAGS);
  cpu_call_at(c, 0x7C7C); /* 0x7C91 */
  c->a = c->c;
  set_hl(c, N1_PHRASE);
  set_de(c, IT_PHRASE);
  set_bc(c, 6);
  ldir(c);
  set_de(c, N2_PHRASE);
  set_hl(c, N2_FLAGS);
  p_copy_phrase(c); /* the original falls into it */
}

/* $7B9E: find the action for the record at IY: the verb (bit 7 of byte 1:
 * ALL) and up to two prepositions are looked up in the action table. Z
 * when there is none (a message is printed); NZ with IX the entry, and
 * the phrases taken ($7C23). ESC_7C16: the original goes on at $7C16. */
static int find_action(Cpu *c) {
  push16(c, c->iy);
  c->l = M(c->iy);
  c->h = M(c->iy + 1);
  c->a = op_and(c, c->h, 0x80);
  M(ALL_FLAG) = c->a;
  c->h &= 0x7F;
  wr16(c, VERB, get_hl(c));
  set_hl(c, PREP1);
  push16(c, get_hl(c));
  c->b = 4;
  cpu_call_at(c, 0x7BB7); /* Blanker */
  set_hl(c, pop16(c));
  c->b = 2;
  c->e = 0x04;
  cpu_call_at(c, 0x7BBF); /* 0x7CAC */
  c->e = 0x0E;
  cpu_call_at(c, 0x7BC4); /* 0x7CAC */
  c->e = 0x06;
  cpu_call_at(c, 0x7BC9); /* 0x7CAC */
  c->e = 0x10;
  cpu_call_at(c, 0x7BCE); /* 0x7CAC */
  xor_a(c);
  M(VAR_B6DF) = c->a;
  set_hl(c, VERB);
  set_de(c, 8);
  c->ix = ACTIONS;
L7BDF:
  push16(c, c->ix); /* PUSH IX; POP IY */
  c->iy = pop16(c);
  cpu_call_at(c, 0x7BE3); /* R_MATCH_VERB */
  if (c->zf) goto L7C1B;
  c->ix = op_add16(c, c->ix, get_de(c));
  c->a = op_or(c, M(c->ix + 1), M(c->ix));
  if (!c->zf) goto L7BDF;
  c->iy = pop16(c);
  c->a = M(VAR_B6DF);
  and_a(c);
  if (!c->zf) { /* $7EA8 */
    push16(c, rd16(c, PREP2));
    push16(c, rd16(c, PREP1));
    push16(c, rd16(c, VERB));
    set_hl(c, MSG_CANT_VERB);
    return print_and_return(c);
  }
  push16(c, rd16(c, VERB));
  set_hl(c, MSG_DONT_UNDERSTAND);
  xor_a(c);
  M(PRINT_TO_INPUT) = c->a;
  M(ALL_MODE) = c->a;
  c->a = 1;
  M(VAR_B6FA) = c->a;
  print_msg(c, 0x7C11);
  c->a = op_dec(c, M(IN_QUOTES));
  if (c->zf) return OK;
  return ESC_7C16;
L7C1B:
  c->iy = pop16(c);
  cpu_call_at(c, 0x7C1D); /* 0x7C23 */
  c->a = op_or(c, c->a, 0x01);
  return OK;
}

static void p_find_action(Cpu *c) {
  if (find_action(c) == ESC_7C16) cpu_tail(c, 0x7C16);
}

/* ---------- carrying out a sentence ---------- */

/* $79B6: carry out the sentence whose verb record is at IY. NZ when done
 * (or to be repeated for ALL), Z when it stopped with a message or a
 * question. The original sometimes leaves through its caller (see the
 * ESC_ codes); the stack is then as the original leaves it at the jump. */
static int run_sentence(Cpu *c) {
  int r;
  c->a = 0xFF;
  M(OBJ2) = c->a;
  M(OBJ1) = c->a;
  cpu_call_at(c, 0x79BE); /* 0x79A9 */
  push16(c, 0x79C4); /* CALL $7B9E */
  if ((r = find_action(c)) != OK) return r; /* (the return address is still there) */
  c->sp += 2;
  if (c->zf) return OK;
  c->a = 1;
  {
    push16(c, c->ix); /* PUSH IX; POP HL */
    uint16_t hl = op_sbc16(c, pop16(c), ACTIONS);
    set_de(c, ACTIONS);
    if (!c->zf) {
      set_de(c, 8);
      do {
        c->a = op_inc(c, c->a);
        hl = op_sbc16(c, hl, 8);
      } while (!c->zf);
    }
    set_hl(c, hl);
  }
  M(ACTION) = c->a;
  M(ACTION_B6E6) = c->a;
  wr16(c, ACTION_ENTRY, c->ix);
  cpu_call_at(c, 0x79E3); /* 0x7B78 */
  cpu_call_at(c, 0x79E6); /* 0x7AA1 */
  xor_a(c);
  M(VAR_B6FA) = c->a;
  c->a = op_and(c, M(ACTION_FLAGS1), 0x0C);
  if (c->zf) goto L7A11;
  c->a = M(ALL_FLAG);
  M(ALL_MODE) = c->a;
  op_rlca(c);
  c->a = op_and(c, c->a, 0x01);
  M(VAR_7953) = c->a;
L7A00:
  call(c, 0x7A14, 0x7A03);
  if (!c->zf) goto L7DBC;
  c->a = M(ALL_MODE);
  and_a(c);
  if (c->zf) goto L7A11;
  cpu_call_at(c, 0x7A0C); /* 0x7A73 */
  if (!c->zf) goto L7A00;
L7A11:
  c->a = op_or(c, c->a, 0x01);
  return OK;

L7DBC: /* the objects did not work out: say why, or ask which */
  c->a = op_dec(c, M(IN_QUOTES));
  if (c->zf) return OK;
  c->a = M(ALL_MODE);
  and_a(c);
  if (!c->zf) return ESC_7DC7;
  c->a = M(N1_COUNT);
  op_cp(c, 0x01);
  if (c->zf) goto L7D98;
  set_hl(c, N1_FLAGS);
  op_bit(c, 0, M(N1_FLAGS));
  if (c->zf) goto L7E30;
  op_bit(c, 1, M(N1_FLAGS));
  set_hl(c, N1_PHRASE);
  set_de(c, OBJ1);
  set_bc(c, OBJ1_CHAR);
  if (c->zf) goto L7DFE;
  c->a = M(N1_TRIES);
  and_a(c);
  if (c->zf) goto L7DF5;
  c->a = op_dec(c, c->a);
  if (!c->zf) goto L7D83;
  cpu_call_at(c, 0x7DF0); /* 0x7AD8 */
  if (!c->zf) goto L7D98;
L7DF5:
  cpu_call_at(c, 0x7DF5); /* 0x7D6B */
  call(c, R_DO_ACTION, 0x7DFB);
  return ESC_798E;
L7D83: /* more than one fits: "which ...?" */
  c->a = M(N1_FIELD);
  set_hl(c, rd16(c, N1_PHRASE));
L7D89:
  push16(c, get_hl(c));
  cpu_call_at(c, 0x7D8A); /* 0x7D74 */
  set_hl(c, MSG_WHICH);
  return print_and_return(c);
L7D98:
  set_hl(c, N2_FLAGS);
  op_bit(c, 0, M(N2_FLAGS));
  if (c->zf) goto L7E4D;
  op_bit(c, 1, M(N2_FLAGS));
  set_hl(c, N2_PHRASE);
  set_de(c, OBJ2);
  set_bc(c, OBJ2_CHAR);
  if (c->zf) goto L7DFE;
  c->a = M(N2_COUNT);
  and_a(c);
  if (c->zf) goto L7DF5;
  /* The original loads N2_FIELD and then overwrites it with the first
   * byte of N2_PHRASE, and pushes the address N2_PHRASE, not the word. */
  c->a = M(N2_FIELD);
  c->a = M(N2_PHRASE);
  goto L7D89;
L7DFE: /* nothing fitted: look for it anywhere */
  push16(c, get_hl(c));
  cpu_call_at(c, 0x7DFF); /* R_VAR_9E95 */
  c->a = 1;
  M(get_bc(c)) = c->a;
  cpu_call_at(c, 0x7E05); /* R_MATCH_CHAR */
  op_cp(c, 0xFF);
  if (!c->zf) goto L7E20;
  set_hl(c, pop16(c));
  c->a = 2;
  M(VAR_B710) = c->a;
  c->ix = OBJECTS;
  xor_a(c);
  M(get_bc(c)) = c->a;
  push16(c, get_hl(c));
  cpu_call_at(c, 0x7E19); /* R_MATCH_OBJECT */
  op_cp(c, 0xFF);
  if (c->zf) { /* $7E25: "you can't see ..." with the phrase pushed */
    cpu_call_at(c, 0x7E25); /* 0x7D6B */
    set_hl(c, MSG_CANT_SEE);
    print_msg(c, 0x7E2E);
    xor_a(c);
    return OK;
  }
L7E20:
  set_hl(c, pop16(c));
  M(get_de(c)) = c->a;
  /* JP $711A, whose RET is RunSentence's. Its seven instructions are
   * followed here (from text.c's range) so that its CALL runs with the
   * stack as in the original. */
  xor_a(c);
  M(ACTION_DONE) = c->a;
  c->a = op_inc(c, c->a);
  M(VAR_B6FA) = c->a;
  c->a = M(IN_QUOTES);
  and_a(c);
  cpu_call_at(c, 0x7126); /* CALL Z,R_TURN */
  xor_a(c);
  return OK;
L7E30: /* no first noun phrase given */
  c->a = 0x20;
  push_entry_words(c, 0x7E33);
  set_hl(c, rd16(c, VERB));
  push16(c, get_hl(c));
  set_hl(c, MSG_ADE1);
  c->a = M(N1_COUNT);
  and_a(c);
  if (c->zf) return print_and_return(c);
  c->a = M(N1_FIELD);
  cpu_call_at(c, 0x7E44); /* 0x7D74 */
  set_hl(c, MSG_ADC4);
  return print_and_return(c);
L7E4D: /* no second noun phrase given */
  c->a = 0x28;
  push_entry_words(c, 0x7E50);
  set_hl(c, 0);
  push16(c, get_hl(c));
  c->a = M(OBJ1);
  cpu_call_at(c, 0x7E57); /* R_OBJ_NAME */
  push16(c, get_hl(c));
  c->a = 0x20;
  push_entry_words(c, 0x7E5E);
  set_hl(c, rd16(c, VERB));
  push16(c, get_hl(c));
  set_hl(c, MSG_ADE7);
  c->a = M(N2_COUNT);
  and_a(c);
  if (c->zf) return print_and_return(c);
  c->a = M(N2_FIELD);
  cpu_call_at(c, 0x7E6F); /* 0x7D74 */
  set_hl(c, MSG_ADC0);
  return print_and_return(c);
}

/* $79B6 as called by other code: what the original does when it leaves
 * through the caller's frame is handed to the original at that point. */
static void p_run_sentence(Cpu *c) {
  switch (run_sentence(c)) {
  case ESC_7C16: /* $7B9E's return address is on top */
    cpu_tail(c, 0x7C16);
    break;
  case ESC_7DC7: cpu_tail(c, 0x7DC7); break;
  case ESC_798E: cpu_tail(c, 0x798E); break;
  }
}

/* $7960: carry out the sentences parsed into the records: each record
 * with a verb, in turn, then the game turn. When VAR_B71A was set (the
 * line answers a question), the first record is the question's, kept at
 * $B9C8, and is skipped. */
enum { FROM_START, FROM_798B, FROM_798E, FROM_7994 };

static void execute(Cpu *c, int from) {
  switch (from) {
  case FROM_798B: goto L798B;
  case FROM_798E: goto L798E;
  case FROM_7994: goto L7994;
  }
  xor_a(c);
  M(ALL_MODE) = c->a;
  c->iy = RECORDS;
  set_hl(c, VAR_B71A);
  op_cp(c, M(VAR_B71A));
  M(VAR_B71A) = c->a;
  if (!c->zf) goto L7994;
L7970:
  push16(c, 0x7973);
  switch (run_sentence(c)) {
  case OK: c->sp += 2; break; /* RET */
  case ESC_7C16: /* POP HL (the return address of $7B9E); POP HL; JP $798B */
    pop16(c);
    set_hl(c, pop16(c));
    goto L798B;
  case ESC_7DC7: /* POP HL; JP $7994 */
    set_hl(c, pop16(c));
    goto L7994;
  case ESC_798E: /* JP $798E, leaving $7973 on the stack */
    goto L798E;
  }
L7973:
  if (!c->zf) goto L797A;
  xor_a(c);
  M(VAR_B705) = c->a;
  goto ret;
L797A:
  call(c, 0x7AED, 0x797D);
  if (c->zf) { /* $7DF5 */
    cpu_call_at(c, 0x7DF5); /* 0x7D6B */
    call(c, R_DO_ACTION, 0x7DFB);
    goto L798E;
  }
  c->a = 1;
  M(VAR_B6FA) = c->a;
  call(c, R_TURN, 0x7988);
  call(c, R_DO_ACTION, 0x798B);
L798B:
  call(c, R_ACTORS, 0x798E);
L798E:
  c->a = M(ALL_MODE);
  and_a(c);
  if (!c->zf) goto L7970;
L7994:
  c->a = op_dec(c, M(PHRASE_COUNT));
  M(PHRASE_COUNT) = c->a;
  if (c->zf) goto ret;
  set_bc(c, 0xFFE8);
  do c->iy = op_add16(c, c->iy, 0xFFE8);
  while (op_bit(c, 6, M(c->iy + 1)));
  goto L7970;
ret:
  /* A return address left by $7DF5 (JP $798E) is returned to first. */
  if (c->sp != c->frame_sp) {
    c->sp += 2;
    goto L7973;
  }
}

static void p_execute(Cpu *c) { execute(c, FROM_START); }

/* Continuation points: where the original goes on after leaving
 * RunSentence through its caller's frame (see run_sentence). Each runs
 * from there with the return address it then has on top of the stack:
 * Execute's own, or $7973 left by $7DF5. */
static void p_execute_798e(Cpu *c) { execute(c, FROM_798E); } /* JP $798E from $7DFB */
static void p_execute_7dc8(Cpu *c) { execute(c, FROM_7994); } /* $7DC8 JP $7994 */
static void p_execute_7c18(Cpu *c) { execute(c, FROM_798B); } /* $7C18 JP $798B */

/* $7DC7: POP HL, leaving the frame (on to $7DC8). */
static void p_pop_7dc7(Cpu *c) {
  set_hl(c, pop16(c));
  cpu_tail(c, 0x7DC8);
}
/* $7C16, $7C17: POP HL each, leaving the frame (on to the next). */
static void p_pop_7c16(Cpu *c) {
  set_hl(c, pop16(c));
  cpu_tail(c, 0x7C17);
}
static void p_pop_7c17(Cpu *c) {
  set_hl(c, pop16(c));
  cpu_tail(c, 0x7C18);
}

/* ---------- called from the game code ---------- */

/* $7AF5 (from $99C8): with all registers kept, try action $B6E7 again on
 * OBJ1/OBJ2 for the current character: NZ if it was done. */
static void p_retry_action(Cpu *c) {
  push16(c, get_hl(c));
  push16(c, c->iy);
  push16(c, c->ix);
  push16(c, get_de(c));
  push16(c, get_bc(c));
  set_hl(c, rd16(c, N1_LIST));
  push16(c, get_hl(c));
  c->a = M(ACTION);
  cpu_call_at(c, 0x7B03); /* R_INDEX_ACTION */
  push16(c, get_hl(c)); /* PUSH HL; POP IX */
  c->ix = pop16(c);
  cpu_call_at(c, 0x7B09); /* 0x79A9 */
  cpu_call_at(c, 0x7B0C); /* R_ACTION_FLAGS */
  cpu_call_at(c, 0x7B0F); /* 0x7B78 */
  c->a = M(OBJ1);
  c->b = c->a;
  c->a = M(OBJ1_CHAR);
  set_de(c, N1_PHRASE);
  cpu_call_at(c, 0x7B1C); /* 0x7B63 */
  c->a = M(OBJ2);
  c->b = c->a;
  c->a = M(OBJ2_CHAR);
  set_de(c, N2_PHRASE);
  cpu_call_at(c, 0x7B29); /* 0x7B63 */
  cpu_call_at(c, 0x7B2C); /* 0x7AA6 */
  xor_a(c);
  M(VAR_B6FA) = c->a;
  c->a = op_and(c, M(ACTION_FLAGS1), 0x0C);
  if (!c->zf) {
    c->a = 1;
    M(VAR_7953) = c->a;
    call(c, 0x7A14, 0x7B4B);
    if (c->zf)
      c->a = op_or(c, c->a, 0x01);
    else
      xor_a(c);
  } else {
    call(c, R_DO_ACTION, 0x7B3D);
    c->a = M(ACTION_DONE);
    and_a(c);
  }
  c->a = 1;
  M(VAR_B6FA) = c->a;
  set_hl(c, pop16(c));
  wr16(c, N1_LIST, get_hl(c));
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  c->ix = pop16(c);
  c->iy = pop16(c);
  set_hl(c, pop16(c));
}

/* $7EBA (from $9058): of the SPEECH_COUNT commands just said, give the
 * first A (at most) to OBJ1 and cancel the rest: the $FF slots after
 * $B71F are given OBJ1, then cleared. */
static void p_assign_speech(Cpu *c) {
  push16(c, get_bc(c));
  push16(c, c->ix);
  push16(c, get_de(c));
  c->b = c->a;
  c->a = M(SPEECH_COUNT);
  c->c = c->a;
  op_cp(c, c->b);
  if (c->cf) c->b = c->a;
  c->a = op_sub(c, c->c, c->b, 0);
  c->c = c->a;
  c->ix = 0xB71F;
  set_de(c, 0x19);
  xor_a(c);
  op_cp(c, c->b);
  if (!c->zf) {
    do {
      do {
        c->ix = op_add16(c, c->ix, get_de(c));
        c->a = M(c->ix);
        op_cp(c, 0xFF);
      } while (!c->zf);
      c->a = M(OBJ1);
      M(c->ix) = c->a;
    } while (--c->b);
  }
  c->b = c->c;
  xor_a(c);
  op_cp(c, c->b);
  if (!c->zf) {
    do {
      do {
        c->ix = op_add16(c, c->ix, get_de(c));
        c->a = M(c->ix);
        op_cp(c, 0xFF);
      } while (!c->zf);
      M(c->ix) = 0;
    } while (--c->b);
  }
  set_de(c, pop16(c));
  c->ix = pop16(c);
  set_bc(c, pop16(c));
}

/* $7EFF: HL = the speech slot for the current character (ACTOR), Z if
 * there is one. */
static void p_find_speech(Cpu *c) {
  set_hl(c, SPEECH);
  set_de(c, 0x19);
  c->a = M(ACTOR);
  c->b = 8;
  do {
    op_cp(c, M(get_hl(c)));
    if (c->zf) return;
    set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  } while (--c->b);
}

/* $7F10 (from $9874): $7EFF keeping HL, DE, BC. */
static void p_has_speech(Cpu *c) {
  push16(c, get_hl(c));
  push16(c, get_de(c));
  push16(c, get_bc(c));
  cpu_call_at(c, 0x7F13); /* 0x7EFF */
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  set_hl(c, pop16(c));
}

/* $7F1A: the current character carries out what it was told: clear its
 * speech slot's owner; with A = 0, return NZ and HL = the record; else
 * run the record as a sentence (with IN_QUOTES set). Z when nothing was
 * done, and then its other orders are dropped too ($7F60). */
static void p_obey(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  push16(c, get_bc(c));
  push16(c, get_de(c));
  push16(c, get_hl(c));
  c->c = c->a;
  cpu_call_at(c, 0x7F22); /* 0x7EFF */
  M(get_hl(c)) = 0;
  set_hl(c, get_hl(c) + 1);
  xor_a(c);
  op_cp(c, c->c);
  if (c->zf) {
    c->a = op_or(c, c->a, 0x01);
    ex_sp_hl(c);
    goto L7F2F;
  }
  push16(c, get_hl(c)); /* PUSH HL; POP IY */
  c->iy = pop16(c);
  c->a = 1;
  M(IN_QUOTES) = c->a;
  c->a = M(ALL_MODE);
  cpu_push_af(c);
  call(c, 0x79B6, 0x7F46);
  {
    uint8_t ra = c->a, rf = cpu_f(c); /* EX AF,AF' (AF' itself is not modelled) */
    xor_a(c);
    M(IN_QUOTES) = c->a;
    cpu_pop_af(c);
    M(ALL_MODE) = c->a;
    c->a = ra;
    cpu_set_f(c, rf);
  }
  if (c->zf) goto L7F57;
  call(c, 0x7AED, 0x7F55);
  if (!c->zf) goto L7F2F;
L7F57:
  c->a = M(ACTOR);
  cpu_call_at(c, 0x7F5A); /* 0x7F60 */
  xor_a(c);
L7F2F:
  set_hl(c, pop16(c));
  set_de(c, pop16(c));
  set_bc(c, pop16(c));
  c->iy = pop16(c);
  c->ix = pop16(c);
}

/* $7F60: clear the speech slots of character A. Keeps HL, DE, BC. */
static void p_drop_speech(Cpu *c) {
  push16(c, get_hl(c));
  push16(c, get_de(c));
  push16(c, get_bc(c));
  set_hl(c, SPEECH);
  set_de(c, 0x19);
  c->b = 8;
  do {
    op_cp(c, M(get_hl(c)));
    if (c->zf) M(get_hl(c)) = 0;
    set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  } while (--c->b);
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  set_hl(c, pop16(c));
}

#define STD (OUT_REGS | OUT_ZF | OUT_CF)

const PortRoutine executor_routines[] = {
    {0x7960, "Execute", p_execute, STD},
    {0x79A9, "ClearExecVars", p_clear_vars, STD},
    {0x79B6, "RunSentence", p_run_sentence, STD},
    {0x7A14, "MatchObjects", p_match_objects, STD},
    {0x7A73, "NextAllRecord", p_next_all, STD},
    {0x7AA1, "StartSearch1Once", p_start_search1_once, STD},
    {0x7AA6, "StartSearch1", p_start_search1, STD},
    {0x7ABA, "StartSearch2", p_start_search2, STD},
    {0x7ACC, "ObjectList", p_object_list, STD},
    {0x7AD8, "NeedObject2", p_need_obj2, STD},
    {0x7AED, "TryAction", p_try_action, STD},
    {0x798E, "Execute798E", p_execute_798e, STD},
    {0x7C16, "Pop7C16", p_pop_7c16, STD},
    {0x7C17, "Pop7C17", p_pop_7c17, STD},
    {0x7C18, "Execute7C18", p_execute_7c18, STD},
    {0x7DC7, "Pop7DC7", p_pop_7dc7, STD},
    {0x7DC8, "Execute7DC8", p_execute_7dc8, STD},
    {0x7AF5, "RetryAction", p_retry_action, STD},
    {0x7B63, "CopyName", p_copy_name, STD},
    {0x7B78, "DecodeFlags", p_decode_flags, STD},
    {0x7B9E, "FindAction", p_find_action, STD},
    {0x7C23, "TakePhrases", p_take_phrases, STD},
    {0x7C91, "CopyPhrase", p_copy_phrase, STD},
    {0x7CAC, "CopyWord", p_copy_word, STD},
    {0x7CC9, "CallIY", p_call_iy, STD},
    {0x7CCB, "FindObject1", p_find_obj1, STD},
    {0x7CFC, "NextObject1", p_next_obj1, STD},
    {0x7D17, "FindObject2", p_find_obj2, STD},
    {0x7D54, "NextObject2", p_next_obj2, STD},
    {0x7D6B, "OutputOn", p_output_on, STD},
    {0x7D74, "KeepQuestion", p_keep_question, STD},
    {0x7E78, "EntryWordsZ", p_entry_words_z, STD},
    {0x7E7C, "EntryWordsNZ", p_entry_words_nz, STD},
    {0x7EBA, "AssignSpeech", p_assign_speech, STD},
    {0x7EFF, "FindSpeech", p_find_speech, STD},
    {0x7F10, "HasSpeech", p_has_speech, STD},
    {0x7F1A, "Obey", p_obey, STD},
    {0x7F60, "DropSpeech", p_drop_speech, STD},
    {0, NULL, NULL, 0},
};
