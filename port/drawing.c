/* Drawing the location pictures ($7F78-$8250).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * Drawing ($7F78) is called by YouSee ($966E) with the location in A. If
 * graphics are on, it looks the location up in the picture table at
 * $CC00 (3 bytes an entry: location, address of its data; $FF ends) and
 * draws the picture into the top 16 character rows of the screen
 * (pixels $4000-$4FFF, attributes $5800-$59FF).
 *
 * Coordinates: D = x (0-255, left to right), E = y (0-127, from the
 * bottom: screen row = $7F - E).
 *
 * The picture data:
 *   byte 0       border colour (OUT $FE)
 *   byte 1       attribute for the picture area ($5800-$59FF)
 *                (both 0 if the location is dark: the picture then ends
 *                there, after clearing)
 *   then commands, until $00:
 *   $08 x y      move the pen to (x, y)
 *   $80-$FF n    line: from the pen, L = (n & $3F) + 1 steps; bit 0 of the
 *                command selects the axis that steps every time (0: x,
 *                1: y), bit 2 the x direction (1: left), bit 1 the y
 *                direction (1: down); the other axis steps once every
 *                B = ((cmd >> 3 & $0F) << 2 | n >> 6) + 1 steps. Each
 *                point is plotted before the step, so the last one is not;
 *                the pen ends there. A line stops at the edge.
 *   $40-$7F x y  flood fill from (x, y) with ink (cmd & 7); the pen is
 *                unchanged.
 *   $20-$3F h l  attributes: paper (cmd & 7) from attribute address hl,
 *                then bytes up to $FF: (b & 3) direction (0 up, 1 right,
 *                2 down, 3 left), (b >> 2) + 1 cells; each cell gets the
 *                paper and keeps its ink (inverted if equal to the paper),
 *                with no BRIGHT or FLASH, then the address moves on (it
 *                stays inside $5800-$59FF).
 *   others       (1-7, 9-$1F) ignored.
 * Plotting a point sets its pixel and the ink of its cell to INK ($824E)
 * (inverted if it equals the paper), with no BRIGHT or FLASH. INK is the
 * colour of the fill in progress, 0 after one; it is $38 when the game
 * starts.
 *
 * Every CALL, to this file's routines too, is made with cpu_call_at, so
 * the stack holds what the original leaves there (the random number
 * generator reads it); the flood fill keeps its seeds on the stack too.
 *
 * The alternate AF (EX AF,AF' in $820B) is not part of Cpu: $820B leaves
 * it changed. No code in the game reads AF' before writing it. */
#include <stddef.h>
#include <stdlib.h>

#include "cpu.h"
#include "routines.h"

#define M(a) c->mem[(uint16_t)(a)]

/* Variables. */
#define GRAPHICS_ON 0xB707 /* 0: text only (N held at the title) */
#define PICTURE_LOC 0x7F77 /* location whose picture is shown, $FF none */
#define FILL_ABOVE 0x806F  /* flood fill: a seed was pushed above this run */
#define FILL_BELOW 0x8070  /* ... below it */
#define INK 0x824E         /* ink for plotting (the fill colour) */
#define PICTURES 0xCC00    /* location, picture data address; $FF ends */

/* Routines elsewhere. */
#define R_FIND 0x9DBD     /* IX = 3-byte record with key A in table IX; Z if none */
#define R_IS_DARK 0x95ED  /* carry if the player's location is dark */

/* Sites of the CALLs to other files' routines. */
#define SITE_FIND 0x7F91
#define SITE_IS_DARK 0x820B

/* CALL at site (CALL nn, or a conditional CALL whose condition holds): the
 * original CALL instruction is run, so the stack gets the real return
 * address; this file's own routines then run through the hook too. A
 * callee that leaves our frame abandons this routine, as in the
 * original. target is checked against the instruction. (DrawingSetup
 * does its CALL $820B itself: in the dark, $820B returns past it.) */
static void call_at(Cpu *c, uint16_t site, uint16_t target) {
  if (rd16(c, (uint16_t)(site + 1)) != target) abort();
  cpu_call_at(c, site);
}
#define CALL(site, target) call_at(c, site, target)

/* AF kept in C (EX AF,AF' and back), with all of F. */
typedef struct {
  uint8_t a;
  uint8_t f;
  bool cf;
} SavedAF;
static SavedAF save_af(const Cpu *c) { return (SavedAF){c->a, cpu_f(c), c->cf}; }
static void restore_af(Cpu *c, SavedAF s) { c->a = s.a, cpu_set_f(c, s.f); }

/* ---------- pixels ---------- */

/* $81DE PixelAddress: HL = screen address of the point (D, E), A = its bit
 * (with the flags of the last RRC A). Keeps BC, DE. */
static void p_pixel_address(Cpu *c) {
  c->a = op_sub(c, 0x7F, c->e, 0); /* the row */
  c->l = c->a;
  c->a = op_or(c, op_and(c, c->a, 0x07), 0x40);
  c->h = c->a;
  c->a = op_and(c, c->l, 0xC0);
  op_rrca(c), op_rrca(c), op_rrca(c);
  c->a = op_or(c, c->a, c->h);
  c->h = c->a;
  c->a = op_and(c, c->l, 0x38);
  op_rlca(c), op_rlca(c);
  c->l = c->a;
  c->a = c->d;
  op_rrca(c), op_rrca(c), op_rrca(c);
  c->a = op_and(c, c->a, 0x1F);
  c->a = op_or(c, c->a, c->l);
  c->l = c->a;
  c->a = op_and(c, c->d, 0x07);
  push16(c, get_bc(c));
  c->b = c->a;
  c->b = op_inc(c, c->b);
  c->a = 0x01;
  do c->a = op_rrc(c, c->a);
  while (--c->b);
  set_bc(c, pop16(c));
}

/* $80EE TestPixel: A = the point's bit AND the screen byte: Z if the point
 * (D, E) is not set. Keeps HL. */
static void p_test_pixel(Cpu *c) {
  push16(c, get_hl(c));
  CALL(0x80EF, 0x81DE);
  c->a = op_and(c, c->a, M(get_hl(c)));
  set_hl(c, pop16(c));
}

/* $81B5 Plot: set the point (D, E), and give its cell the ink INK (its
 * complement if that is the cell's paper), no BRIGHT or FLASH. A = the
 * new screen byte. Keeps HL. */
static void p_plot(Cpu *c) {
  push16(c, get_hl(c));
  CALL(0x81B6, 0x81DE);
  cpu_push_af(c);
  push16(c, get_hl(c));
  c->a = op_and(c, c->h, 0x18);
  op_rrca(c), op_rrca(c), op_rrca(c);
  c->a = op_add(c, c->a, 0x58, 0);
  c->h = c->a; /* the attribute */
  c->a = op_and(c, M(get_hl(c)), 0x38);
  M(get_hl(c)) = c->a;
  c->a = M(INK);
  op_rlca(c), op_rlca(c), op_rlca(c);
  op_cp(c, M(get_hl(c)));
  if (c->zf) c->a = op_xor(c, c->a, 0x38);
  op_rrca(c), op_rrca(c), op_rrca(c);
  c->a = op_or(c, c->a, M(get_hl(c)));
  M(get_hl(c)) = c->a;
  set_hl(c, pop16(c));
  cpu_pop_af(c);
  c->a = op_or(c, c->a, M(get_hl(c)));
  M(get_hl(c)) = c->a;
  set_hl(c, pop16(c));
}

/* ---------- moving the pen ----------
 * Each moves (D, E) one point and leaves NZ, or Z at the edge of the
 * picture (then the pen does not move). Keep A; the ones that test with
 * OR or XOR use H. */

/* $8135: NZ (OR 1), keeping A in H too. */
static void nz_keep_a(Cpu *c) {
  c->h = c->a;
  op_or(c, c->a, 0x01);
  c->a = c->h;
}

/* $812B PenUp: E + 1, up to $7F. */
static void p_pen_up(Cpu *c) {
  c->e = op_inc(c, c->e);
  if (!op_bit(c, 7, c->e)) {
    nz_keep_a(c);
    return;
  }
  c->e = op_dec(c, c->e);
  c->h = c->a;
  op_xor(c, c->a, c->a);
  c->a = c->h;
}

/* $813A PenDown: E - 1, down to 0. */
static void p_pen_down(Cpu *c) {
  c->e = op_dec(c, c->e);
  if (!op_bit(c, 7, c->e)) {
    nz_keep_a(c);
    return;
  }
  c->e = op_inc(c, c->e);
}

/* $8141 PenRight: D + 1, up to $FF. */
static void p_pen_right(Cpu *c) {
  c->d = op_inc(c, c->d);
  if (!c->zf) return;
  c->d = op_dec(c, c->d);
  c->h = c->a;
  op_xor(c, c->a, c->a);
  c->a = c->h;
}

/* $8148 PenLeft: D - 1, down to 0. */
static void p_pen_left(Cpu *c) {
  c->d = op_dec(c, c->d);
  c->h = c->a;
  c->a = c->d;
  op_cp(c, 0xFF);
  c->a = c->h;
  if (!c->zf) return;
  c->d = op_inc(c, c->d);
}

/* ---------- moving in the attributes ----------
 * HL moves one cell in the picture's attributes ($5800-$59FF), staying
 * where it is at the edge (left and right wrap to the next row). Keep AF,
 * DE. Called only with Z (after AND A or DEC A giving 0). */

/* $80F5 AttrUp. */
static void p_attr_up(Cpu *c) {
  cpu_push_af(c);
  push16(c, get_de(c));
  set_de(c, 0x0020);
  c->a = op_and(c, c->a, c->a);
  set_hl(c, op_sbc16(c, get_hl(c), get_de(c)));
  c->a = c->h;
  op_cp(c, 0x57);
  if (c->zf) set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  set_de(c, pop16(c));
  cpu_pop_af(c);
}

/* $8106 AttrDown. */
static void p_attr_down(Cpu *c) {
  cpu_push_af(c);
  push16(c, get_de(c));
  set_de(c, 0x0020);
  set_hl(c, op_add16(c, get_hl(c), get_de(c)));
  c->a = c->h;
  op_cp(c, 0x5A);
  if (c->zf) {
    c->a = op_and(c, c->a, c->a);
    set_hl(c, op_sbc16(c, get_hl(c), get_de(c)));
  }
  /* $8113: LD D,D (the JR lands inside SBC HL,DE) */
  set_de(c, pop16(c));
  cpu_pop_af(c);
}

/* $8117 AttrLeft. */
static void p_attr_left(Cpu *c) {
  cpu_push_af(c);
  set_hl(c, (uint16_t)(get_hl(c) - 1));
  c->a = c->h;
  op_cp(c, 0x57);
  if (c->zf) set_hl(c, (uint16_t)(get_hl(c) + 1));
  cpu_pop_af(c);
}

/* $8121 AttrRight. */
static void p_attr_right(Cpu *c) {
  cpu_push_af(c);
  set_hl(c, (uint16_t)(get_hl(c) + 1));
  c->a = c->h;
  op_cp(c, 0x5A);
  if (c->zf) set_hl(c, (uint16_t)(get_hl(c) - 1));
  cpu_pop_af(c);
}

/* ---------- lines ---------- */

/* $8151 Line: plot L points from the pen (D, E), stepping each time
 * along x (bit 0 of C clear; bit 2 set: left) or y (bit 0 set; bit 1 set:
 * down), and along the other axis once every B points. Stops early at
 * the edge. The pen is left after the last point (not plotted). Keeps
 * BC, HL. */
static void p_line(Cpu *c) {
  if (op_bit(c, 0, c->c)) goto L8185;
  push16(c, get_hl(c));
  push16(c, get_bc(c));
L8157:
  CALL(0x8157, 0x81B5);
  if (!op_bit(c, 2, c->c)) goto L8165;
  CALL(0x815E, 0x8148);
  if (c->zf) goto L8182;
  goto L816A;
L8165:
  CALL(0x8165, 0x8141);
  if (c->zf) goto L8182;
L816A:
  c->b = op_dec(c, c->b);
  if (!c->zf) goto L817F;
  if (!op_bit(c, 1, c->c)) goto L8178;
  CALL(0x8171, 0x813A);
  if (c->zf) goto L8182;
  goto L817D;
L8178:
  CALL(0x8178, 0x812B);
  if (c->zf) goto L8182;
L817D:
  set_bc(c, pop16(c));
  push16(c, get_bc(c));
L817F:
  c->l = op_dec(c, c->l);
  if (!c->zf) goto L8157;
L8182:
  set_bc(c, pop16(c));
  set_hl(c, pop16(c));
  return;

L8185:
  push16(c, get_hl(c));
  push16(c, get_bc(c));
L8187:
  CALL(0x8187, 0x81B5);
  if (!op_bit(c, 1, c->c)) goto L8195;
  CALL(0x818E, 0x813A);
  if (c->zf) goto L81B2;
  goto L819A;
L8195:
  CALL(0x8195, 0x812B);
  if (c->zf) goto L81B2;
L819A:
  c->b = op_dec(c, c->b);
  if (!c->zf) goto L81AF;
  if (!op_bit(c, 2, c->c)) goto L81A8;
  CALL(0x81A1, 0x8148);
  if (c->zf) goto L81B2;
  goto L81AD;
L81A8:
  CALL(0x81A8, 0x8141);
  if (c->zf) goto L81B2;
L81AD:
  set_bc(c, pop16(c));
  push16(c, get_bc(c));
L81AF:
  c->l = op_dec(c, c->l);
  if (!c->zf) goto L8187;
L81B2:
  set_bc(c, pop16(c));
  set_hl(c, pop16(c));
}

/* ---------- flood fill ---------- */

/* $8071 Fill: fill with ink A from the point (D, E), up to set points.
 * A scan-line fill: from a seed, go left to the edge of the area, then
 * plot rightwards to its other edge, pushing a seed for each new run
 * found in the rows above and below (FILL_ABOVE, FILL_BELOW: the run there
 * already has one). The seeds are kept on the stack, under $0080 (E =
 * $80 ends). The points at the edges are plotted too, giving their cells
 * the ink. INK is the ink while filling, then 0. Keeps DE, HL. */
static void p_fill(Cpu *c) {
  M(INK) = c->a;
  push16(c, get_de(c));
  push16(c, get_hl(c));
  set_hl(c, 0x0080);
  push16(c, get_hl(c));
L807A: /* left to the edge of the run */
  CALL(0x807A, 0x80EE);
  if (!c->zf) goto L8086;
  CALL(0x807F, 0x8148);
  if (!c->zf) goto L807A;
  goto L808C;
L8086:
  CALL(0x8086, 0x81B5);
  CALL(0x8089, 0x8141);
L808C:
  set_hl(c, 0x0000);
  wr16(c, FILL_ABOVE, get_hl(c));
L8092: /* the point above */
  CALL(0x8092, 0x812B);
  c->a = 0x00;
  if (c->zf) goto L80AE;
  CALL(0x8099, 0x80EE);
  c->a = 0x00;
  if (!c->zf) goto L80A9;
  c->a = op_and(c, M(FILL_ABOVE), M(FILL_ABOVE));
  if (!c->zf) goto L80A9;
  push16(c, get_de(c)); /* a seed */
  c->a = 0x01;
L80A9:
  cpu_push_af(c);
  CALL(0x80AA, 0x813A);
  cpu_pop_af(c);
L80AE:
  M(FILL_ABOVE) = c->a;
  /* the point below */
  CALL(0x80B1, 0x813A);
  c->a = 0x00;
  if (c->zf) goto L80CD;
  CALL(0x80B8, 0x80EE);
  c->a = 0x00;
  if (!c->zf) goto L80C8;
  c->a = op_and(c, M(FILL_BELOW), M(FILL_BELOW));
  if (!c->zf) goto L80C8;
  push16(c, get_de(c)); /* a seed */
  c->a = 0x01;
L80C8:
  cpu_push_af(c);
  CALL(0x80C9, 0x812B);
  cpu_pop_af(c);
L80CD:
  M(FILL_BELOW) = c->a;
  CALL(0x80D0, 0x81B5);
  CALL(0x80D3, 0x8141); /* right */
  if (c->zf) goto L80E0;
  CALL(0x80D8, 0x80EE);
  if (c->zf) goto L8092;
  CALL(0x80DD, 0x81B5);
L80E0: /* the next seed */
  set_de(c, pop16(c));
  c->a = c->e;
  op_cp(c, 0x80);
  if (!c->zf) goto L807A;
  c->a = 0x00;
  M(INK) = c->a;
  set_hl(c, pop16(c));
  set_de(c, pop16(c));
}

/* ---------- the picture ---------- */

/* $820B DrawingClear: read the border colour and the attribute for the
 * picture from IY (moving IY past them), set the border, clear the
 * picture's pixels ($4000-$4FFF) and set its attributes ($5800-$59FF).
 * In the dark, both are 0, and the picture ends here: the routine pops its
 * return address and jumps to the end of DrawingSetup ($8069), returning
 * from that. Otherwise A and the flags are as $95ED left them (NC). Keeps
 * BC, DE, HL. clear_screen is all but the dark ending: true if dark. */
static bool clear_screen(Cpu *c) {
  CALL(0x820B, 0x95ED);
  SavedAF dark = save_af(c); /* in AF while the colours go through AF' */
  uint8_t border = M(c->iy);
  c->iy++;
  if (dark.cf) border = 0x00;
  c->out(c, (uint16_t)(border << 8 | 0xFE), border);
  push16(c, get_hl(c));
  push16(c, get_de(c));
  push16(c, get_bc(c));
  set_hl(c, 0x4000);
  set_de(c, 0x4001);
  set_bc(c, 0x0FFF);
  M(get_hl(c)) = 0x00;
  ldir(c);
  set_hl(c, 0x5800);
  set_de(c, 0x5801);
  set_bc(c, 0x01FF);
  uint8_t paper = M(c->iy);
  c->iy++;
  if (dark.cf) paper = 0x00;
  M(get_hl(c)) = paper;
  ldir(c);
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  set_hl(c, pop16(c));
  restore_af(c, dark);
  return c->cf; /* RET NC */
}
static void p_drawing_clear(Cpu *c) {
  if (!clear_screen(c)) return;
  set_hl(c, pop16(c));
  cpu_tail(c, 0x824B); /* JP $8069: returns past DrawingSetup */
}

/* $7FA7 DrawingSetup: draw the picture whose data is at HL (see the top of
 * the file). Keeps BC, DE, HL, IY; A = 0 with Z at the end (A and the
 * flags of $95ED in the dark). */
static void p_drawing_setup(Cpu *c) {
  push16(c, c->iy);
  push16(c, get_hl(c));
  push16(c, get_hl(c));
  c->iy = pop16(c);
  push16(c, get_de(c));
  push16(c, get_bc(c));
  /* CALL $820B, done here: in the dark it pops its return address and
   * jumps to $8069, ending the picture. */
  push16(c, 0x7FB2);
  if (clear_screen(c)) {
    set_hl(c, pop16(c));
    goto L8069;
  }
  c->sp += 2;
  c->d = 0x7F;
  c->e = 0x3F;
  c->b = 0x01;
  c->c = 0x01;
  c->l = 0x01;
L7FBC: /* the next command */
  c->a = op_and(c, M(c->iy), M(c->iy));
  if (c->zf) goto L8069;
  c->iy++;
  op_cp(c, 0x08);
  if (!c->zf) goto L7FD5;
  c->d = M(c->iy); /* $08: move to (x, y) */
  c->iy++;
  c->e = M(c->iy);
  c->iy++;
  goto L7FBC;
L7FD5:
  if (!op_bit(c, 7, c->a)) goto L7FFA;
  c->b = c->a; /* a line */
  c->a = op_and(c, c->a, 0x07);
  c->c = c->a;
  c->a = c->b;
  op_rrca(c);
  c->a = op_and(c, c->a, 0x3C);
  c->b = c->a;
  c->a = op_and(c, M(c->iy), 0x3F);
  c->l = c->a;
  c->l = op_inc(c, c->l);
  c->a = M(c->iy);
  c->iy++;
  op_rlca(c), op_rlca(c);
  c->a = op_and(c, c->a, 0x03);
  c->a = op_or(c, c->a, c->b);
  c->b = c->a;
  c->b = op_inc(c, c->b);
  CALL(0x7FF5, 0x8151);
  goto L7FBC;
L7FFA:
  if (!op_bit(c, 6, c->a)) goto L8012;
  c->a = op_and(c, c->a, 0x07); /* a fill */
  push16(c, get_de(c));
  c->d = M(c->iy);
  c->iy++;
  c->e = M(c->iy);
  c->iy++;
  CALL(0x800B, 0x8071);
  set_de(c, pop16(c));
  goto L7FBC;
L8012:
  if (!op_bit(c, 5, c->a)) goto L7FBC;
  c->a = op_and(c, c->a, 0x07); /* attributes */
  op_rlca(c), op_rlca(c), op_rlca(c);
  push16(c, get_hl(c));
  push16(c, get_de(c));
  push16(c, get_bc(c));
  c->c = c->a;
  c->h = M(c->iy);
  c->iy++;
  c->l = M(c->iy);
  c->iy++;
L802A:
  c->a = M(c->iy);
  c->iy++;
  op_cp(c, 0xFF);
  if (c->zf) goto L8063;
  c->b = c->a;
  c->a = op_and(c, c->a, 0x03);
  c->e = c->a;
  c->a = c->b;
  op_rrca(c), op_rrca(c);
  c->a = op_and(c, c->a, 0x3F);
  c->a = op_inc(c, c->a);
  c->b = c->a;
L803E:
  c->a = op_and(c, M(get_hl(c)), 0x07);
  op_rlca(c), op_rlca(c), op_rlca(c);
  op_cp(c, c->c);
  if (c->zf) c->a = op_xor(c, c->a, 0x38);
  op_rrca(c), op_rrca(c), op_rrca(c);
  c->a = op_or(c, c->a, c->c);
  M(get_hl(c)) = c->a;
  c->a = op_and(c, c->e, c->e);
  if (c->zf) CALL(0x8050, 0x80F5);
  c->a = op_dec(c, c->a);
  if (c->zf) CALL(0x8054, 0x8121);
  c->a = op_dec(c, c->a);
  if (c->zf) CALL(0x8058, 0x8106);
  c->a = op_dec(c, c->a);
  if (c->zf) CALL(0x805C, 0x8117);
  if (--c->b) goto L803E;
  goto L802A;
L8063:
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  set_hl(c, pop16(c));
  goto L7FBC;
L8069:
  set_bc(c, pop16(c));
  set_de(c, pop16(c));
  set_hl(c, pop16(c));
  c->iy = pop16(c);
}

/* $7F78 Drawing: draw the picture of location A, if graphics are on and
 * it has one. PICTURE_LOC is then A, or $FF if nothing was drawn (YouSee
 * waits for a key if something was). Keeps every register and the flags. */
static void p_drawing(Cpu *c) {
  cpu_push_af(c);
  c->a = op_and(c, M(GRAPHICS_ON), M(GRAPHICS_ON));
  if (!c->zf) goto L7F86;
  c->a = 0xFF;
  M(PICTURE_LOC) = c->a;
  cpu_pop_af(c);
  return;
L7F86:
  cpu_pop_af(c);
  cpu_push_af(c);
  push16(c, get_hl(c));
  push16(c, get_bc(c));
  push16(c, get_de(c));
  push16(c, c->ix);
  c->ix = PICTURES;
  CALL(SITE_FIND, R_FIND); /* Z and A = $FF if the location has no picture */
  M(PICTURE_LOC) = c->a;
  c->l = M(c->ix + 1);
  c->h = M(c->ix + 2);
  if (!c->zf) CALL(0x7F9D, 0x7FA7);
  c->ix = pop16(c);
  set_de(c, pop16(c));
  set_bc(c, pop16(c));
  set_hl(c, pop16(c));
  cpu_pop_af(c);
}

const PortRoutine drawing_routines[] = {
    {0x7F78, "Drawing", p_drawing, OUT_REGS | OUT_ZF | OUT_CF},
    {0x7FA7, "DrawingSetup", p_drawing_setup, OUT_REGS | OUT_ZF | OUT_CF},
    {0x8071, "Fill", p_fill, OUT_REGS | OUT_ZF | OUT_CF},
    {0x80EE, "TestPixel", p_test_pixel, OUT_REGS | OUT_ZF | OUT_CF},
    {0x80F5, "AttrUp", p_attr_up, OUT_REGS | OUT_ZF | OUT_CF},
    {0x8106, "AttrDown", p_attr_down, OUT_REGS | OUT_ZF | OUT_CF},
    {0x8117, "AttrLeft", p_attr_left, OUT_REGS | OUT_ZF | OUT_CF},
    {0x8121, "AttrRight", p_attr_right, OUT_REGS | OUT_ZF | OUT_CF},
    {0x812B, "PenUp", p_pen_up, OUT_REGS | OUT_ZF | OUT_CF},
    {0x813A, "PenDown", p_pen_down, OUT_REGS | OUT_ZF | OUT_CF},
    {0x8141, "PenRight", p_pen_right, OUT_REGS | OUT_ZF | OUT_CF},
    {0x8148, "PenLeft", p_pen_left, OUT_REGS | OUT_ZF | OUT_CF},
    {0x8151, "Line", p_line, OUT_REGS | OUT_ZF | OUT_CF},
    {0x81B5, "Plot", p_plot, OUT_REGS | OUT_ZF | OUT_CF},
    {0x81DE, "PixelAddress", p_pixel_address, OUT_REGS | OUT_ZF | OUT_CF},
    {0x820B, "DrawingClear", p_drawing_clear, OUT_REGS | OUT_ZF | OUT_CF},
    {0, NULL, NULL, 0},
};
