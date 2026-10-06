/* The clean edition: the location pictures. See drawing.h for the picture
 * format and docs/CLEAN.md for the rules.
 *
 * Two halves: the interpreter (draw_picture and the commands: lines,
 * fills, attribute paths), which works in picture coordinates, and the
 * Canvas it draws on. The Spectrum canvas here is the original's screen
 * memory, byte for byte. */
#include "drawing.h"

#include "characters.h"
#include "platform.h"

#include <stddef.h>
#include <stdlib.h>

/* ---------- the Spectrum canvas ---------- */

#define SCREEN_PIXELS 0x4000 /* the picture's pixels: $4000-$4FFF */
#define SCREEN_ATTRS 0x5800  /* its attributes: $5800-$59FF */

/* The screen row of a picture point: the picture is the top 128 rows,
 * y counting up from the bottom. (Byte arithmetic, as the original: a y
 * over 127 would land below the picture.) */
static uint8_t screen_row(uint8_t y) { return (uint8_t)(0x7F - y); }

/* The screen byte holding the point, and the point's bit in it. */
static uint16_t pixel_address(uint8_t x, uint8_t y, uint8_t *bit) {
  uint8_t row = screen_row(y);
  *bit = (uint8_t)(0x80 >> (x & 7));
  return (uint16_t)(SCREEN_PIXELS | (row & 0x07) << 8 | (row & 0xC0) << 5 | (row & 0x38) << 2 | x >> 3);
}

/* The attribute of the cell holding the point. */
static uint16_t attr_address(uint8_t x, uint8_t y) {
  return (uint16_t)(SCREEN_ATTRS + (screen_row(y) >> 3) * 32 + (x >> 3));
}

static void spectrum_begin(Canvas *cv, uint8_t border, uint8_t attr) {
  (void)cv;
  device_out((uint16_t)(border << 8 | 0xFE), border); /* OUT ($FE),A */
  for (uint16_t a = SCREEN_PIXELS; a < SCREEN_PIXELS + 0x1000; a++) mem[a] = 0;
  for (uint16_t a = SCREEN_ATTRS; a < SCREEN_ATTRS + 0x200; a++) mem[a] = attr;
}

static bool spectrum_test(Canvas *cv, uint8_t x, uint8_t y) {
  (void)cv;
  uint8_t bit;
  return mem[pixel_address(x, y, &bit)] & bit;
}

/* The three bits of a byte rotated left three places (RLCA x3). */
static uint8_t rotate3(uint8_t v) { return (uint8_t)(v << 3 | v >> 5); }

static void spectrum_plot(Canvas *cv, uint8_t x, uint8_t y, uint8_t ink) {
  (void)cv;
  uint8_t bit;
  uint16_t pixel = pixel_address(x, y, &bit);
  uint16_t attr = attr_address(x, y);
  uint8_t paper = mem[attr] & 0x38;
  /* The original compares the ink, rotated into the paper's place, with
   * the paper: a raw ink byte over 7 (the $38 at the start) never
   * matches, and lands on the paper bits. */
  if (rotate3(ink) == paper) ink ^= 0x07;
  mem[attr] = paper | ink;
  mem[pixel] |= bit;
}

static void spectrum_paint(Canvas *cv, uint8_t col, uint8_t row, uint8_t paper) {
  (void)cv;
  uint16_t a = (uint16_t)(SCREEN_ATTRS + row * 32 + col);
  uint8_t ink = mem[a] & 0x07;
  if (ink == paper) ink ^= 0x07;
  mem[a] = (uint8_t)(paper << 3 | ink);
}

static Canvas spectrum = {spectrum_begin, spectrum_test, spectrum_plot, spectrum_paint};

Canvas *spectrum_canvas(void) { return &spectrum; }

/* ---------- the pen ----------
 * Byte arithmetic, as the original: y stops at 127 going up (bit 7 would
 * be set) and at 0 going down, x at 255 and 0. */

bool pen_up(Pen *p) {
  if ((uint8_t)(p->y + 1) & 0x80) return false;
  p->y++;
  return true;
}

bool pen_down(Pen *p) {
  if ((uint8_t)(p->y - 1) & 0x80) return false;
  p->y--;
  return true;
}

bool pen_right(Pen *p) {
  if (p->x == 0xFF) return false;
  p->x++;
  return true;
}

bool pen_left(Pen *p) {
  if (p->x == 0x00) return false;
  p->x--;
  return true;
}

/* ---------- lines ---------- */

enum { LINE_Y_MAJOR = 1, LINE_DOWN = 2, LINE_LEFT = 4 };

static bool step_x(Pen *p, uint8_t dir) { return dir & LINE_LEFT ? pen_left(p) : pen_right(p); }
static bool step_y(Pen *p, uint8_t dir) { return dir & LINE_DOWN ? pen_down(p) : pen_up(p); }

void draw_line(Canvas *cv, Pen *p, uint8_t dir, uint8_t count, uint8_t every) {
  bool y_major = dir & LINE_Y_MAJOR;
  uint8_t until_minor = every;
  do {
    cv->plot(cv, p->x, p->y, mem[V_DRAW_INK]);
    if (!(y_major ? step_y(p, dir) : step_x(p, dir))) return;
    if (--until_minor == 0) {
      if (!(y_major ? step_x(p, dir) : step_y(p, dir))) return;
      until_minor = every;
    }
  } while (--count);
}

/* ---------- flood fill ---------- */

/* The points still to fill from, last in first out. */
typedef struct {
  Pen *seeds;
  size_t n, cap;
} SeedStack;

static void push_seed(SeedStack *s, Pen p) {
  if (s->n == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->seeds = realloc(s->seeds, s->cap * sizeof *s->seeds);
    if (!s->seeds) abort();
  }
  s->seeds[s->n++] = p;
}

/* Look at the point next to p (above or below): true if it is clear. A
 * clear point starting a new stretch (the one before it was not clear)
 * becomes a seed. */
static bool look_beside(Canvas *cv, Pen p, bool (*move)(Pen *), bool was_clear, SeedStack *s) {
  if (!move(&p)) return false;
  if (cv->test(cv, p.x, p.y)) return false;
  if (!was_clear) push_seed(s, p);
  return true;
}

/* A scan-line fill, as the original's (which kept its seeds on the Z80
 * stack): from each seed, go left to the edge of the stretch, then fill
 * rightwards to its other edge, keeping a seed for each new stretch of
 * clear points above and below. The edge points are plotted too (they
 * are set already: it gives their cells the ink). A seed that has been
 * filled since it was kept is not passed over: the run starts again just
 * to its right, set or not, as in the original. */
void fill(Canvas *cv, uint8_t x, uint8_t y, uint8_t ink) {
  SeedStack seeds = {NULL, 0, 0};
  Pen p = {x, y};
  mem[V_DRAW_INK] = ink;
  for (;;) {
    /* Left to the edge. */
    for (;;) {
      if (cv->test(cv, p.x, p.y)) {
        cv->plot(cv, p.x, p.y, ink);
        pen_right(&p);
        break;
      }
      if (!pen_left(&p)) break;
    }
    /* Right to the other edge. */
    bool above = false, below = false;
    for (;;) {
      above = look_beside(cv, p, pen_up, above, &seeds);
      below = look_beside(cv, p, pen_down, below, &seeds);
      cv->plot(cv, p.x, p.y, ink);
      if (!pen_right(&p)) break;
      if (cv->test(cv, p.x, p.y)) {
        cv->plot(cv, p.x, p.y, ink);
        break;
      }
    }
    if (seeds.n == 0) break;
    p = seeds.seeds[--seeds.n];
  }
  free(seeds.seeds);
  mem[V_DRAW_INK] = 0;
}

/* ---------- attribute paths ----------
 * A cell is numbered 0-511 along the rows (col + 32 * row). A move off
 * the top or bottom of the picture, or past its first or last cell,
 * stays put; left and right go on along the rows. */

enum { CELL_UP, CELL_RIGHT, CELL_DOWN, CELL_LEFT };
enum { CELLS = PICTURE_COLS * PICTURE_ROWS };

static uint16_t cell_up(uint16_t cell) { return cell >= PICTURE_COLS ? cell - PICTURE_COLS : cell; }
static uint16_t cell_down(uint16_t cell) { return cell + PICTURE_COLS < CELLS ? cell + PICTURE_COLS : cell; }
static uint16_t cell_left(uint16_t cell) { return cell > 0 ? cell - 1 : cell; }
static uint16_t cell_right(uint16_t cell) { return cell + 1 < CELLS ? cell + 1 : cell; }

static uint16_t cell_move(uint16_t cell, uint8_t direction) {
  switch (direction) {
  case CELL_UP: return cell_up(cell);
  case CELL_RIGHT: return cell_right(cell);
  case CELL_DOWN: return cell_down(cell);
  default: return cell_left(cell);
  }
}

/* Paint the paper along the path at data (moves to $FF) from the cell;
 * returns the address after the $FF. */
static uint16_t paint_path(Canvas *cv, uint16_t data, uint16_t cell, uint8_t paper) {
  for (uint8_t b; (b = mem[data++]) != 0xFF;) {
    for (int n = (b >> 2) + 1; n > 0; n--) {
      cv->paint(cv, cell % PICTURE_COLS, cell / PICTURE_COLS, paper);
      cell = cell_move(cell, b & 3);
    }
  }
  return data;
}

/* ---------- the picture ---------- */

/* Is the player's location dark? */
static bool location_is_dark(void) { return player_in_dark(); }

/* Start the picture whose data is at data: its border colour and
 * attribute, both 0 in the dark. False in the dark: the picture is not
 * drawn. */
static bool begin_picture(Canvas *cv, uint16_t data) {
  bool dark = location_is_dark();
  cv->begin(cv, dark ? 0 : mem[data], dark ? 0 : mem[(uint16_t)(data + 1)]);
  return !dark;
}

enum { CMD_MOVE = 0x08, CMD_LINE = 0x80, CMD_FILL = 0x40, CMD_PAINT = 0x20 };

void draw_picture(Canvas *cv, uint16_t data) {
  if (!begin_picture(cv, data)) return;
  data += 2;
  Pen pen = {0x7F, 0x3F};
  for (uint8_t cmd; (cmd = mem[data++]) != 0;) {
    device_at(0x7FBC, cmd); /* the next command: a host may show the picture as it is drawn */
    if (cmd == CMD_MOVE) {
      pen.x = mem[data++];
      pen.y = mem[data++];
    } else if (cmd & CMD_LINE) {
      uint8_t n = mem[data++];
      uint8_t every = (uint8_t)(((cmd >> 1 & 0x3C) | n >> 6) + 1);
      draw_line(cv, &pen, cmd & 7, (uint8_t)((n & 0x3F) + 1), every);
    } else if (cmd & CMD_FILL) {
      uint8_t x = mem[data++];
      uint8_t y = mem[data++];
      fill(cv, x, y, cmd & 7);
    } else if (cmd & CMD_PAINT) {
      /* The cell is given by its attribute address, high byte first; the
       * game's pictures all start inside the picture ($5800-$59FF). */
      uint16_t attr = (uint16_t)(mem[data] << 8 | mem[(uint16_t)(data + 1)]);
      data = paint_path(cv, (uint16_t)(data + 2), (uint16_t)(attr - SCREEN_ATTRS), cmd & 7);
    }
  }
}

/* The location's picture in the table, NULL if it has none. */
static const PictureEntry *find_picture(uint8_t location) {
  for (const PictureEntry *e = (const PictureEntry *)&mem[PICTURE_TABLE]; e->location != 0xFF; e++)
    if (e->location == location) return e;
  return NULL;
}

void draw_location_picture(uint8_t location) {
  if (!mem[V_GRAPHICS]) {
    mem[V_PICTURE_LOC] = 0xFF;
    return;
  }
  const PictureEntry *e = find_picture(location);
  mem[V_PICTURE_LOC] = e ? location : 0xFF;
  if (e) draw_picture(spectrum_canvas(), e->data);
}

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ---------- adapters (for the faithful callers, while they remain) ----------
 * Adapters put back the registers the faithful routine keeps. Each
 * outputs mask is what the routine's callers use: the only caller from
 * outside this module is YouSee ($966E, of $7F78); the rest are called
 * from here. */

typedef struct {
  uint8_t a, b, c, d, e, h, l;
  uint16_t ix, iy;
  bool sf, zf, hf, pf, nf, cf, yf, xf;
} SavedRegs;

static SavedRegs save_regs(const Cpu *c) {
  return (SavedRegs){c->a, c->b, c->c, c->d, c->e, c->h, c->l, c->ix, c->iy,
                     c->sf, c->zf, c->hf, c->pf, c->nf, c->cf, c->yf, c->xf};
}

static void restore_regs(Cpu *c, const SavedRegs *s) {
  c->a = s->a, c->b = s->b, c->c = s->c, c->d = s->d, c->e = s->e, c->h = s->h, c->l = s->l;
  c->ix = s->ix, c->iy = s->iy;
  c->sf = s->sf, c->zf = s->zf, c->hf = s->hf, c->pf = s->pf, c->nf = s->nf, c->cf = s->cf;
  c->yf = s->yf, c->xf = s->xf;
}

/* $7F78 Drawing: location in A; keeps every register and the flags
 * (YouSee goes on using B). */
static void a_drawing(Cpu *c) {
  SavedRegs keep = save_regs(c);
  draw_location_picture(c->a);
  restore_regs(c, &keep);
}

/* $7FA7 DrawingSetup: data at HL; keeps BC, DE, HL, IY (Drawing restores
 * the rest). */
static void a_drawing_setup(Cpu *c) {
  SavedRegs keep = save_regs(c);
  draw_picture(spectrum_canvas(), (uint16_t)(c->h << 8 | c->l));
  restore_regs(c, &keep);
}

/* $820B DrawingClear: data at IY, moved past the two colours. In the
 * dark it abandons its caller as the original does: pops the return
 * address and goes to $824B (JP $8069), the end of DrawingSetup. Keeps
 * BC, DE, HL; its A and flags are not used (DrawingSetup sets them). */
static void a_drawing_clear(Cpu *c) {
  SavedRegs keep = save_regs(c);
  bool lit = begin_picture(spectrum_canvas(), c->iy);
  restore_regs(c, &keep);
  c->iy += 2;
  if (lit) return;
  set_hl(c, pop16(c));
  cpu_tail(c, 0x824B);
}

/* $8071 Fill: ink in A, from (D, E); A = 0 after. DrawingSetup keeps
 * DE across it and uses BC, HL no further: BC, DE, HL are as before. */
static void a_fill(Cpu *c) {
  fill(spectrum_canvas(), c->d, c->e, c->a);
  c->a = 0;
}

/* $80EE TestPixel: A = the point's bit of its screen byte, Z if clear
 * (Fill tests Z). Keeps BC, DE, HL. */
static void a_test_pixel(Cpu *c) {
  uint8_t bit;
  c->a = mem[pixel_address(c->d, c->e, &bit)] & bit;
  c->zf = c->a == 0;
}

/* $81DE PixelAddress: HL = the screen byte of (D, E), A = its bit (Plot
 * and TestPixel use them). Keeps BC, DE. */
static void a_pixel_address(Cpu *c) {
  uint8_t bit;
  set_hl(c, pixel_address(c->d, c->e, &bit));
  c->a = bit;
}

/* $81B5 Plot: the point (D, E) in the ink V_DRAW_INK; A = the new screen
 * byte. Keeps BC, DE, HL (Line and Fill go on with them). */
static void a_plot(Cpu *c) {
  uint8_t bit;
  spectrum_plot(spectrum_canvas(), c->d, c->e, mem[V_DRAW_INK]);
  c->a = mem[pixel_address(c->d, c->e, &bit)];
}

/* $8151 Line: from the pen (D, E), C = direction, L = points, B = points
 * per minor step; the pen moves. Keeps BC, HL. */
static void a_line(Cpu *c) {
  Pen p = {c->d, c->e};
  draw_line(spectrum_canvas(), &p, c->c, c->l, c->b);
  c->d = p.x, c->e = p.y;
}

/* $812B PenUp, $813A PenDown, $8141 PenRight, $8148 PenLeft: (D, E)
 * moved, NZ, or Z at the edge (Line and Fill test Z). Keep A. (The
 * originals also leave A in H, sometimes; their callers keep HL across
 * them.) */
static void pen_adapter(Cpu *c, bool (*move)(Pen *)) {
  Pen p = {c->d, c->e};
  c->zf = !move(&p);
  c->d = p.x, c->e = p.y;
}
static void a_pen_up(Cpu *c) { pen_adapter(c, pen_up); }
static void a_pen_down(Cpu *c) { pen_adapter(c, pen_down); }
static void a_pen_right(Cpu *c) { pen_adapter(c, pen_right); }
static void a_pen_left(Cpu *c) { pen_adapter(c, pen_left); }

/* $80F5 AttrUp, $8106 AttrDown, $8117 AttrLeft, $8121 AttrRight: HL (an
 * attribute address in the picture) moved one cell. Keep AF, BC, DE
 * (DrawingSetup goes on with A, B and C, and tests A again itself). */
static void attr_adapter(Cpu *c, uint16_t (*move)(uint16_t)) {
  set_hl(c, (uint16_t)(SCREEN_ATTRS + move((uint16_t)(get_hl(c) - SCREEN_ATTRS))));
}
static void a_attr_up(Cpu *c) { attr_adapter(c, cell_up); }
static void a_attr_down(Cpu *c) { attr_adapter(c, cell_down); }
static void a_attr_left(Cpu *c) { attr_adapter(c, cell_left); }
static void a_attr_right(Cpu *c) { attr_adapter(c, cell_right); }

#define KEEP (OUT_BC | OUT_DE | OUT_HL | OUT_IX | OUT_IY)

const CleanRoutine drawing_clean[] = {
    {0x7F78, "draw_location_picture", a_drawing, OUT_REGS | OUT_ZF | OUT_CF | OUT_SF | OUT_PF},
    {0x7FA7, "draw_picture", a_drawing_setup, KEEP},
    {0x820B, "begin_picture", a_drawing_clear, KEEP},
    {0x8071, "fill", a_fill, KEEP},
    {0x80EE, "test_pixel", a_test_pixel, OUT_A | OUT_ZF | KEEP},
    {0x81DE, "pixel_address", a_pixel_address, OUT_A | KEEP},
    {0x81B5, "plot", a_plot, OUT_A | KEEP},
    {0x8151, "draw_line", a_line, KEEP},
    {0x812B, "pen_up", a_pen_up, OUT_A | OUT_ZF | OUT_DE},
    {0x813A, "pen_down", a_pen_down, OUT_A | OUT_ZF | OUT_DE},
    {0x8141, "pen_right", a_pen_right, OUT_A | OUT_ZF | OUT_DE},
    {0x8148, "pen_left", a_pen_left, OUT_A | OUT_ZF | OUT_DE},
    {0x80F5, "cell_up", a_attr_up, OUT_A | KEEP},
    {0x8106, "cell_down", a_attr_down, OUT_A | KEEP},
    {0x8117, "cell_left", a_attr_left, OUT_A | KEEP},
    {0x8121, "cell_right", a_attr_right, OUT_A | KEEP},
    {0, NULL, NULL, 0},
};

const CleanScratch drawing_scratch[] = {
    {0x806F, 0x8070, "Fill's flags for the rows above and below the run (\"a seed was kept for this stretch\"): only Fill uses them, kept in C"},
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
