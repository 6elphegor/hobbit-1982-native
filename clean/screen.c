/* The clean edition: the screen and the keyboard. See docs/CLEAN.md and
 * screen.h. */
#include "screen.h"

#include <stddef.h>

/* Variables (in the game's data, and in code space next to the routines
 * that use them; others read some of them, so they stay in memory). */
#define PRINTER_ON 0xB6F2 /* 1: copy each finished line to the ZX Printer (PRINT) */
#define DRUNK 0xB700      /* after the wine: an "h" after each "s" */
#define CAPITAL 0xB704    /* nonzero: capitalise the next letter */
#define MORE_LINES 0xB716 /* lines to scroll before waiting for a key (or a while) */
#define IN_COLS 0x85B3    /* input line: columns left on the row */
#define IN_POS 0x85B4     /* input line: screen address of the cursor (a word) */
#define IN_CURSOR 0x85B6  /* input line: the cursor character */
#define TW_LEFT 0x869B    /* text window: characters left on the line */
#define TW_POS 0x869C     /* text window: screen address (a word) */
#define TW_BIT 0x869E     /* text window: pixel offset in that byte */
#define TW_INDENT 0x869F  /* text window: spaces to start a line with */
#define TW_LAST 0x86A0    /* text window: last character printed, 0 after a new line */
#define KEY_MASK 0x8B81   /* keys never reported, a byte per half-row */
#define KEY_FOUND 0x8B89  /* the port and (inverted) bits of the last new key */
#define KEY_STATE 0x8B8B  /* the 8 half-rows as last scanned */
#define KEY_MAP1 0x8BFB   /* characters by key, 5 a half-row */
#define KEY_MAP2 0x8C23   /* ... with CAPS SHIFT or SYMBOL SHIFT */
#define ROM_FONT 0x3D00   /* the ROM's character set, from space */
#define PROP_FONT 0x8722  /* the game's font, from character 0 ($8822: space) */

/* Screen places. */
#define TEXT_LINE 0x5020     /* character row 17: where text is printed */
#define TEXT_LINE_CHARS 42
#define INPUT_ROW_19 0x5060
#define INPUT_ROW_20 0x5080
#define INPUT_ROW_22 0x50C0
#define INPUT_ROW_23 0x50E0
#define INPUT_ROW_CHARS 32

/* ---------- devices (platform.h) ---------- */

/* The keyboard's port for all half-rows at once; a key is down when its
 * bit (0-4) reads 0. */
#define ALL_KEYS 0x00FE

static bool any_key_down(uint16_t at) { return (device_in(ALL_KEYS, at) & 0x1F) != 0x1F; }

/* ---------- screen addresses ---------- */

/* One character column right or left: the low byte only, as the original
 * steps it. */
static uint16_t column_right(uint16_t addr) { return (uint16_t)((addr & 0xFF00) | (uint8_t)(addr + 1)); }
static uint16_t column_left(uint16_t addr) { return (uint16_t)((addr & 0xFF00) | (uint8_t)(addr - 1)); }
static uint16_t at_column(uint16_t addr, uint8_t low) { return (uint16_t)((addr & 0xFF00) | low); }

/* The character row below, crossing into the next third of the screen. */
static uint16_t next_char_row(uint16_t addr) {
  addr = (uint16_t)(addr + 0x20);
  if (addr & 0x0700) addr = (uint16_t)(addr + 0x0700);
  return addr;
}

/* Copy one character row (its 8 pixel lines of 32 bytes) to another. */
static void copy_char_row(uint16_t from, uint16_t to) {
  for (int line = 0; line < 8; line++, from += 0x100, to += 0x100)
    for (int i = 0; i < 32; i++) mem[(uint16_t)(to + i)] = mem[(uint16_t)(from + i)];
}

/* ---------- printing ---------- */

bool can_print(void) { return (mem[V_DOING] & mem[V_OUTPUT_ON]) != 0; }

/* ---------- the input line (ROM font) ---------- */

/* Print ch in the ROM font at screen address addr (8 pixel lines). */
static void rom_char(uint8_t ch, uint16_t addr) {
  uint16_t glyph = (uint16_t)(ROM_FONT + (uint8_t)(ch - 0x20) * 8);
  for (int line = 0; line < 8; line++, addr += 0x100) mem[addr] = mem[(uint16_t)(glyph + line)];
}

/* $860D: scroll the input rows 20-23 up into 19-22, and blank row 23. */
static void input_scroll_up(void) {
  uint16_t from = INPUT_ROW_20, to = INPUT_ROW_19;
  for (int row = 0; row < 4; row++, from += 0x20, to += 0x20) copy_char_row(from, to);
  for (int col = 0; col < INPUT_ROW_CHARS; col++) rom_char(' ', (uint16_t)(INPUT_ROW_23 + col));
}

/* $864A: scroll rows 18-22 down into 19-23 (deleting back past the start
 * of a row). */
static void input_scroll_down(void) {
  uint16_t from = INPUT_ROW_22, to = INPUT_ROW_23;
  for (int row = 0; row < 5; row++, from -= 0x20, to -= 0x20) copy_char_row(from, to);
}

/* $85B8, without telling the host (the adapter's entry already has). */
static void input_line_body(uint8_t ch) {
  uint16_t pos = word_at(IN_POS);
  uint8_t cols;
  if (ch == 0x08) {
    /* Rub out the cursor and step back, into the row above if need be. */
    rom_char(' ', pos);
    pos = column_left(pos);
    cols = (uint8_t)(mem[IN_COLS] + 1);
    if (cols == INPUT_ROW_CHARS + 1) {
      pos = at_column(pos, 0xFF);
      input_scroll_down();
      cols = 1;
    }
  } else {
    bool new_row = ch == '\r';
    if (new_row) ch = ' '; /* rub out the cursor */
    else if (ch >= 'a' && ch <= 'z') ch &= 0x5F;
    rom_char(ch, pos);
    pos = column_right(pos);
    cols = (uint8_t)(mem[IN_COLS] - 1);
    if (new_row || cols == 0) {
      pos = at_column(pos, INPUT_ROW_23 & 0xFF);
      input_scroll_up();
      cols = INPUT_ROW_CHARS;
    }
  }
  mem[IN_COLS] = cols;
  set_word_at(IN_POS, pos);
  rom_char(mem[IN_CURSOR], pos);
}

void input_line_put(uint8_t ch) {
  device_at(0x85B8, ch);
  input_line_body(ch);
}

/* ---------- the text window (proportional font) ---------- */

/* A place on the text line: a screen address and a pixel offset (0-7)
 * into that byte. */
typedef struct {
  uint16_t addr;
  uint8_t bit;
} TextPos;

/* $87C9: print ch in the game's font (6 pixels wide) at p, each pixel
 * line shifted right p.bit pixels and merged into the byte there and the
 * next; and move p on 6 pixels. */
static TextPos prop_char(uint8_t ch, TextPos p) {
  uint16_t glyph = (uint16_t)(PROP_FONT + ch * 8);
  uint16_t addr = p.addr;
  for (int line = 0; line < 8; line++, addr += 0x100) {
    uint8_t g = mem[(uint16_t)(glyph + line)];
    mem[addr] = (uint8_t)((mem[addr] & ~(0xFF >> p.bit)) | g >> p.bit);
    if (p.bit) {
      uint16_t next = (uint16_t)(addr + 1);
      uint8_t keep = (uint8_t)~(0xFF << (8 - p.bit));
      mem[next] = (uint8_t)((mem[next] & keep) | (uint8_t)(g << (8 - p.bit)));
    }
  }
  p.bit += 6;
  if (p.bit >= 8) {
    p.bit -= 8;
    p.addr = column_right(p.addr);
  }
  return p;
}

/* $8761: step p back one character (6 pixels). */
static TextPos text_back(TextPos p) {
  if (p.bit >= 6) {
    p.bit -= 6;
  } else {
    p.bit += 2;
    p.addr = column_left(p.addr);
  }
  return p;
}

void scroll_text_window(void) {
  uint16_t from = 0x4020, to = 0x4000; /* rows 1-17 into 0-16 */
  for (int row = 0; row < 17; row++) {
    copy_char_row(from, to);
    from = next_char_row(from);
    to = next_char_row(to);
  }
  for (uint16_t a = 0x5800; a < 0x5800 + 17 * 32; a++) mem[a] = mem[a + 32]; /* the attributes */
  TextPos p = {TEXT_LINE, 1};
  for (int i = 0; i < TEXT_LINE_CHARS; i++) p = prop_char(' ', p);
}

/* The end of a line in the text window: copy it to the printer; wait
 * (after MORE_LINES more lines: for a key to go down or a while) for any
 * key to come up; scroll. The new line starts at p, with the number of
 * characters left on it returned. */
static uint8_t end_text_line(TextPos *p) {
  mem[TW_LAST] = 0;
  printer_copy_line();
  bool wait_key_up = true;
  if (mem[MORE_LINES] != 0) {
    mem[MORE_LINES]--;
  } else {
    wait_key_up = false;
    for (unsigned n = 0x8000; n > 0; n--)
      if (any_key_down(0x86E3)) {
        wait_key_up = true;
        break;
      }
  }
  if (wait_key_up)
    while (any_key_down(0x86F9)) {
    }
  scroll_text_window();
  p->addr = TEXT_LINE;
  p->bit = 1;
  return TEXT_LINE_CHARS;
}

/* $86A1, without telling the host (the adapter's entry already has). A
 * line starts with TW_INDENT spaces; capitals become small letters, except
 * the first letter after a CR or a full stop. */
static void text_window_body(uint8_t ch) {
  TextPos p = {word_at(TW_POS), mem[TW_BIT]};
  if (mem[TW_LAST] == 0)
    for (uint8_t n = mem[TW_INDENT]; n > 0; n--) {
      p = prop_char(' ', p);
      mem[TW_LEFT]--;
    }
  uint8_t left;
  if (ch == '\r') {
    mem[CAPITAL] = 1;
    left = end_text_line(&p);
  } else if (ch == 0x08) {
    p = text_back(p);
    p = prop_char(' ', p);
    p = text_back(p);
    left = (uint8_t)(mem[TW_LEFT] + 1);
  } else {
    if (ch >= 'A' && ch <= 'Z') ch |= 0x20;
    if (mem[CAPITAL] != 0 && ch >= 'a' && ch <= 'z') {
      ch &= ~0x20;
      mem[CAPITAL] = 0;
    }
    if (ch == '.') mem[CAPITAL]++;
    p = prop_char(ch, p);
    mem[TW_LAST] = ch;
    left = (uint8_t)(mem[TW_LEFT] - 1);
    if (left == 0) left = end_text_line(&p);
  }
  mem[TW_LEFT] = left;
  set_word_at(TW_POS, p.addr);
  mem[TW_BIT] = p.bit;
}

void text_window_put(uint8_t ch) {
  device_at(0x86A1, ch);
  text_window_body(ch);
}

/* After the wine, an "h" is printed after each "s". */
void print_char(uint8_t ch) {
  if (!can_print()) return;
  if (mem[V_PRINT_TO_INPUT]) {
    input_line_put(ch);
    return;
  }
  text_window_put(ch);
  if (mem[DRUNK] && (ch == 'S' || ch == 's')) text_window_put('H');
}

void print_newline(void) { print_char('\r'); }

/* ---------- the ZX Printer ---------- */

/* The printer's port is $FB; the high byte of the port address is
 * whatever the original had in A, kept here for the device's sake.
 * Reading: bit 0, ready for the next dot; bit 6, no printer; bit 7, the
 * stylus is at the paper. Writing: bit 7, a dot; bit 2, stop the motor;
 * bit 1, slow. */
#define PRINTER_PORT(hi) ((uint16_t)((hi) << 8 | 0xFB))

static void printer_send(uint8_t v) { device_out(PRINTER_PORT(v), v); }

/* Wait for the printer to be ready for a dot; hi, the port's high byte
 * for the first read (each read after uses the last value, shifted). */
static void printer_wait_ready(uint8_t hi, uint16_t at) {
  uint8_t v;
  do hi = (uint8_t)((v = device_in(PRINTER_PORT(hi), at)) >> 1);
  while (!(v & 1));
}

/* For each of the 8 pixel lines of character row 17: wait for the
 * stylus, send its 256 dots (with the motor bits the original works out
 * from the line number), then stop the motor. Gives up at once if there
 * is no printer. */
void printer_copy_line(void) {
  if (!mem[PRINTER_ON]) return;
  uint8_t a = 0;
  for (uint8_t line = 0; line < 8; line++) {
    printer_send(a);
    for (;;) { /* wait for the stylus */
      uint8_t v = device_in(PRINTER_PORT(a), 0x8B33);
      a = (uint8_t)(v << 1);
      if (v & 0x40) return; /* no printer */
      if (v & 0x80) break;
    }
    uint8_t motor = (uint8_t)(line << 1) & line;
    uint16_t addr = (uint16_t)(TEXT_LINE + line * 0x100);
    for (int col = 0; col < 32; col++, addr++)
      for (int bit = 7; bit >= 0; bit--) {
        uint8_t out = (uint8_t)((mem[addr] >> bit & 1) << 7 | motor >> 1);
        printer_wait_ready(out, 0x8B4E);
        printer_send(out);
      }
    printer_wait_ready(0, 0x8B5F);
    a = motor >> 1;
    printer_send(a);
  }
  printer_send(0x04);
}

/* ---------- the keyboard ---------- */

/* $8B93, without telling the host (the adapter's entry already has).
 * Each half-row is compared with the last scan (KEY_STATE); KEY_MASK
 * takes out keys never reported (CAPS SHIFT, SYMBOL SHIFT, 1, 3, 4, 9).
 * Of the keys gone down, the lowest of the last half-row with any is
 * reported, from KEY_MAP2 if CAPS SHIFT or SYMBOL SHIFT is held, else
 * KEY_MAP1. */
static uint8_t get_key_body(void) {
  uint16_t found = 0;
  int found_row = 0;
  for (int row = 0; row < 8; row++) {
    uint16_t port = (uint16_t)((~(0x100 << row) & 0xFF00) | 0xFE);
    uint8_t now = (uint8_t)((device_in(port, 0x8BA8) & 0x1F) | mem[KEY_MASK + row]);
    uint8_t went_down = (uint8_t)(~now & mem[KEY_STATE + row]);
    if (went_down) {
      found = (uint16_t)((port & 0xFF00) | (uint8_t)~went_down);
      found_row = row;
    }
    mem[KEY_STATE + row] = now;
  }
  set_word_at(KEY_FOUND, found);
  if (found == 0) return 0;
  int bit = 0; /* the lowest key gone down: the lowest 0 in the low byte */
  while ((uint8_t)found >> bit & 1) bit++;
  uint8_t key = (uint8_t)(found_row * 5 + bit);
  uint16_t map = KEY_MAP2;
  if (device_in(0xFEFE, 0x8BE3) & 0x01) /* CAPS SHIFT up */
    if (device_in(0x7FFE, 0x8BEB) & 0x02) map = KEY_MAP1; /* SYMBOL SHIFT up */
  return mem[(uint16_t)(map + key)];
}

uint8_t get_key(void) {
  device_at(0x8B93, 0);
  return get_key_body();
}

/* Wait for a key to go down, reading the keyboard as the original's
 * instruction at `at` does. Returns the keys read (bits 0-4, 0 down). */
static uint8_t wait_key_down(uint16_t at) {
  uint8_t keys;
  while ((keys = device_in(ALL_KEYS, at) & 0x1F) == 0x1F) {
  }
  return keys;
}

void wait_key_after_picture(void) {
  (void)wait_key_down(0x969B);
  device_out(0x07FE, 0x07); /* the border white */
}

void wait_key_after_death(void) { (void)wait_key_down(0x90E0); }

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ---------- adapters (for the faithful callers, while they remain) ----------
 * The host has already been told of the routine's own address on entry. */

/* $8576 CanPrint: Z if not (from the AND); A, HL and the rest kept. */
static void a_can_print(Cpu *c) {
  c->zf = !can_print();
  c->cf = 0;
}

/* $858B PrintChar: A kept; the flags are left as the original leaves them
 * (callers such as $97F9, JP $8583, pass them on): Z from the AND of the
 * gate, or after the wine the CP of the character with "S"/"s". */
static void a_print_char(Cpu *c) {
  (void)c;
  uint8_t ch = c->a;
  bool zf = false, cf = false;
  if (!can_print()) {
    zf = true;
  } else if (!mem[V_PRINT_TO_INPUT] && mem[DRUNK]) {
    zf = ch == 'S' || ch == 's';
    cf = !zf && ch < 's';
  }
  print_char(ch);
  c->zf = zf, c->cf = cf;
}

/* $8583 PrintCR: every register and flag kept. */
static void a_print_newline(Cpu *c) {
  (void)c;
  print_newline();
}

/* $85B8 InputLine: every register and flag kept. */
static void a_input_line(Cpu *c) {
  (void)c;
  input_line_body(c->a);
}

/* $860D, $864A: every register and flag kept. */
static void a_input_scroll_up(Cpu *c) { (void)c, input_scroll_up(); }
static void a_input_scroll_down(Cpu *c) { (void)c, input_scroll_down(); }

/* $867A RomChar: A at HL; HL one column on (INC L: its flags). */
static void a_rom_char(Cpu *c) {
  rom_char(c->a, get_hl(c));
  c->l = op_inc(c, c->l);
}

/* $86A1 TextWindow: every register and flag kept. */
static void a_text_window(Cpu *c) {
  (void)c;
  text_window_body(c->a);
}

/* $8761, $87C9: the text position in HL and C (A, from $8761, is C).
 * C, the pixel within the byte, is 0-7: every caller has it from $87C9
 * or $8761 (which keep it so) or sets it; the check does not mutate it. */
static void a_text_back(Cpu *c) {
  TextPos p = text_back((TextPos){get_hl(c), c->c});
  set_hl(c, p.addr), c->c = c->a = p.bit;
}
static void a_prop_char(Cpu *c) {
  TextPos p = prop_char(c->a, (TextPos){get_hl(c), c->c});
  set_hl(c, p.addr), c->c = p.bit;
}

/* $876B ScrollLine: every register and flag kept. */
static void a_scroll(Cpu *c) { (void)c, scroll_text_window(); }

/* $8B22 PrinterCopy: BC, DE, HL kept (its one caller reloads A). */
static void a_printer(Cpu *c) {
  (void)c;
  printer_copy_line();
}

/* $8B78 Pause: BC counted down to 0 (GetKey stores it). */
static void a_pause(Cpu *c) {
  c->a = c->b = c->c = 0;
  c->zf = 1, c->cf = 0;
}

/* $8B93 GetKey: the character in A (its caller tests it with AND A); BC,
 * DE, HL, IX kept. */
static void a_get_key(Cpu *c) {
  (void)c;
  c->a = get_key_body();
}

/* $969A WaitForKey2: A=7 (the border), flags from the last CP (NZ, C). */
static void a_wait_picture(Cpu *c) {
  (void)c;
  wait_key_after_picture();
  c->a = 0x07, c->zf = 0, c->cf = 1;
}

/* $90DF: wait for a key, then restart the game ($6C27); A the keys read
 * (NZ, C from the last CP). */
static void a_wait_death(Cpu *c) {
  (void)c;
  c->a = wait_key_down(0x90E0);
  c->zf = 0, c->cf = 1;
  cpu_tail(c, 0x6C27);
}

#define KEPT (OUT_REGS | OUT_ZF | OUT_CF)

const CleanRoutine screen_clean[] = {
    {0x8576, "can_print", a_can_print, KEPT},
    {0x8583, "print_newline", a_print_newline, KEPT},
    {0x858B, "print_char", a_print_char, KEPT},
    {0x85B8, "input_line_put", a_input_line, KEPT},
    {0x860D, "input_scroll_up", a_input_scroll_up, KEPT},
    {0x864A, "input_scroll_down", a_input_scroll_down, KEPT},
    {0x867A, "rom_char", a_rom_char, KEPT},
    {0x86A1, "text_window_put", a_text_window, KEPT},
    {0x8761, "text_back", a_text_back, OUT_REGS},
    {0x876B, "scroll_text_window", a_scroll, KEPT},
    {0x87C9, "prop_char", a_prop_char, KEPT, OUT_C},
    {0x8B22, "printer_copy_line", a_printer, OUT_BC | OUT_DE | OUT_HL | OUT_IX | OUT_IY},
    {0x8B78, "pause", a_pause, KEPT},
    {0x8B93, "get_key", a_get_key, OUT_REGS},
    {0x90DF, "wait_key_after_death", a_wait_death, KEPT},
    {0x969A, "wait_key_after_picture", a_wait_picture, KEPT},
    {0, NULL, NULL, 0},
};

const CleanScratch screen_scratch[] = {
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
