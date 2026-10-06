/* The front end of the parser: reading the input line and splitting it
 * into dictionary words. Translated from $6DD6-$6FD2. */
#include "parser.h"

#include <stddef.h>

#include "addrs.h"
#include "routines.h"

/* LD A,ch then the CALL $858B at site. */
static void print(Cpu *c, uint8_t ch, uint16_t site) {
  c->a = ch;
  cpu_call_at(c, site);
}

static void hl_inc(Cpu *c) { set_hl(c, get_hl(c) + 1); }
static void hl_dec(Cpu *c) { set_hl(c, get_hl(c) - 1); }

/* ---------- reading the input line ---------- */

/* $6E8B: delete everything typed so far. B counts the free space in the
 * buffer down from $80, HL is the end of the text. */
void p_clear_line(Cpu *c) {
  while (!(c->b & 0x80)) {
    print(c, 0x08, 0x6E90);
    c->b++;
    hl_dec(c);
  }
  c->zf = 0; /* BIT 7,B */
}

/* $6E4F: as the first key of a line, the cursor keys type a direction and
 * ENTER: 8 (right) E, 5 or 0 (left) W, 6 (down) S, 7 (up) N. Leaves A=$0D
 * if so, else A unchanged. */
static void direction_key(Cpu *c) {
  uint8_t letter;
  switch (c->a) {
  case 0x09: letter = 'E'; break;
  case 0x08: letter = 'W'; break;
  case 0x0A: letter = 'S'; break;
  case 0x5B: letter = 'N'; break;
  default: return;
  }
  c->a = letter;
  c->mem[get_hl(c)] = c->a;
  hl_inc(c);
  cpu_call_at(c, 0x6E62);
  c->b--;
  c->a = 0x0D;
  c->mem[get_hl(c)] = c->a;
  hl_inc(c);
  c->b--;
  c->a |= 1;
}

static bool accepted(uint8_t ch) {
  return ch >= 0x40 || ch == '"' || ch == ' ' || ch == 0x0D || ch == '.' || ch == ',';
}

/* $6DD6: print the prompt and read a command into INPUT_BUFFER, echoing
 * it on the input line. Returns NZ when ENTER is pressed, or Z (A=0) when
 * '@' on an empty line asks to repeat the last command. */
void p_read_line(Cpu *c) {
  wr16(c, IDLE_TIMER, 0x0BB8);
  c->a = 1;
  c->mem[PRINT_TO_INPUT] = c->a;
  c->mem[VAR_B6FA] = c->a;
  print(c, '>', 0x6DE6);
  print(c, ' ', 0x6DEB);
  set_hl(c, INPUT_BUFFER);
  c->b = 0x80;
new_line:
  c->c = 0; /* no key yet */
  for (;;) {
    cpu_call_at(c, 0x6DF5);
    if ((c->b & 0x80) && c->a == '@') {
      c->a = c->mem[VAR_B71A];
      if (c->a) continue;
      print(c, 0x08, 0x6E83);
      print(c, 0x08, 0x6E86);
      c->a = 0;
      flags_logic(c, false);
      return;
    }
    if (!(c->c & 1)) direction_key(c);
    c->c = 1;
    if (c->a == 0x18) { /* CAPS SHIFT+0 */
      p_clear_line(c);
      goto new_line;
    }
    if (c->a == 0x08) { /* delete */
      if (c->b & 0x80) continue;
      print(c, 0x08, 0x6E1B);
      c->b++;
      hl_dec(c);
      continue;
    }
    if (!accepted(c->a)) continue;
    c->mem[LAST_KEY] = c->a;
    if (c->b != 0) { /* room left */
      cpu_call_at(c, 0x6E41);
      c->mem[get_hl(c)] = c->a;
      hl_inc(c);
      c->b--;
    }
    if (c->a == 0x0D) {
      c->a |= 1;
      flags_logic(c, false);
      return;
    }
  }
}

/* ---------- dictionary lookup ---------- */

/* Dictionary entries are runs of bytes holding 5-bit letter codes (0 for
 * none) in bits 0-4. The entry ends at a byte with bit 7 set, except that
 * it always runs to at least 3 bytes, and a third byte with bit 7 set only
 * ends it if the second did not have bit 7 set as well. Bit 6 of the last
 * byte means a 2-byte link to the word's real entry follows (a synonym).
 * Bits 5-6 of the first two bytes give the word's class. */

/* The tokenizer below keeps the original's stack traffic exactly (every
 * PUSH and CALL return address lands where the original puts it), and
 * the registers that get pushed: the random number generator reads the
 * stack too. A CALL to a helper here is push16(c, return address), the
 * helper, then c->sp += 2. */
static void ret(Cpu *c) { c->sp += 2; }

/* $6F76: decode the entry at IX into DICT_CODES/DICT_LEN, leaving IX after
 * it (and its link). Returns false (NZ) without decoding if its first
 * letter is not the word's. */
static bool decode_entry(Cpu *c) {
  c->a = op_and(c, c->mem[c->ix], 0x1F);
  c->b = c->a;
  c->a = c->mem[WORD_CODES];
  op_cp(c, c->b);
  if (!c->zf) return false;
  push16(c, get_hl(c));
  set_hl(c, DICT_CODES);
  set_bc(c, 0);
  for (;;) {
    c->a = op_and(c, c->mem[c->ix], 0x1F);
    if (!c->zf) {
      c->mem[get_hl(c)] = c->a;
      hl_inc(c);
      c->b++;
    }
    c->ix++;
    c->c++;
    if (!op_bit_at(c, 7, c->mem[(uint16_t)(c->ix - 1)], (uint16_t)(c->ix - 1))) continue;
    c->a = c->c;
    op_cp(c, 0x02);
    if (c->zf) continue;
    op_cp(c, 0x03);
    if (c->zf && op_bit_at(c, 7, c->mem[(uint16_t)(c->ix - 2)], (uint16_t)(c->ix - 2))) continue;
    break;
  }
  set_hl(c, pop16(c));
  c->a = c->b;
  c->mem[DICT_LEN] = c->a;
  if (!op_bit_at(c, 6, c->mem[(uint16_t)(c->ix - 1)], (uint16_t)(c->ix - 1))) return true;
  c->ix += 2;
  c->a = op_xor(c, c->a, c->a);
  return true;
}

/* $6F72: make IX the entry being compared, and decode it. */
static bool next_entry(Cpu *c) {
  wr16(c, DICT_ENTRY, c->ix);
  return decode_entry(c);
}

/* $6FBA: does the word match the decoded entry, over the shorter length?
 * Leaves HL and DE at the letters where it stopped. */
static bool compare_word(Cpu *c) {
  c->a = c->mem[WORD_LEN];
  c->b = c->a;
  c->a = c->mem[DICT_LEN];
  op_cp(c, c->b);
  if (c->cf) c->b = c->a;
  set_hl(c, WORD_CODES);
  set_de(c, DICT_CODES);
  do {
    c->a = c->mem[get_de(c)];
    op_cp(c, c->mem[get_hl(c)]);
    if (!c->zf) return false;
    set_de(c, get_de(c) + 1);
    hl_inc(c);
  } while (--c->b);
  return true;
}

/* $6E97: read the next word of the input line at HL.
 *
 * Returns the word's class in A and D, and its dictionary offset (address
 * - $6000) in BC with the class added to B; HL is left after the word and
 * E is preserved. The classes that are not words: $C0 end of line, $B0
 * '.', $A0 ',', $90 '"' (with BC=0), and $D0 for an unknown word. A word
 * matches an entry if one is a prefix of the other, but a word longer than
 * an entry of 4 or more letters only matches if the next entry does not
 * match as well. */
void p_get_word(Cpu *c) {
  push16(c, get_de(c));
  do {
    c->a = c->mem[get_hl(c)];
    hl_inc(c);
    op_cp(c, ' ');
  } while (c->zf);
  hl_dec(c);
  wr16(c, WORD_START, get_hl(c));
  op_cp(c, 0x0D);
  if (c->zf) {
    c->a = 0xC0;
    set_bc(c, 0);
    goto done;
  }

  /* CALL $6F30: punctuation */
  push16(c, 0x6EA9);
  c->b = 0xB0;
  op_cp(c, '.');
  if (!c->zf) {
    c->b = 0xA0;
    op_cp(c, ',');
    if (!c->zf) {
      op_cp(c, '"');
      if (c->zf) c->b = 0x90;
    }
  }
  if (c->zf) {
    hl_inc(c);
    c->a = c->b;
    set_bc(c, 0);
    ret(c);
    goto done;
  }
  ret(c);

  /* CALL $6F47: encode the word, and find the first entry for its letter. */
  push16(c, 0x6EAE);
  set_de(c, WORD_CODES);
  c->b = 0;
  for (;;) {
    c->a = c->mem[get_hl(c)];
    op_cp(c, 0x40);
    if (c->cf) break;
    c->a = op_and(c, c->a, 0x1F);
    c->mem[get_de(c)] = c->a;
    set_de(c, get_de(c) + 1);
    hl_inc(c);
    c->b++;
  }
  c->a = c->b;
  c->mem[WORD_LEN] = c->a;
  push16(c, get_hl(c));
  set_hl(c, rd16(c, WORD_CODES));
  c->h = 0;
  set_de(c, DICT_BASE);
  set_hl(c, op_add16(c, get_hl(c), get_hl(c)));
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  c->e = c->mem[get_hl(c)];
  hl_inc(c);
  c->d = c->mem[get_hl(c)];
  c->ix = op_add16(c, DICT_BASE, get_de(c));
  set_hl(c, pop16(c));
  bool found = next_entry(c);
  ret(c);
  if (!found) goto unknown;

  push16(c, get_hl(c)); /* the input pointer */
  for (;;) {
    push16(c, 0x6EB4); /* CALL $6FBA */
    bool same = compare_word(c);
    ret(c);
    if (same) {
      c->a = c->mem[WORD_LEN];
      c->b = c->a;
      c->a = c->mem[DICT_LEN];
      op_cp(c, c->b);
      if (!c->cf) goto accept;
      op_cp(c, 0x04);
      if (!c->cf) {
        push16(c, c->ix);
        push16(c, 0x6ED3); /* CALL $6F76 */
        bool next_matches = decode_entry(c);
        ret(c);
        if (next_matches) {
          push16(c, 0x6ED8); /* CALL $6FBA */
          next_matches = compare_word(c);
          ret(c);
        }
        c->ix = pop16(c);
        if (!next_matches) goto accept;
      }
    }
    push16(c, 0x6EB9); /* CALL $6F72 */
    found = next_entry(c);
    ret(c);
    if (!found) {
      set_hl(c, pop16(c));
      goto unknown;
    }
  }

accept: /* $6EEB: accept the entry; follow its link if it has one. */
  c->ix = rd16(c, DICT_ENTRY);
  push16(c, c->ix);
  c->a = op_xor(c, c->a, c->a);
  for (;;) {
    c->ix++;
    c->a = op_inc(c, c->a);
    if (!op_bit_at(c, 7, c->mem[(uint16_t)(c->ix - 1)], (uint16_t)(c->ix - 1))) continue;
    op_cp(c, 0x02);
    if (c->zf) continue;
    op_cp(c, 0x03);
    if (c->zf && op_bit_at(c, 7, c->mem[(uint16_t)(c->ix - 2)], (uint16_t)(c->ix - 2))) continue;
    break;
  }
  if (op_bit_at(c, 6, c->mem[(uint16_t)(c->ix - 1)], (uint16_t)(c->ix - 1))) {
    set_hl(c, rd16(c, c->ix));
    set_de(c, DICT_BASE);
    set_hl(c, op_add16(c, get_hl(c), get_de(c)));
    ex_sp_hl(c);
  }
  set_hl(c, pop16(c));
  {
    uint8_t b0 = c->mem[get_hl(c)];
    c->a = b0;
    op_rlca(c);
    c->a = op_and(c, c->a, 0xC0);
    c->b = c->a;
    hl_inc(c);
    c->a = c->mem[get_hl(c)];
    op_rrca(c);
    c->a = op_and(c, c->a, 0x30);
    c->a = op_add(c, c->a, c->b, 0);
    hl_dec(c);
    set_de(c, 0xA000);
    set_hl(c, op_add16(c, get_hl(c), get_de(c)));
    push16(c, get_hl(c));
    set_bc(c, pop16(c));
    set_hl(c, pop16(c)); /* the input pointer */
  }
  goto done;

unknown:
  c->a = 0xD0;
  set_bc(c, 0);

done: /* $6EE3 */
  set_de(c, pop16(c));
  c->d = c->a;
  c->a = op_add(c, c->a, c->b, 0);
  c->b = c->a;
  c->a = c->d;
}

const PortRoutine parser_routines[] = {
    {0x6DD6, "ReadLine", p_read_line, OUT_A | OUT_BC | OUT_HL | OUT_ZF},
    {0x6E8B, "ClearLine", p_clear_line, OUT_A | OUT_B | OUT_HL},
    {0x6E97, "GetWord", p_get_word, OUT_REGS},
    {0, NULL, NULL, 0},
};
