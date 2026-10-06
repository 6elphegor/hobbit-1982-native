/* Screen output and the keyboard: printing to the text window and the
 * input line, scrolling, the fonts, the ZX Printer, GetKey ($8576-$8C4A),
 * and the key waits at $90DF and $969A.
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * The screen: the text window is the top 18 character rows. Text is
 * always printed on character row 17 ($5020) in the game's proportional
 * font (6 pixels a character, 42 to a line); a new line
 * scrolls rows 1-17 up into rows 0-16, pixels and attributes, over
 * whatever picture was there. The input line is printed in the ROM font
 * (8 pixels) on rows 19-23: the cursor runs along row 23 ($50E0) and a
 * full line scrolls rows 20-23 up into 19-22 ($860D); deleting back past
 * the start of a line scrolls rows 18-22 down ($864A).
 *
 * Labels keep the original addresses. */
#include <stddef.h>

#include "cpu.h"
#include "routines.h"

/* Variables. */
#define PRINTER_ON 0xB6F2  /* 1: copy each finished line to the ZX Printer (PRINT) */
#define DOING 0xB6FA       /* 0 while a command is only being tried: print nothing */
#define DRUNK 0xB700       /* after the wine: "s" is printed "sh" */
#define INPUT_LINE 0xB701  /* nonzero: print on the input line, not the text window */
#define OUTPUT_ON 0xB702   /* 0: print nothing */
#define CAPITAL 0xB704     /* nonzero: capitalise the next letter (set by CR and ".") */
#define MORE_LINES 0xB716  /* lines to scroll before waiting for a key (or time) */
#define IN_COLS 0x85B3     /* input line: columns left on the row */
#define IN_POS 0x85B4      /* input line: screen address of the cursor */
#define IN_CURSOR 0x85B6   /* input line: the cursor character ("+") */
#define TW_LEFT 0x869B     /* text window: characters left on the line */
#define TW_POS 0x869C      /* text window: screen address */
#define TW_BIT 0x869E      /* text window: pixel offset in that byte */
#define TW_INDENT 0x869F   /* text window: spaces to start a line with */
#define TW_LAST 0x86A0     /* text window: last character printed, 0 after a new line */
#define KEY_FOUND 0x8B89   /* GetKey: port and bits of the new key */
#define KEY_STATE 0x8B8B   /* GetKey: the 8 half-rows as last scanned */
#define KEY_MASK 0x8B81    /* GetKey: keys to ignore, by half-row */
#define KEY_MAP1 0x8BFB    /* GetKey: characters by key */
#define KEY_MAP2 0x8C23    /* ... with SYMBOL SHIFT or CAPS SHIFT */
#define ROM_FONT 0x3D00    /* the ROM's CHARSET, from space */
#define PROP_FONT 0x8722   /* the game's font, from character 0 ($8822 = space) */

/* A write as the Z80 does it: the ROM cannot be written. */
static void wb(Cpu *c, uint16_t a, uint8_t v) {
  if (a >= 0x4000) c->mem[a] = v;
}

static void push_bc(Cpu *c) { push16(c, get_bc(c)); }
static void push_de(Cpu *c) { push16(c, get_de(c)); }
static void push_hl(Cpu *c) { push16(c, get_hl(c)); }
static void pop_bc(Cpu *c) { set_bc(c, pop16(c)); }
static void pop_de(Cpu *c) { set_de(c, pop16(c)); }
static void pop_hl(Cpu *c) { set_hl(c, pop16(c)); }
static void ex_de_hl(Cpu *c) {
  uint16_t t = get_de(c);
  set_de(c, get_hl(c));
  set_hl(c, t);
}

/* ---------- the output gate ---------- */

/* $8576: may anything be printed? Z (from DOING AND OUTPUT_ON) if not.
 * A and HL are kept. */
static void p_8576(Cpu *c) {
  push_hl(c);
  c->l = c->a;
  c->a = c->mem[DOING];
  c->h = c->a;
  c->a = c->mem[OUTPUT_ON];
  c->a = op_and(c, c->a, c->h);
  c->a = c->l;
  pop_hl(c);
}

static void input_line(Cpu *c);

/* $858B: print the character in A, if printing is on (Z from $8576 if
 * not): on the input line if INPUT_LINE is set, else in the text window
 * (and after the wine, an "h" after each "s"). A and the flags are kept. */
static void p_858b(Cpu *c) {
  cpu_call_at(c, 0x858B); /* CALL $8576 */
  if (c->zf) return;
  cpu_push_af(c);
  c->a = op_and(c, c->mem[INPUT_LINE], c->mem[INPUT_LINE]);
  if (!c->zf) {
    cpu_pop_af(c); /* $85B7 */
    cpu_at(c, 0x85B8);
    input_line(c);
    return;
  }
  cpu_pop_af(c);
  cpu_call_at(c, 0x8597); /* CALL $86A1 */
  cpu_push_af(c);
  c->a = op_and(c, c->mem[DRUNK], c->mem[DRUNK]);
  if (c->zf) {
    cpu_pop_af(c);
    return;
  }
  cpu_pop_af(c); /* $85A3 */
  op_cp(c, 0x53);
  if (!c->zf) {
    op_cp(c, 0x73);
    if (!c->zf) return;
  }
  cpu_push_af(c); /* $85AB */
  c->a = 0x48;
  cpu_call_at(c, 0x85AE); /* CALL $86A1 */
  cpu_pop_af(c);
}

/* $8583: print a new line. AF is kept. */
static void p_8583(Cpu *c) {
  cpu_push_af(c);
  c->a = 0x0D;
  cpu_call_at(c, 0x8586); /* CALL $858B */
  cpu_pop_af(c);
}

/* ---------- the input line (ROM font) ---------- */

/* $85B8: print A on the input line: CR moves to a new row, 8 (delete)
 * steps back (into the row above if need be), lower case letters are
 * printed in capitals; the cursor follows. All registers and flags are
 * kept. Reached from $858B by falling through (with its return address
 * on top of the stack), and CALLed from $6CFE. */
static void input_line(Cpu *c) {
  push_hl(c);
  cpu_push_af(c);
  set_hl(c, rd16(c, IN_POS));
  op_cp(c, 0x0D);
  if (!c->zf) goto L85C8;
  c->a = 0x20;
  cpu_call_at(c, 0x85C3); /* CALL $867A */
  goto L85DF;
L85C8:
  op_cp(c, 0x08);
  if (c->zf) goto L85F5;
  op_cp(c, 0x61);
  if (c->cf) goto L85D6;
  op_cp(c, 0x7B);
  if (!c->cf) goto L85D6;
  c->a = op_and(c, c->a, 0x5F);
L85D6:
  cpu_call_at(c, 0x85D6); /* CALL $867A */
  c->a = op_dec(c, c->mem[IN_COLS]);
  if (!c->zf) goto L85E6;
L85DF:
  c->l = 0xE0;
  cpu_call_at(c, 0x85E1); /* CALL $860D: scroll the input rows up */
  c->a = 0x20;
L85E6:
  wb(c, IN_COLS, c->a);
  c->a = c->mem[IN_CURSOR];
  wr16(c, IN_POS, get_hl(c));
  cpu_call_at(c, 0x85EF); /* CALL $867A: the cursor */
  cpu_pop_af(c);
  pop_hl(c);
  return;
L85F5:
  c->a = 0x20;
  cpu_call_at(c, 0x85F7); /* CALL $867A: rub out the cursor */
  c->l = op_dec(c, c->l);
  c->l = op_dec(c, c->l);
  c->a = op_inc(c, c->mem[IN_COLS]);
  op_cp(c, 0x21);
  if (!c->zf) goto L85E6;
  c->l = 0xFF;
  cpu_call_at(c, 0x8606); /* CALL $864A: scroll the input rows down */
  c->a = 0x01;
  goto L85E6;
}

static void p_85b8(Cpu *c) { input_line(c); }

/* $860D: scroll character rows 20-23 up into 19-22, and clear row 23
 * with ROM spaces. All registers and flags are kept. */
static void p_860d(Cpu *c) {
  push_hl(c), push_de(c), push_bc(c), cpu_push_af(c);
  set_hl(c, 0x5080);
  set_de(c, 0x5060);
  c->a = 0x04;
  c->b = 0x00;
  do { /* $861B */
    push_hl(c), push_de(c);
    c->c = 0x08;
    do { /* $861F */
      push_hl(c), push_de(c), push_bc(c);
      c->c = 0x20;
      ldir(c);
      pop_bc(c), pop_de(c), pop_hl(c);
      c->h++, c->d++;
      c->c = op_dec(c, c->c);
    } while (!c->zf);
    pop_de(c), pop_hl(c);
    c->c = 0x20;
    set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
    ex_de_hl(c);
    set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
    ex_de_hl(c);
    c->a = op_dec(c, c->a);
  } while (!c->zf);
  c->b = 0x20;
  set_hl(c, 0x50E0);
  c->a = 0x20;
  do cpu_call_at(c, 0x8640); /* CALL $867A */
  while (--c->b);
  cpu_pop_af(c), pop_bc(c), pop_de(c), pop_hl(c);
}

/* $864A: scroll character rows 18-22 down into 19-23 (deleting back past
 * the start of a row). All registers and flags are kept. */
static void p_864a(Cpu *c) {
  push_hl(c), push_de(c), push_bc(c), cpu_push_af(c);
  set_hl(c, 0x50C0);
  set_de(c, 0x50E0);
  c->a = 0x05;
  do { /* $8656 */
    push_hl(c), push_de(c);
    c->b = 0x08;
    do { /* $865A */
      push_hl(c), push_de(c), push_bc(c);
      set_bc(c, 0x0020);
      ldir(c);
      pop_bc(c), pop_de(c), pop_hl(c);
      c->h++, c->d++;
    } while (--c->b);
    pop_de(c), pop_hl(c);
    set_bc(c, 0xFFE0);
    set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
    ex_de_hl(c);
    set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
    ex_de_hl(c);
    c->a = op_dec(c, c->a);
  } while (!c->zf);
  cpu_pop_af(c), pop_bc(c), pop_de(c), pop_hl(c);
}

/* $867A PrintChar: print A in the ROM font at screen address HL, and
 * move HL one column right (INC L: its flags, C kept). */
static void p_867a(Cpu *c) {
  cpu_push_af(c), push_bc(c), push_de(c), push_hl(c);
  c->a = op_sub(c, c->a, 0x20, 0);
  uint16_t de = (uint16_t)(c->a * 8 + ROM_FONT);
  set_de(c, de);
  uint16_t hl = get_hl(c);
  for (int i = 0; i < 8; i++) { /* $868F */
    wb(c, hl, c->mem[de]);
    de++;
    hl = (uint16_t)(hl + 0x100);
  }
  /* the last values of the loop's registers are popped away: A, B, DE */
  pop_hl(c), pop_de(c), pop_bc(c), cpu_pop_af(c);
  c->l = op_inc(c, c->l);
}

/* ---------- the text window (proportional font) ---------- */

/* $8761: step the text position (HL, pixel offset C) back one character
 * (6 pixels). */
static void p_8761(Cpu *c) {
  c->a = op_sub(c, c->c, 0x06, 0);
  c->c = c->a;
  if (!c->cf) return;
  c->a = op_add(c, c->a, 0x08, 0);
  c->c = c->a;
  c->l = op_dec(c, c->l);
}

/* $86A1: print A in the text window, on row 17. A line starts with
 * TW_INDENT spaces; CR (or a full line) copies the line to the printer,
 * waits (after MORE_LINES lines: for a key, or a while), and scrolls the
 * window up a row; 8 deletes; capitals become small letters, except the
 * first letter after a CR or a full stop. All registers and flags are
 * kept. */
static void p_86a1(Cpu *c) {
  push_hl(c), push_bc(c), cpu_push_af(c);
  set_hl(c, rd16(c, TW_POS));
  c->c = c->mem[TW_BIT];
  c->a = op_and(c, c->mem[TW_LAST], c->mem[TW_LAST]);
  if (!c->zf) goto L86C6;
  c->a = op_and(c, c->mem[TW_INDENT], c->mem[TW_INDENT]);
  if (c->zf) goto L86C6;
  c->b = c->a;
  do { /* $86B8 */
    c->a = 0x20;
    cpu_call_at(c, 0x86BA); /* CALL $87C9 */
    c->a = op_dec(c, c->mem[TW_LEFT]);
    wb(c, TW_LEFT, c->a);
  } while (--c->b);
L86C6:
  cpu_pop_af(c); /* POP AF; PUSH AF (the same bytes go back) */
  c->sp -= 2;
  op_cp(c, 0x0D);
  if (!c->zf) goto L870D;
  c->a = 0x01;
  wb(c, CAPITAL, c->a);
L86D1:
  c->a = op_xor(c, c->a, c->a);
  wb(c, TW_LAST, c->a);
  cpu_call_at(c, 0x86D5); /* CALL $8B22: the printer */
  push_bc(c);
  c->a = op_and(c, c->mem[MORE_LINES], c->mem[MORE_LINES]);
  if (!c->zf) goto L86F3;
  set_bc(c, 0x8000);
  for (;;) { /* $86E2: wait a while, or until a key goes down */
    c->a = op_xor(c, c->a, c->a);
    c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFE), 0x86E3);
    c->a = op_and(c, c->a, 0x1F);
    op_cp(c, 0x1F);
    if (!c->zf) goto L86F7;
    set_bc(c, (uint16_t)(get_bc(c) - 1));
    c->a = op_or(c, c->b, c->c);
    if (c->zf) break;
  }
  pop_bc(c);
  goto L8701;
L86F3:
  c->a = op_dec(c, c->a);
  wb(c, MORE_LINES, c->a);
L86F7:
  pop_bc(c);
  do { /* $86F8: wait for the key to come up */
    c->a = op_xor(c, c->a, c->a);
    c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFE), 0x86F9);
    c->a = op_and(c, c->a, 0x1F);
    op_cp(c, 0x1F);
  } while (!c->zf);
L8701:
  set_hl(c, 0x5020);
  c->c = 0x01;
  cpu_call_at(c, 0x8706); /* CALL $876B: scroll */
  c->a = 0x2A;
  goto L8753;
L870D:
  op_cp(c, 0x08);
  if (!c->zf) goto L8722;
  cpu_call_at(c, 0x8711); /* CALL $8761 */
  c->a = 0x20;
  cpu_call_at(c, 0x8716); /* CALL $87C9 */
  cpu_call_at(c, 0x8719); /* CALL $8761 */
  c->a = op_inc(c, c->mem[TW_LEFT]);
  goto L8753;
L8722:
  op_cp(c, 0x41);
  if (c->cf) goto L872C;
  op_cp(c, 0x5B);
  if (!c->cf) goto L872C;
  c->a = op_or(c, c->a, 0x20);
L872C:
  push_hl(c);
  set_hl(c, CAPITAL);
  /* INC (HL); DEC (HL): Z if it is 0 */
  wb(c, CAPITAL, op_inc(c, c->mem[CAPITAL]));
  wb(c, CAPITAL, op_dec(c, c->mem[CAPITAL]));
  if (c->zf) goto L8740;
  op_cp(c, 0x61);
  if (c->cf) goto L8740;
  op_cp(c, 0x7B);
  if (!c->cf) goto L8740;
  c->a &= ~0x20; /* RES 5,A */
  wb(c, CAPITAL, 0x00);
L8740:
  op_cp(c, 0x2E);
  if (c->zf) wb(c, CAPITAL, op_inc(c, c->mem[CAPITAL]));
  pop_hl(c); /* $8745 */
  cpu_call_at(c, 0x8746); /* CALL $87C9 */
  wb(c, TW_LAST, c->a);
  c->a = op_dec(c, c->mem[TW_LEFT]);
  if (c->zf) goto L86D1;
L8753:
  wb(c, TW_LEFT, c->a);
  wr16(c, TW_POS, get_hl(c));
  c->a = c->c;
  wb(c, TW_BIT, c->a);
  cpu_pop_af(c), pop_bc(c), pop_hl(c);
}

/* $876B ScrollLine: scroll character rows 1-17 up into rows 0-16 (pixels
 * and attributes), and clear row 17 with 42 spaces of the proportional
 * font. All registers and flags are kept. */
static void p_876b(Cpu *c) {
  cpu_push_af(c), push_bc(c), push_hl(c), push_de(c);
  set_hl(c, 0x4020);
  set_de(c, 0x4000);
  c->a = 0x11;
  c->b = 0x00;
  do { /* $8779 */
    push_hl(c), push_de(c);
    c->c = 0x08;
    do { /* $877D */
      push_hl(c), push_de(c), push_bc(c);
      c->c = 0x20;
      ldir(c);
      pop_bc(c), pop_de(c), pop_hl(c);
      c->h++, c->d++;
      c->c = op_dec(c, c->c);
    } while (!c->zf);
    pop_de(c), pop_hl(c);
    c->c = 0x20;
    set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
    ex_de_hl(c);
    set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
    ex_de_hl(c);
    cpu_push_af(c); /* F bits 5, 3 from the last ADD HL,BC */
    c->a = op_and(c, c->d, 0x07);
    if (!c->zf) c->d = op_add(c, c->d, 0x07, 0); /* into the next third */
    c->a = op_and(c, c->h, 0x07);
    if (!c->zf) c->h = op_add(c, c->h, 0x07, 0);
    cpu_pop_af(c); /* $87A7 */
    c->a = op_dec(c, c->a);
  } while (!c->zf);
  set_hl(c, 0x5820);
  set_de(c, 0x5800);
  set_bc(c, 0x0220);
  ldir(c);
  c->b = 0x2A;
  set_hl(c, 0x5020);
  c->c = 0x01;
  c->a = 0x20;
  do cpu_call_at(c, 0x87BF); /* CALL $87C9 */
  while (--c->b);
  pop_de(c), pop_hl(c), pop_bc(c), cpu_pop_af(c);
}

/* $87C9 PrintPropChar: print A in the game's font (6 pixels wide, at
 * $8822 for space) at screen address HL, C pixels in (0-7): each row is
 * shifted right C pixels and merged into the byte at HL and the next.
 * Moves C on 6 pixels (and L one byte when it passes 8). A, B, DE and the
 * flags are kept. */
static void p_87c9(Cpu *c) {
  cpu_push_af(c), push_bc(c), push_de(c), push_hl(c);
  set_de(c, (uint16_t)(c->a * 8 + PROP_FONT));
  pop_hl(c);
  push_hl(c);
  c->b = 0x08;
  do { /* $87DC */
    uint8_t a = c->mem[get_de(c)], mask = 0xFF;
    push_bc(c);
    for (uint8_t n = c->c; n; n--) a >>= 1, mask >>= 1; /* $87E4 */
    uint16_t hl = get_hl(c);
    wb(c, hl, (uint8_t)((~mask & c->mem[hl]) | a)); /* $87EB */
    pop_bc(c);
    if (c->c) {
      push_bc(c);
      uint8_t n = (uint8_t)(8 - c->c);
      a = c->mem[get_de(c)], mask = 0xFF;
      do a <<= 1, mask <<= 1; /* $87FE */
      while (--n);
      hl++;
      wb(c, hl, (uint8_t)((~mask & c->mem[hl]) | a));
      pop_bc(c);
    }
    set_de(c, (uint16_t)(get_de(c) + 1)); /* $880E */
    c->h++;
  } while (--c->b);
  pop_hl(c), pop_de(c), pop_bc(c);
  c->a = op_add(c, c->c, 0x06, 0);
  op_cp(c, 0x08);
  if (!c->cf) {
    c->a = op_sub(c, c->a, 0x08, 0);
    c->l = op_inc(c, c->l);
  }
  c->c = c->a; /* $881F */
  cpu_pop_af(c);
}

/* ---------- the ZX Printer ---------- */

/* $8B22: if PRINTER_ON, copy character row 17 (the line just finished)
 * to the ZX Printer (port $FB): for each of its 8 pixel rows, wait for
 * the stylus, send 256 dots, stop the motor. Returns at once if the
 * printer is not there (bit 6 of its port set). */
static void p_8b22(Cpu *c) {
  c->a = op_and(c, c->mem[PRINTER_ON], c->mem[PRINTER_ON]);
  if (c->zf) return;
  push_hl(c), push_de(c), push_bc(c);
  c->d = 0x01;
  set_hl(c, 0x5020);
  c->a = op_xor(c, c->a, c->a);
  c->e = c->a;
  do {
    c->out(c, (uint16_t)(c->a << 8 | 0xFB), c->a); /* $8B31 */
    do { /* $8B33 */
      c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFB), 0x8B33);
      c->a = op_add(c, c->a, c->a, 0);
      if (c->sf) goto L8B74; /* no printer */
    } while (!c->cf);
    push_hl(c), push_de(c);
    c->a = c->d;
    op_cp(c, 0x02);
    c->a = op_sub(c, c->a, c->a, c->cf); /* SBC A,A */
    c->a = op_and(c, c->a, c->e);
    op_rlca(c);
    c->a = op_and(c, c->a, c->e);
    c->d = c->a;
    do { /* $8B45: the 32 bytes of the pixel row */
      c->c = c->mem[get_hl(c)];
      push_hl(c);
      c->b = 0x08;
      do { /* $8B49 */
        c->a = c->d;
        c->c = op_rlc(c, c->c);
        op_rra(c);
        c->h = c->a;
        do { /* $8B4E */
          c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFB), 0x8B4E);
          op_rra(c);
        } while (!c->cf);
        c->a = c->h;
        c->out(c, (uint16_t)(c->a << 8 | 0xFB), c->a);
      } while (--c->b);
      pop_hl(c);
      set_hl(c, (uint16_t)(get_hl(c) + 1));
      c->a = op_and(c, c->l, 0x1F);
    } while (!c->zf);
    do { /* $8B5F */
      c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFB), 0x8B5F);
      op_rra(c);
    } while (!c->cf);
    c->a = c->d;
    op_rrca(c);
    c->out(c, (uint16_t)(c->a << 8 | 0xFB), c->a);
    pop_de(c), pop_hl(c);
    c->h++;
    c->e = op_inc(c, c->e);
    op_bit(c, 3, c->e);
  } while (c->zf);
  c->a = 0x04;
  c->out(c, (uint16_t)(c->a << 8 | 0xFB), c->a);
L8B74:
  pop_bc(c), pop_de(c), pop_hl(c);
}

/* ---------- the keyboard ---------- */

/* $8B78 Pause: count BC down from 1000. */
static void p_8b78(Cpu *c) {
  set_bc(c, 0x03E8);
  set_bc(c, 0);
  c->a = op_or(c, c->b, c->c);
}

/* $8B93 GetKey: scan the keyboard, half-row by half-row, and return in A
 * the character of a key that has gone down since the last scan (0 if
 * none): KEY_STATE keeps each half-row as last read, KEY_MASK takes out
 * keys that are never reported (CAPS SHIFT, SYMBOL SHIFT, 1, 3, 4, 9).
 * The character comes from KEY_MAP1, or KEY_MAP2 when CAPS SHIFT or
 * SYMBOL SHIFT is held. BC, HL and IX are kept. */
static void p_8b93(Cpu *c) {
  push_hl(c);
  push16(c, c->ix);
  push_bc(c);
  cpu_call_at(c, 0x8B97); /* CALL $8B78 */
  wr16(c, KEY_FOUND, get_bc(c));
  set_hl(c, KEY_STATE);
  c->ix = KEY_MASK;
  set_bc(c, 0xFEFE);
  do { /* $8BA8 */
    c->a = c->in_at(c, get_bc(c), 0x8BA8);
    c->a = op_and(c, c->a, 0x1F);
    c->a = op_or(c, c->a, c->mem[c->ix]);
    cpu_push_af(c);
    op_cpl(c);
    c->a = op_and(c, c->a, c->mem[get_hl(c)]);
    op_cpl(c);
    if (!c->zf) { /* a key has gone down in this half-row */
      wr16(c, KEY_FOUND, get_bc(c));
      wb(c, KEY_FOUND, c->a);
    }
    cpu_pop_af(c); /* $8BBC */
    wb(c, get_hl(c), c->a);
    set_hl(c, (uint16_t)(get_hl(c) + 1));
    c->ix++;
    c->b = op_rlc(c, c->b);
  } while (c->cf);
  set_bc(c, rd16(c, KEY_FOUND));
  c->a = op_or(c, c->b, c->c);
  if (c->zf) goto L8BF6;
  c->a = 0xFB;
  do { /* $8BCF: 5 a half-row */
    c->a = op_add(c, c->a, 0x05, 0);
    c->b = op_rrc(c, c->b);
  } while (c->cf);
  c->a = op_dec(c, c->a);
  do { /* $8BD6: and 1 a key */
    c->a = op_inc(c, c->a);
    c->c = op_rrc(c, c->c);
  } while (c->cf);
  c->c = c->a;
  c->b = 0x00;
  set_hl(c, KEY_MAP1);
  c->a = 0xFE;
  c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFE), 0x8BE3);
  c->a = op_and(c, c->a, 0x01); /* CAPS SHIFT */
  if (c->zf) goto L8BF1;
  c->a = 0x7F;
  c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFE), 0x8BEB);
  c->a = op_and(c, c->a, 0x02); /* SYMBOL SHIFT */
  if (!c->zf) goto L8BF4;
L8BF1:
  set_hl(c, KEY_MAP2);
L8BF4:
  set_hl(c, op_add16(c, get_hl(c), get_bc(c)));
  c->a = c->mem[get_hl(c)];
L8BF6:
  pop_bc(c);
  c->ix = pop16(c);
  pop_hl(c);
}

/* $969A WaitForKey2 (after a picture): wait for a key, then set the
 * border white. */
static void p_969a(Cpu *c) {
  do {
    c->a = op_xor(c, c->a, c->a);
    c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFE), 0x969B);
    c->a = op_and(c, c->a, 0x1F);
    op_cp(c, 0x1F);
  } while (c->zf);
  c->a = 0x07;
  c->out(c, (uint16_t)(c->a << 8 | 0xFE), c->a);
}

/* $90DF (after "You are dead"): wait for a key, then restart the game. */
static void p_90df(Cpu *c) {
  do {
    c->a = op_xor(c, c->a, c->a);
    c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFE), 0x90E0);
    c->a = op_and(c, c->a, 0x1F);
    op_cp(c, 0x1F);
  } while (c->zf);
  cpu_tail(c, 0x6C27);
}

#define OUT_DEFAULT (OUT_REGS | OUT_ZF | OUT_CF)

const PortRoutine io_routines[] = {
    {0x8576, "CanPrint", p_8576, OUT_DEFAULT},
    {0x8583, "PrintCR", p_8583, OUT_DEFAULT},
    {0x858B, "PrintChar", p_858b, OUT_DEFAULT},
    {0x85B8, "InputLine", p_85b8, OUT_DEFAULT},
    {0x860D, "InputScrollUp", p_860d, OUT_DEFAULT},
    {0x864A, "InputScrollDn", p_864a, OUT_DEFAULT},
    {0x867A, "RomChar", p_867a, OUT_DEFAULT},
    {0x86A1, "TextWindow", p_86a1, OUT_DEFAULT},
    {0x8761, "TextBack", p_8761, OUT_DEFAULT},
    {0x876B, "ScrollLine", p_876b, OUT_DEFAULT},
    {0x87C9, "PropChar", p_87c9, OUT_DEFAULT},
    {0x8B22, "PrinterCopy", p_8b22, OUT_DEFAULT},
    {0x8B78, "Pause", p_8b78, OUT_DEFAULT},
    {0x8B93, "GetKey", p_8b93, OUT_DEFAULT},
    {0x90DF, "DeadWaitKey", p_90df, OUT_DEFAULT},
    {0x969A, "WaitForKey2", p_969a, OUT_DEFAULT},
    {0, NULL, NULL, 0},
};
