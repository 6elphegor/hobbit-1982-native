#include "spectrum.h"

#include <stdio.h>
#include <string.h>

#include "../common/tzx.h"

/* ---------- memory and ports ---------- */

static uint8_t rd(void *ud, uint16_t a) {
  Spectrum *s = ud;
  if (s->trace_rom && a < 0x3D00)
    fprintf(stderr, "[rom] read $%04X at pc=$%04X\n", a, s->ipc);
  return s->mem[a];
}

static void wr(void *ud, uint16_t a, uint8_t v) {
  Spectrum *s = ud;
  if (a >= 0x4000) s->mem[a] = v;
}

/* IN instructions that wait for a key to go down; the typist may only
 * start a key press at one of these (or inside GetKey). */
static bool in_getkey(uint16_t pc) { return pc >= 0x8B93 && pc <= 0x8BFA; }

static bool listening(uint16_t pc) {
  if (in_getkey(pc)) return true;
  switch (pc) {
  case 0x6C6E: /* press any key to start */
  case 0x8395: /* game over / restart */
  case 0x84A8: /* tape error */
  case 0x84C3: /* NormalAnyKey (save prompts) */
  case 0x856A: /* verify error */
  case 0x90E0: /* you are dead */
  case 0x969B: /* WaitForKey2 */
    return true;
  }
  return false;
}

static void typist_tick(Spectrum *s);

static uint8_t port_in(z80 *z, uint16_t port) {
  Spectrum *s = z->userdata;
  if ((port & 1) == 0) {
    s->reads++;
    if (s->on_read) s->on_read(s);
    s->waiting_for_input = listening(s->ipc);
    /* GetKey reports keys that were up on its previous scan, so a key may
     * only go down from the second scan of a wait. */
    if (!s->waiting_for_input) s->ready_at = s->getkey_calls + 2;
    typist_tick(s);
    uint8_t keys = 0;
    for (int r = 0; r < 8; r++)
      if (!(port & (0x100 << r))) keys |= s->live[r] | s->typed[r];
    return 0xE0 | (~keys & 0x1F);
  }
  return 0xFF; /* ZX Printer ($FB) absent, floating bus elsewhere */
}

uint8_t spec_port_in(Spectrum *s, uint16_t port) { return port_in(&s->cpu, port); }

static void port_out(z80 *z, uint16_t port, uint8_t v) {
  Spectrum *s = z->userdata;
  if ((port & 1) == 0) s->border = v & 7;
}

/* ---------- keyboard ---------- */

static const char *const MATRIX[8] = {"^ZXCV", "ASDFG", "QWERT", "12345",
                                      "09876", "POIUY", "\nLKJH", " $MNB"};

static bool find_key(char ch, int *row, int *bit) {
  for (int r = 0; r < 8; r++)
    for (int b = 0; b < 5; b++)
      if (MATRIX[r][b] == ch) {
        *row = r;
        *bit = b;
        return true;
      }
  return false;
}

int spec_char_keys(char ch, int rows[2], int bits[2]) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  /* Symbol shift combinations from the game's KeyboardMap2 ($8C23). */
  const char *sym = strchr(".,\"@", ch);
  if (ch && sym) {
    static const char base[] = "MNP2";
    rows[0] = ROW_SPACE_B, bits[0] = 1;
    find_key(base[sym - ".,\"@"], &rows[1], &bits[1]);
    return 2;
  }
  /* The game's own keys (KeyboardMap1/2 at $8BFB/$8C23): 0 deletes (as
   * does 5), CAPS SHIFT+0 clears the line. As the first key of a line,
   * 5/0, 6, 7, 8 type W, S, N, E and ENTER. */
  if (ch == KEY_DELETE) ch = '0';
  if (ch == KEY_CLEAR) {
    rows[0] = ROW_CAPS_V, bits[0] = 0;
    rows[1] = ROW_0_6, bits[1] = 0;
    return 2;
  }
  if (ch != '^' && ch != '$' && find_key(ch, &rows[0], &bits[0])) return 1;
  return 0;
}

void spec_key(Spectrum *s, int row, int bit, bool down) {
  if (down)
    s->live[row] |= 1 << bit;
  else
    s->live[row] &= ~(1 << bit);
}

bool spec_type(Spectrum *s, const char *text) {
  for (; *text; text++) {
    size_t next = (s->qtail + 1) % sizeof s->queue;
    if (next == s->qhead) return false;
    s->queue[s->qtail] = *text;
    s->qtail = next;
  }
  return true;
}

bool spec_in_getkey(const Spectrum *s) { return in_getkey(s->ipc); }

void spec_stop(Spectrum *s) {
  fflush(stdout);
  _longjmp(*s->stop_jmp, 1);
}

bool spec_typist_idle(const Spectrum *s) {
  return s->qhead == s->qtail && s->tstate == 0;
}

/* The typist counts in keyboard reads and GetKey scans, not T-states, so
 * replacing original routines with native ones (which take no emulated
 * time) does not change what the game sees.
 *
 * A key pressed in GetKey is held for TYPIST_HOLD_SCANS scans, or until
 * the game reads the keyboard anywhere else (it has gone off to act on the
 * key). Elsewhere (press-any-key loops) it is held for TYPIST_HOLD_READS
 * reads. After release, the next key waits TYPIST_GAP_SCANS scans, or
 * TYPIST_HOLD_READS reads outside GetKey. */
#define TYPIST_HOLD_SCANS 4
#define TYPIST_GAP_SCANS 4
#define TYPIST_HOLD_READS 500

static void typist_tick(Spectrum *s) {
  bool getkey = in_getkey(s->ipc);
  if (s->tstate == 1 &&
      (s->press_in_getkey ? !getkey || s->getkey_calls - s->mark_scans >= TYPIST_HOLD_SCANS
                          : s->reads - s->mark_reads >= TYPIST_HOLD_READS)) {
    memset(s->typed, 0, sizeof s->typed);
    s->tstate = 2;
    s->mark_scans = s->getkey_calls;
    s->mark_reads = s->reads;
  }
  if (s->tstate == 2 && (s->getkey_calls - s->mark_scans >= TYPIST_GAP_SCANS ||
                         (!getkey && s->reads - s->mark_reads >= TYPIST_HOLD_READS)))
    s->tstate = 0;
  bool ready = s->tstate == 0 && s->waiting_for_input && (!getkey || s->getkey_calls >= s->ready_at);
  if (ready && s->qhead == s->qtail && s->need_input) {
    s->idle_steps = 0;
    s->need_input(s);
  }
  if (ready && s->qhead != s->qtail) {
    char ch = s->queue[s->qhead];
    s->qhead = (s->qhead + 1) % sizeof s->queue;
    int rows[2], bits[2];
    int n = spec_char_keys(ch, rows, bits);
    if (n == 0) {
      fprintf(stderr, "[typist] cannot type character $%02X, skipped\n", (uint8_t)ch);
      return;
    }
    for (int i = 0; i < n; i++) s->typed[rows[i]] |= 1 << bits[i];
    if (s->trace_keys)
      fprintf(stderr, "[key] $%02X at pc=$%04X scans=%lu\n", (uint8_t)ch, s->ipc, s->getkey_calls);
    s->tstate = 1;
    s->press_in_getkey = getkey;
    s->mark_scans = s->getkey_calls;
    s->mark_reads = s->reads;
  }
}

/* ---------- tape traps ---------- */

static void ret(Spectrum *s) {
  z80 *z = &s->cpu;
  z->pc = s->mem[z->sp] | (s->mem[(uint16_t)(z->sp + 1)] << 8);
  z->sp += 2;
}

/* Tape operations come in groups: SAVE writes four blocks in a row, LOAD
 * and the verify after SAVE read four in a row. A group ends when the
 * keyboard is read (the game asks for a key between them, and for the
 * next command after), which does not depend on emulated time. */
static bool same_tape_group(const Spectrum *s, int op) {
  return s->tape_last_op == op && s->reads == s->tape_last_reads;
}

bool spec_tape_save(Spectrum *s, uint8_t flag, uint16_t start, uint16_t len) {
  bool append = same_tape_group(s, 1);
  FILE *f = fopen(s->tape_path, append ? "ab" : "wb");
  bool ok = f != NULL;
  if (f) {
    uint8_t hdr[3] = {(uint8_t)(len + 2), (uint8_t)((len + 2) >> 8), flag};
    uint8_t sum = flag;
    fwrite(hdr, 1, 3, f);
    for (uint16_t i = 0; i < len; i++) {
      uint8_t b = s->mem[(uint16_t)(start + i)];
      sum ^= b;
      fputc(b, f);
    }
    fputc(sum, f);
    ok = fclose(f) == 0;
  }
  s->tape_last_op = 1;
  s->tape_last_reads = s->reads;
  return ok;
}

/* SA-BYTES: A=flag, IX=start, DE=length. Saves start a fresh file. */
static void trap_save(Spectrum *s) {
  z80 *z = &s->cpu;
  z->cf = spec_tape_save(s, z->a, z->ix, (uint16_t)((z->d << 8) | z->e));
  ret(s);
}

bool spec_tape_load(Spectrum *s, uint8_t flag, uint16_t start, uint16_t len, bool load) {
  bool ok = false;
  if (!same_tape_group(s, 2)) s->tape_pos = 0;
  s->tape_last_op = 2;
  FILE *f = fopen(s->tape_path, "rb");
  if (f && fseek(f, s->tape_pos, SEEK_SET) == 0) {
    uint8_t hdr[3];
    if (fread(hdr, 1, 3, f) == 3) {
      uint16_t blen = hdr[0] | (hdr[1] << 8);
      static uint8_t buf[0x10000];
      if (blen >= 2 && fread(buf, 1, blen - 1, f) == (size_t)(blen - 1)) {
        s->tape_pos += 2 + blen;
        uint8_t sum = hdr[2];
        for (int i = 0; i < blen - 1; i++) sum ^= buf[i];
        ok = hdr[2] == flag && blen - 2 == len && sum == 0;
        for (uint16_t i = 0; ok && i < len; i++) {
          uint16_t a = start + i;
          if (load) {
            if (a >= 0x4000) s->mem[a] = buf[i];
          } else if (s->mem[a] != buf[i]) {
            ok = false;
          }
        }
      }
    }
  }
  if (f) fclose(f);
  s->tape_last_reads = s->reads;
  return ok;
}

/* LD-BYTES: A=flag, IX=start, DE=length, carry set to load, reset to
 * verify. Reads the next block of the tape file; a new group starts from
 * the beginning of the file (rewinding the tape). */
static void trap_load(Spectrum *s) {
  z80 *z = &s->cpu;
  z->cf = spec_tape_load(s, z->a, z->ix, (uint16_t)((z->d << 8) | z->e), z->cf);
  ret(s);
}

/* ---------- machine ---------- */

void spec_setup(Spectrum *s) {
  memset(s, 0, sizeof *s);
  memset(s->mem, 0xFF, 0x4000);
  z80_init(&s->cpu);
  s->cpu.read_byte = rd;
  s->cpu.write_byte = wr;
  s->cpu.port_in = port_in;
  s->cpu.port_out = port_out;
  s->cpu.userdata = s;
  s->cpu.pc = HOBBIT_ENTRY;
  s->cpu.sp = 0x5EFF;
  s->seed = -1;
  s->ready_at = 2;
  s->initial_seed = -1;
  snprintf(s->tape_path, sizeof s->tape_path, "hobbit-save.tap");
}

int spec_init(Spectrum *s, const char *tzx_path, const char *rom_path, char *err, size_t errlen) {
  spec_setup(s);
  if (hobbit_load_tzx(tzx_path, s->mem, err, errlen) != 0) return -1;

  if (rom_path) {
    FILE *f = fopen(rom_path, "rb");
    size_t n = f ? fread(s->mem, 1, 0x4000, f) : 0;
    if (f) fclose(f);
    if (n != 0x4000) {
      snprintf(err, errlen, "%s: expected a 16K Spectrum 48K ROM", rom_path);
      return -1;
    }
    if (crc32_buf(s->mem, 0x4000) != 0xDDEE531FU)
      fprintf(stderr, "warning: %s is not the standard Sinclair 48K ROM; random numbers will "
                      "differ from real hardware\n", rom_path);
  } else {
    /* The game prints the input line with the ROM CHARSET (PrintChar,
     * $867A). Without a ROM, use the game's own font ($8822 = space). */
    memcpy(s->mem + 0x3D00, s->mem + 0x8822, 0x300);
  }
  return 0;
}

void spec_step(Spectrum *s) {
  z80 *z = &s->cpu;
  if (s->idle_limit && ++s->idle_steps > s->idle_limit) {
    s->hung = true;
    spec_stop(s);
  }
  if (s->hook && s->hook(s)) return;
  s->ipc = z->pc;
  switch (z->pc) {
  case ROM_SA_BYTES: trap_save(s); return;
  case ROM_LD_BYTES: trap_load(s); return;
  }
  spec_at(s, z->pc);
  if (s->no_cpu) {
    s->on_missing(s, z->pc);
    spec_stop(s);
  }
  z80_step(z);
}

void spec_at(Spectrum *s, uint16_t addr) {
  z80 *z = &s->cpu;
  switch (addr) {
  case HOOK_PRINT_CHAR:
    if (s->on_char) s->on_char(s->ud, 0, z->a);
    break;
  case HOOK_PRINT_INPUT:
    if (s->on_char) s->on_char(s->ud, 1, z->a);
    break;
  case HOOK_GETKEY:
    s->getkey_calls++;
    break;
  case HOOK_SEED_SET:
    if (s->seed >= 0) s->mem[ADDR_RNG_SEED] = (uint8_t)s->seed;
    s->initial_seed = s->mem[ADDR_RNG_SEED];
    break;
  }
}

void spec_port_out(Spectrum *s, uint16_t port, uint8_t v) { port_out(&s->cpu, port, v); }

void spec_run(Spectrum *s, unsigned long t) {
  unsigned long end = s->cpu.cyc + t;
  while (s->cpu.cyc < end) spec_step(s);
}

/* ---------- display ---------- */

static const uint32_t PALETTE[16] = {
    0x000000, 0x0000D7, 0xD70000, 0xD700D7, 0x00D700, 0x00D7D7, 0xD7D700, 0xD7D7D7,
    0x000000, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF};

void spec_render(const Spectrum *s, uint32_t *px, bool flash_phase) {
  for (int i = 0; i < SPEC_SCREEN_W * SPEC_SCREEN_H; i++) px[i] = PALETTE[s->border];
  for (int y = 0; y < 192; y++) {
    uint32_t *row = px + (y + SPEC_BORDER_Y) * SPEC_SCREEN_W + SPEC_BORDER_X;
    int line = 0x4000 | ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2);
    for (int cx = 0; cx < 32; cx++) {
      uint8_t bits = s->mem[line + cx];
      uint8_t attr = s->mem[0x5800 + (y >> 3) * 32 + cx];
      int bright = (attr & 0x40) >> 3;
      uint32_t ink = PALETTE[(attr & 7) | bright], paper = PALETTE[((attr >> 3) & 7) | bright];
      if ((attr & 0x80) && flash_phase) {
        uint32_t t = ink;
        ink = paper;
        paper = t;
      }
      for (int b = 0; b < 8; b++) row[cx * 8 + b] = (bits & (0x80 >> b)) ? ink : paper;
    }
  }
}
