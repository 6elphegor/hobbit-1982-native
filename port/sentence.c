/* The sentence parser: turns the token list built from the input line
 * ($709C, two bytes a word: class and dictionary offset) into noun-phrase
 * records at $B9B0.. and the phrase buffer at $757B. Translated from
 * $7585-$793C and $8251-$838E (the special words).
 *
 * It is a state machine driven by a jump table on the class of each word
 * ($75D2), with flags in E (which parts of a phrase are still allowed),
 * the class expected next in D, and IY pointing at the current record.
 * Labels keep the original addresses so the code can be followed against
 * the disassembly. */
#include "sentence.h"

#include "addrs.h"
#include <stddef.h>

#include "routines.h"

static void hl_inc(Cpu *c) { set_hl(c, get_hl(c) + 1); }
static void hl_dec(Cpu *c) { set_hl(c, get_hl(c) - 1); }

#define TOKEN_PTR 0xB6DC    /* next token */
#define PREV_TOKEN 0xB6DA   /* the token just read (WORD_START reused) */
#define PREV_CLASS 0xB6DE   /* D when the last token was read */
#define IN_QUOTES 0xB71B    /* inside "...", where errors are not reported */
#define VAR_B719 0xB719     /* unknown: parser mode, 1 or 2 */
#define PHRASE_COUNT 0xB706 /* records built */
#define VAR_B705 0xB705     /* unknown: cleared by the end-of-line word */
#define WORD_COUNT 0x757A   /* words stored in the slots at $757B */
#define PHRASE 0x757B       /* 10 bytes: word slots for the current phrase */
#define SAVED_TOKEN 0x7574  /* state saved at a ',' for backtracking */
#define SAVED_FLAGS 0x7576
#define SAVED_RECORD 0x7577
#define SAVED_COUNT 0x7579
#define RECORDS 0xB9C8      /* noun-phrase records, $18 bytes each, built downwards */
#define JUMP_TABLE 0x75D2
#define SPECIAL_WORDS 0x8271 /* 13 words handled by their own routines */
#define MSG_WHAT 0xAD9F      /* "what ?" */
#define PRINTER_ON 0xB6F2    /* copy output to the ZX Printer */
#define QUOTE_CLASS 0x824F   /* D, saved at an opening quote */
#define QUOTE_COUNT 0x8250   /* PHRASE_COUNT, saved at an opening quote */
#define SPEECH 0xB738        /* 8 slots of $19 bytes: records of what was said */
#define SPEECH_COUNT 0xB737
#define R_PRINT_MSG 0x72DD

enum { OK, ABORT };

static uint16_t iy_at(const Cpu *c, int off) { return (uint16_t)(c->iy + off); }
static uint16_t ix_at(const Cpu *c, int off) { return (uint16_t)(c->ix + off); }

/* Stack traffic is kept exactly as the original's (the random number
 * generator reads the stack too). Calling convention here: the caller
 * pushes the return address of the original CALL (CALL site + 3), and each
 * helper does its own RET (ret(c)) where the original returns. */
static void ret(Cpu *c) { c->sp += 2; }
static void call(Cpu *c, uint16_t back) { push16(c, back); }

/* $785F */
static void clear_mode(Cpu *c) {
  c->a = op_xor(c, c->a, c->a);
  c->mem[VAR_B719] = c->a;
  ret(c);
}

/* $7864: clear the record at IY, and the first word of the one below. */
static void clear_record(Cpu *c) {
  push16(c, c->iy);
  set_hl(c, pop16(c));
  c->b = 0x18;
  call(c, 0x786C); /* CALL $70E2 */
  blank(c);
  ret(c);
  c->mem[iy_at(c, -0x18)] = c->b;
  c->mem[iy_at(c, -0x17)] = c->b;
  ret(c);
}

/* $7873: read the next token: class in A and D, offset in B (low nibble
 * of the first byte) and C. */
static void next_token(Cpu *c) {
  set_hl(c, rd16(c, TOKEN_PTR));
  wr16(c, PREV_TOKEN, get_hl(c));
  c->a = c->d;
  c->mem[PREV_CLASS] = c->a;
  c->a = op_and(c, c->mem[get_hl(c)], 0x0F);
  c->b = c->a;
  c->a = op_and(c, c->mem[get_hl(c)], 0xF0);
  c->d = c->a;
  hl_inc(c);
  c->c = c->mem[get_hl(c)];
  hl_inc(c);
  wr16(c, TOKEN_PTR, get_hl(c));
  ret(c);
}

/* $789F/$78A5: IX = the nearest record below (step -$18) or above (+$18)
 * IY whose second byte does not have bit 6 set. */
static void find_record(Cpu *c, uint16_t step) {
  push16(c, get_de(c));
  set_de(c, step);
  push16(c, c->iy);
  c->ix = pop16(c);
  do c->ix = op_add16(c, c->ix, get_de(c));
  while (op_bit_at(c, 6, c->mem[ix_at(c, 1)], ix_at(c, 1)));
  set_de(c, pop16(c));
  ret(c);
}

/* $7896: swap IX and IY (through the stack), and return. */
static void swap_ix_iy(Cpu *c) {
  push16(c, c->ix);
  push16(c, c->iy);
  c->ix = pop16(c);
  c->iy = pop16(c);
  ret(c);
}

/* $7893, $7892 (counting down B), $788C (B, upwards). */
static void record_down(Cpu *c) {
  call(c, 0x7896); /* CALL $789F */
  find_record(c, 0xFFE8);
  swap_ix_iy(c);
}
static void record_down_count(Cpu *c) {
  c->b = op_dec(c, c->b);
  record_down(c);
}
static void record_up_count(Cpu *c) {
  c->b = op_dec(c, c->b);
  call(c, 0x7890); /* CALL $78A5 */
  find_record(c, 0x0018);
  swap_ix_iy(c);
}

/* $7858: move up to the next record, and clear the mode. */
static void record_up(Cpu *c) {
  call(c, 0x785B); /* CALL $78A5 */
  find_record(c, 0x0018);
  push16(c, c->ix);
  c->iy = pop16(c);
  clear_mode(c);
}

/* $7918: store BC at IY+L. */
static void store_bc(Cpu *c) {
  push16(c, get_de(c));
  push16(c, c->iy);
  set_de(c, pop16(c));
  c->h = 0;
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  c->mem[get_hl(c)] = c->c;
  hl_inc(c);
  c->mem[get_hl(c)] = c->b;
  set_de(c, pop16(c));
  ret(c);
}

/* $7914 */
static void store_bc_first(Cpu *c) {
  c->e &= ~0x02;
  c->l = 0;
  store_bc(c);
}

/* $75F1: NZ if inside quotes. */
static bool in_quotes(Cpu *c) {
  c->a = c->mem[IN_QUOTES];
  c->a = op_and(c, c->a, c->a);
  ret(c);
  return !c->zf;
}

/* $792F: print "what ?" on the input line; the parser returns NZ (the
 * caller does the RET). */
static void what(Cpu *c) {
  set_hl(c, MSG_WHAT);
  c->a = 1;
  c->mem[PRINT_TO_INPUT] = c->a;
  cpu_call_at(c, 0x7937);
  c->a = op_or(c, c->a, 0x01);
}

/* $7924: inside quotes, carry on (RET NZ); otherwise pop the return
 * address and say "what ?" ($7929, $792F): the parse is abandoned. Either
 * way the address on top of the stack is used up. (From $782B the extra
 * level unwinds through $7805, which the caller handles.) */
static int fail(Cpu *c) {
  call(c, 0x7927);
  if (in_quotes(c)) {
    ret(c);
    return OK;
  }
  set_hl(c, pop16(c));
  call(c, 0x792C);
  in_quotes(c);
  what(c);
  return ABORT;
}

/* $780C: store BC in the first free one of the two word slots at HL. Both
 * full: JP $7924 (which returns from $7809/$781C without storing). */
static int store_slot(Cpu *c) {
  c->a = c->mem[get_hl(c)];
  hl_inc(c);
  c->a = op_or(c, c->a, c->mem[get_hl(c)]);
  if (!c->zf) {
    hl_inc(c);
    c->a = c->mem[get_hl(c)];
    hl_inc(c);
    c->a = op_or(c, c->a, c->mem[get_hl(c)]);
    if (!c->zf) return fail(c);
  }
  c->mem[get_hl(c)] = c->b;
  hl_dec(c);
  c->mem[get_hl(c)] = c->c;
  ret(c);
  return OK;
}

/* $7809 */
static int store_7581(Cpu *c) {
  set_hl(c, 0x7581);
  return store_slot(c);
}

/* $781C: count the word, at most 3. */
static int store_word(Cpu *c) {
  set_hl(c, WORD_COUNT);
  c->mem[WORD_COUNT] = op_inc(c, c->mem[WORD_COUNT]);
  c->a = c->mem[WORD_COUNT];
  op_cp(c, 0x03);
  if (!c->cf) return fail(c);
  set_hl(c, PHRASE);
  return store_slot(c);
}

/* $783E: copy the phrase buffer into the record at IY+DE (DE pushed by
 * the caller, $7838/$7850). */
static void copy_phrase_tail(Cpu *c) {
  push16(c, get_bc(c));
  push16(c, c->iy);
  set_hl(c, pop16(c));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  uint16_t t = get_de(c);
  set_de(c, get_hl(c));
  set_hl(c, t);
  set_hl(c, PHRASE);
  set_bc(c, 0x000A);
  ldir(c);
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  c->a = op_xor(c, c->a, c->a);
  ret(c);
}

/* $7838 (into the record's +$0E) and $7850 (+$04). */
static void copy_phrase_0e(Cpu *c) {
  c->e &= ~0x80;
  push16(c, get_de(c));
  set_de(c, 0x000E);
  copy_phrase_tail(c);
}
static void copy_phrase_04(Cpu *c) {
  c->e &= ~0x40;
  push16(c, get_de(c));
  set_de(c, 0x0004);
  copy_phrase_tail(c);
}

/* $782B: finish the phrase into the record. Z to carry on; on ABORT the
 * caller's return address ($7805) is still on the stack. */
static int finish_phrase(Cpu *c) {
  if (op_bit(c, 6, c->e)) {
    copy_phrase_04(c);
    return OK;
  }
  if (op_bit(c, 7, c->e)) {
    copy_phrase_0e(c);
    return OK;
  }
  call(c, 0x7836); /* CALL $7924 */
  if (fail(c) == ABORT) return ABORT;
  c->a = op_xor(c, c->a, c->a);
  ret(c);
  return OK;
}

/* $7905: copy C bytes from IX+DE to IY+DE (on the current register set). */
static void copy_ix_iy(Cpu *c) {
  push16(c, c->iy);
  set_hl(c, pop16(c));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  push16(c, get_hl(c));
  push16(c, c->ix);
  set_hl(c, pop16(c));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  set_de(c, pop16(c));
  c->b = 0;
  ldir(c);
  ret(c);
}

/* $78EA: with the alternate registers in, copy fields of the record at IX
 * into the one at IY: 10 bytes from DE, and the word at +2 if IY has none.
 * Ends with EXX; RET. */
static void copy_fields(Cpu *c) {
  call(c, 0x78ED); /* CALL $78FF */
  c->c = 0x0A;
  copy_ix_iy(c);
  c->a = op_or(c, c->mem[iy_at(c, 2)], c->mem[iy_at(c, 3)]);
  set_de(c, 2);
  if (c->zf) {
    call(c, 0x78F9); /* CALL Z,$7903 */
    c->c = 2;
    copy_ix_iy(c);
  }
  exx(c);
  ret(c);
}

/* $78B7: merge the record at IX into the one at IY. */
static void merge_into(Cpu *c) {
  exx(c);
  c->a = op_and(c, c->mem[ix_at(c, 1)], 0x7F);
  c->a = op_or(c, c->a, c->mem[iy_at(c, 1)]);
  c->mem[iy_at(c, 1)] = c->a;
  c->a = c->mem[c->ix];
  c->mem[c->iy] = c->a;
  exx(c);
  c->a = op_or(c, c->mem[iy_at(c, 0x1C)], c->mem[iy_at(c, 0x1D)]);
  if (c->zf) {
    ret(c);
    return;
  }
  if (!op_bit(c, 7, c->e)) {
    ret(c);
    return;
  }
  exx(c);
  push16(c, c->iy);
  set_hl(c, pop16(c));
  set_de(c, 0x0012);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  push16(c, get_hl(c));
  set_de(c, 0xFFF6);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  set_de(c, pop16(c));
  set_bc(c, 6);
  ldir(c);
  set_de(c, 4);
  copy_fields(c);
}

/* $7638: when $B71A is set (A), fill in empty words of the record above
 * ($B9B8 or $B9C2) from $B9C8+$B71A. */
static void fill_from_previous(Cpu *c) {
  push16(c, get_de(c));
  c->e = c->a;
  c->d = 0;
  set_hl(c, RECORDS);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  push16(c, c->iy);
  c->iy = 0xB9B0;
  c->a = op_or(c, c->mem[iy_at(c, 8)], c->mem[iy_at(c, 9)]);
  set_de(c, 0xB9B8);
  if (!c->zf) {
    c->a = c->mem[iy_at(c, 8)];
    op_cp(c, c->mem[get_hl(c)]);
    if (c->zf) {
      c->a = c->mem[iy_at(c, 9)];
      op_cp(c, c->mem[(uint16_t)(get_hl(c) + 1)]);
    }
    if (!c->zf) {
      c->a = op_or(c, c->mem[iy_at(c, 0x12)], c->mem[iy_at(c, 0x13)]);
      set_de(c, 0xB9C2);
    }
  }
  c->iy = pop16(c);
  if (c->zf) {
    uint16_t t = get_de(c);
    set_de(c, get_hl(c));
    set_hl(c, t);
    c->b = 3;
    do {
      c->a = c->mem[get_hl(c)];
      c->a = op_or(c, c->a, c->mem[(uint16_t)(get_hl(c) + 1)]);
      bool empty = c->zf;
      if (empty) c->mem[get_hl(c)] = c->a = c->mem[get_de(c)];
      hl_inc(c), set_de(c, get_de(c) + 1);
      if (empty) c->mem[get_hl(c)] = c->a = c->mem[get_de(c)];
      hl_inc(c), set_de(c, get_de(c) + 1);
    } while (--c->b);
  }
  set_de(c, pop16(c));
}

/* $7585: parse the token list at TOKEN_PTR. Returns Z when the sentence is
 * done (or NZ with "what ?" printed); see the original's callers. */
static void parse(Cpu *c, uint16_t entry) {
  switch (entry) {
  case 0x82B3: goto L82B3;
  case 0x75B4: goto L75B4;
  default: break;
  }
  c->iy = RECORDS;
  c->a = op_xor(c, c->a, c->a);
L758A: /* also the start of a quoted sentence, with A=1 */
  c->mem[IN_QUOTES] = c->a;
  call(c, 0x7590);
  clear_mode(c);
  c->mem[PHRASE_COUNT] = c->a;
  c->e = 0xFF;
  c->a = c->mem[VAR_B71A];
  c->a = op_and(c, c->a, c->a);
  c->d = 0xA0;
  if (!c->zf) goto L7614;
  c->d = 0xC0;

L75A0: /* a new phrase */
  c->a = op_xor(c, c->a, c->a);
  c->mem[WORD_COUNT] = c->a;
  c->a = op_or(c, c->e, 0xC7);
  c->e = c->a;
  c->a = c->mem[VAR_B719];
  op_cp(c, 0x02);
  if (c->zf) c->e &= ~0x02;
  call(c, 0x75B4);
  clear_record(c);

L75B4:
  set_hl(c, PHRASE);
  c->b = 0x0A;
  call(c, 0x75BC); /* CALL $70E2 */
  blank(c);
  ret(c);
  c->e |= 0x10;

L75BE: /* dispatch on the next word's class */
  call(c, 0x75C1);
  next_token(c);
  {
    push16(c, get_de(c));
    op_rrca(c);
    op_rrca(c);
    op_rrca(c);
    c->e = c->a;
    c->d = 0;
    set_hl(c, JUMP_TABLE);
    set_hl(c, op_add16(c, get_hl(c), get_de(c)));
    c->e = c->mem[get_hl(c)];
    hl_inc(c);
    c->d = c->mem[get_hl(c)];
    uint16_t handler = get_de(c);
    set_de(c, get_hl(c));
    set_hl(c, handler);
    set_de(c, pop16(c));
    switch (handler) {
    case 0x76F2: goto L76F2;
    case 0x7795: goto L7795;
    case 0x76EC: goto L76EC;
    case 0x7733: goto L7733;
    case 0x772F: goto L772F;
    case 0x77D1: goto L77D1;
    case 0x77C9: goto L77C9;
    case 0x77A2: goto L77A2;
    case 0x7790: goto L7790;
    case 0x8251: goto L8251;
    case 0x770B: goto L770B;
    case 0x75FA: goto L75FA;
    case 0x75F6: goto L75F6;
    default: cpu_tail(c, handler); return;
    }
  }

L76EC: /* class $20 */
  if (op_bit(c, 1, c->e)) goto L7731;
  c->d = 0;
L76F2: /* class $00 */
  if (!op_bit(c, 2, c->e)) goto L7929;
  c->a = c->mem[VAR_B719];
  op_cp(c, 0x02);
  if (c->zf) {
    call(c, 0x7701);
    record_up(c);
  }
  c->e &= ~0x04;
  c->l = 2;
  call(c, 0x7708);
  store_bc(c);
  goto L75B4;

L770B: /* class $A0: ',' - remember where we are, to come back to */
  c->e &= ~0x08;
  push16(c, get_de(c));
  do {
    call(c, 0x7711);
    next_token(c);
    op_cp(c, 0xA0);
  } while (c->zf);
  hl_dec(c);
  hl_dec(c);
  wr16(c, SAVED_TOKEN, get_hl(c));
  wr16(c, TOKEN_PTR, get_hl(c));
  set_de(c, pop16(c));
  c->a = c->e;
  c->mem[SAVED_FLAGS] = c->a;
  wr16(c, SAVED_RECORD, c->iy);
  c->a = c->mem[PHRASE_COUNT];
  c->mem[SAVED_COUNT] = c->a;
  goto L75FA;

L772F: /* class $40 */
  c->e &= ~0x01;
L7731:
  c->d = 0x30;
L7733: /* class $30 */
  if (op_bit(c, 3, c->e)) goto L7755;
  call(c, 0x773A);
  if (in_quotes(c)) goto L7755;
  set_hl(c, rd16(c, SAVED_TOKEN));
  wr16(c, TOKEN_PTR, get_hl(c));
  c->d = 0xB0;
  c->a = c->mem[SAVED_FLAGS];
  c->e = c->a;
  c->a = c->mem[SAVED_COUNT];
  c->mem[PHRASE_COUNT] = c->a;
  c->iy = rd16(c, SAVED_RECORD);
  goto L75FA;
L7755:
  call(c, 0x7758);
  clear_mode(c);
  if (!op_bit(c, 1, c->e)) goto L7929;
  if (!op_bit(c, 0, c->e)) goto L7767;
L7761:
  call(c, 0x7764);
  store_bc_first(c);
  goto L75B4;
L7767:
  push16(c, get_de(c));
  push16(c, get_bc(c));
  set_hl(c, rd16(c, TOKEN_PTR));
  push16(c, get_hl(c));
  do {
    call(c, 0x7770);
    next_token(c);
    op_cp(c, 0x00);
  } while (c->zf);
  c->e |= 0x01;
  op_cp(c, 0x20);
  if (!c->zf) {
    set_hl(c, pop16(c));
    wr16(c, TOKEN_PTR, get_hl(c));
    set_bc(c, pop16(c));
    set_de(c, pop16(c));
    goto L7761;
  }
  call(c, 0x7785);
  store_bc_first(c);
  set_hl(c, pop16(c));
  set_bc(c, pop16(c));
  set_hl(c, pop16(c)); /* the saved DE, into HL */
  c->l = 2;
  call(c, 0x778D);
  store_bc(c);
  goto L75B4;

L7790: /* class $80 */
  c->e &= ~0x10;
  goto L75BE;

L7795: /* class $10 */
  if (op_bit(c, 1, c->e)) {
    c->a = c->mem[PREV_CLASS];
    op_cp(c, 0xA0);
    if (c->zf) goto L7731;
  }
  c->d = 0x70;
L77A2: /* class $70 */
  call(c, 0x77A5);
  if (store_word(c) == ABORT) return;
L77A5:
  call(c, 0x77A8);
  next_token(c);
  op_cp(c, 0x70);
  if (c->zf) goto L77A2;
  op_cp(c, 0x80);
  if (c->zf) {
    if (!op_bit(c, 4, c->e)) {
      call(c, 0x77B5); /* CALL Z,$7924 */
      if (fail(c) == ABORT) return;
    }
    c->e &= ~0x10;
    goto L77A5;
  }
L77B9:
  op_cp(c, 0x60);
  if (c->zf) goto L77C9;
  op_cp(c, 0x50);
  if (c->zf) goto L77D1;
  set_hl(c, rd16(c, PREV_TOKEN)); /* put the word back */
  wr16(c, TOKEN_PTR, get_hl(c));
  goto L77D7;

L77C9: /* class $60 */
  call(c, 0x77CC);
  if (store_7581(c) == ABORT) return;
  call(c, 0x77CF);
  next_token(c);
  goto L77B9;

L77D1: /* class $50 */
  set_hl(c, 0x757F);
  c->mem[get_hl(c)] = c->c;
  hl_inc(c);
  c->mem[get_hl(c)] = c->b;
L77D7:
  c->d = 0x50;
  c->a = c->mem[VAR_B719];
  op_cp(c, 0x02);
  if (!c->zf) goto L7802;
  set_hl(c, rd16(c, PHRASE));
  c->a = c->h;
  c->a = op_or(c, c->a, c->l);
  if (c->zf) {
    if (!op_bit_at(c, 6, c->mem[iy_at(c, 0x19)], iy_at(c, 0x19))) {
      set_bc(c, 0xFFE8);
      c->iy = op_add16(c, c->iy, get_bc(c));
      call(c, 0x77F5);
      clear_record(c);
    }
    call(c, 0x77F8);
    copy_phrase_04(c);
    c->mem[iy_at(c, 1)] = 0x40;
    goto L75B4;
  }
  call(c, 0x7802);
  record_up(c);
L7802:
  call(c, 0x7805);
  if (finish_phrase(c) == ABORT) {
    ret(c); /* the RET of "what ?" returns to $7805, which returns (NZ) */
    return;
  }
  if (c->zf) goto L75B4;
  return;

L75F6: /* class $C0: end of the line */
  c->a = op_xor(c, c->a, c->a);
  c->mem[VAR_B705] = c->a;
L75FA: /* class $B0: '.' - the end of a sentence */
  if (!op_bit(c, 1, c->e)) goto L7614;
  c->a = c->mem[PHRASE_COUNT];
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) goto L7614;
  c->a = c->mem[PREV_CLASS];
  op_cp(c, 0xC0);
  if (c->zf) {
    op_cp(c, c->d);
    if (c->zf) { /* $75EC */
      c->a = c->mem[IN_QUOTES];
      c->a = op_dec(c, c->a);
      return;
    }
  }
  call(c, 0x7611);
  if (!in_quotes(c)) {
    what(c);
    return;
  }
L7614:
  c->a = c->mem[VAR_B719];
  c->a = op_dec(c, c->a);
  if (c->zf) c->mem[iy_at(c, 1)] |= 0x80;
  set_hl(c, PHRASE_COUNT);
  c->a = op_dec(c, c->a);
  {
    bool nz = !c->zf;
    c->a = c->d;
    if (nz) goto L7629;
    op_cp(c, 0xA0);
    if (c->zf) goto L762A;
  }
L7629:
  c->mem[PHRASE_COUNT] = op_inc(c, c->mem[PHRASE_COUNT]);
L762A:
  call(c, 0x762D);
  record_down(c);
  op_cp(c, 0xA0);
  if (c->zf) goto L75A0;
  c->a = c->mem[VAR_B71A];
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) fill_from_previous(c);
  /* $7682: merge each record into the ones it qualifies */
  push16(c, c->iy);
  c->iy = 0xB9E0;
  c->a = c->mem[PHRASE_COUNT];
  c->b = c->a;
  push16(c, get_bc(c));
  call(c, 0x7690);
  record_down_count(c);
  for (;;) {
    c->a = c->b;
    c->a = op_and(c, c->a, c->a);
    if (c->zf) break;
    call(c, 0x7697);
    record_down_count(c);
    c->a = c->mem[iy_at(c, 1)];
    c->a = op_and(c, c->a, 0x7F);
    c->a = op_or(c, c->a, c->mem[c->iy]);
    if (c->zf) {
      call(c, 0x76A2); /* CALL Z,$78B7 */
      merge_into(c);
    }
  }
  set_bc(c, pop16(c));
  c->iy = pop16(c);
  push16(c, c->iy);
  call(c, 0x76AC);
  record_up_count(c);
  for (;;) {
    c->a = c->b;
    c->a = op_and(c, c->a, c->a);
    if (c->zf) break;
    call(c, 0x76B4);
    record_up_count(c);
    c->a = c->mem[iy_at(c, 0x12)];
    c->a = op_or(c, c->a, c->mem[iy_at(c, 0x13)]);
    if (!c->zf) continue;
    c->a = c->mem[c->iy];
    op_cp(c, c->mem[c->ix]);
    if (!c->zf) continue;
    c->a = c->mem[iy_at(c, 1)];
    op_cp(c, c->mem[ix_at(c, 1)]);
    if (!c->zf) continue;
    c->a = c->mem[ix_at(c, 0x0E)];
    c->a = op_or(c, c->a, c->mem[ix_at(c, 0x0F)]);
    if (c->zf) continue;
    exx(c);
    set_de(c, 0x000E);
    call(c, 0x76DB);
    copy_fields(c);
  }
  c->iy = pop16(c);
  c->a = c->mem[VAR_B705];
  c->a = op_and(c, c->a, c->a);
  if (c->zf) return;
  call(c, 0x76E8);
  if (!in_quotes(c)) return;
  goto L75A0;

L7929:
  call(c, 0x792C);
  if (in_quotes(c)) goto L75B4;
  what(c);
  return;

L8251: /* class $90: one of the special words, each with its own routine */
  set_hl(c, SPECIAL_WORDS);
  push16(c, get_de(c));
  c->d = 0x0D;
  for (;;) {
    c->a = c->mem[get_hl(c)];
    hl_inc(c);
    op_cp(c, c->c);
    if (c->zf) {
      c->a = c->mem[get_hl(c)];
      op_cp(c, c->b);
      if (c->zf) break;
    }
    hl_inc(c);
    c->d = op_dec(c, c->d);
    if (c->zf) {
      /* Not found: the original jumps back leaving DE on the stack, so
       * the parser's final RET would return to it. */
      goto L75A0;
    }
  }
  set_de(c, 0x0019);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  c->e = c->mem[get_hl(c)];
  hl_inc(c);
  c->d = c->mem[get_hl(c)];
  {
    uint16_t handler = get_de(c);
    set_de(c, get_hl(c));
    set_hl(c, handler);
    set_de(c, pop16(c));
    switch (handler) {
    case 0x8315: goto L8315;
    case 0x82D2: goto L82D2;
    case 0x82BA: goto L82BA;
    case 0x82E2: goto L82E2;
    case 0x75B4: goto L75B4; /* ONE */
    case 0x82A5: goto L82A5;
    case 0x82AF: goto L82AF;
    default:
      /* SAVE, LOAD, QUIT, HELP, SCORE, PAUSE: commands of their own, which
       * come back to $82B3 to carry on with the sentence (or restart the
       * game). */
      if (!cpu_jump(c, handler, 0x82B3)) return;
      goto L82B3;
    }
  }

L82A5: /* PRINT: copy output to the ZX Printer, if there is one */
  c->a = c->in(c, (uint16_t)(c->a << 8 | 0xFB));
  if (op_bit(c, 6, c->a)) goto L82B3;
  c->a = 1;
  goto L82B0;
L82AF: /* NOPRINT */
  c->a = op_xor(c, c->a, c->a);
L82B0:
  c->mem[PRINTER_ON] = c->a;
L82B3: /* carry on after a special word */
  c->a = c->mem[PREV_CLASS];
  c->d = c->a;
  goto L75B4;

L82BA: /* EXCEPT, after ALL */
  c->a = c->mem[VAR_B719];
  op_cp(c, 0x01);
  if (!c->zf) goto L7929;
  c->a = 2;
  c->mem[VAR_B719] = c->a;
  c->a = c->mem[iy_at(c, 1)];
  c->a = op_or(c, c->a, 0x80);
  c->mem[iy_at(c, 1)] = c->a;
  goto L75B4;

L82D2: /* ALL */
  c->a = c->mem[VAR_B719];
  op_cp(c, 0x02);
  if (c->zf) goto L75B4;
  c->a = 1;
  c->mem[VAR_B719] = c->a;
  goto L75B4;

L82E2: /* IT: the last thing mentioned ($B6E0) */
  push16(c, get_de(c));
  set_hl(c, 0xB6E0);
  set_de(c, 0x757F);
  set_bc(c, 6);
  ldir(c);
  set_de(c, pop16(c));
  call(c, 0x82F4); /* CALL $82F7: JP Z,$7838 / JP $7850 */
  if (!op_bit(c, 7, c->e))
    copy_phrase_0e(c);
  else
    copy_phrase_04(c);
  goto L75B4;

L8315: /* '"': open or close a quoted sentence */
  c->a = c->mem[IN_QUOTES];
  c->a = op_and(c, c->a, c->a);
  if (c->zf) goto L82FD;
  c->a = op_dec(c, c->a);
  c->mem[IN_QUOTES] = c->a;
  c->a = c->mem[QUOTE_COUNT];
  c->b = c->a;
  c->a = c->mem[PHRASE_COUNT];
  c->a = op_sub(c, c->a, c->b, 0);
  c->a = op_and(c, c->a, c->a);
  c->c = 0;
  if (c->zf) goto L8378;
  c->iy = pop16(c);
  push16(c, c->iy);
  set_de(c, 0xFFE8);
  c->iy = op_add16(c, c->iy, get_de(c));
  c->b = c->a;
L8336: /* copy each quoted record into a free speech slot */
  c->ix = SPEECH;
  push16(c, get_de(c));
  push16(c, get_bc(c));
  set_de(c, 0x0019);
  c->b = 8;
  for (;;) {
    c->a = c->mem[c->ix];
    c->a = op_and(c, c->a, c->a);
    if (c->zf) break;
    c->ix = op_add16(c, c->ix, get_de(c));
    if (!--c->b) break;
  }
  c->mem[c->ix] = 0xFF;
  c->ix++;
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  if (!c->zf) goto L8378;
  if (op_bit_at(c, 6, c->mem[iy_at(c, 1)], iy_at(c, 1))) {
    c->mem[ix_at(c, -1)] = 0;
    goto L8373;
  }
  push16(c, get_bc(c));
  c->b = 0x18;
  do {
    c->a = c->mem[c->iy];
    c->mem[c->ix] = c->a;
    c->iy++;
    c->ix++;
  } while (--c->b);
  set_bc(c, pop16(c));
  c->iy = op_add16(c, c->iy, get_de(c));
L8373:
  c->iy = op_add16(c, c->iy, get_de(c));
  c->c = op_inc(c, c->c);
  if (--c->b) goto L8336;
L8378:
  c->a = op_xor(c, c->a, c->a);
  c->mem[VAR_B719] = c->a;
  c->a = c->c;
  c->mem[SPEECH_COUNT] = c->a;
  c->iy = pop16(c);
  c->a = c->mem[QUOTE_COUNT];
  c->mem[PHRASE_COUNT] = c->a;
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  c->a = c->mem[QUOTE_CLASS];
  c->d = c->a;
  goto L75B4;

L82FD: /* opening quote: parse the quoted sentence into the records below,
        * keeping this sentence's state on the stack */
  c->a = c->d;
  c->mem[QUOTE_CLASS] = c->a;
  push16(c, get_de(c));
  push16(c, get_bc(c));
  push16(c, c->iy);
  c->a = c->mem[PHRASE_COUNT];
  c->mem[QUOTE_COUNT] = c->a;
  set_de(c, 0xFFE8);
  c->iy = op_add16(c, c->iy, get_de(c));
  c->a = 1;
  goto L758A;
}

void p_parse_sentence(Cpu *c) { parse(c, 0x7585); }

/* $82B3 and $75B4: carrying on with a sentence, entered by the routines
 * of the special words (SAVE, HELP...) when they are done, with the
 * parser's return address (and any quoted sentence's state) on the stack
 * and the parser's registers as they left them. */
static void p_parse_82b3(Cpu *c) { parse(c, 0x82B3); }
static void p_parse_75b4(Cpu *c) { parse(c, 0x75B4); }

/* $792F: print "what ?" on the input line, and return NZ (also CALLed by
 * the main loop for an unclosed quote). */
static void p_what(Cpu *c) { what(c); }

const PortRoutine sentence_routines[] = {
    {0x7585, "ParseSentence", p_parse_sentence, OUT_REGS | OUT_ALT | OUT_ZF},
    {0x82B3, "ParseResume", p_parse_82b3, OUT_REGS | OUT_ALT | OUT_ZF},
    {0x75B4, "ParsePhrase", p_parse_75b4, OUT_REGS | OUT_ALT | OUT_ZF},
    {0x792F, "What", p_what, OUT_REGS | OUT_ZF | OUT_CF},
    {0, NULL, NULL, 0},
};
