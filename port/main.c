/* The start of the game, the restart point and the main loop
 * ($6C00-$6DD5, $6FD3).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * The refresh register. The game takes its random seed from R ($6CA7), and
 * the Z80 adds one to R (low 7 bits) for every opcode fetch: one for a
 * plain instruction, two for a prefixed one (CB, DD, ED, FD), two for
 * every repeat of LDIR. So that the seed (and R at the next restart) is
 * what the original would have, this code adds to c->r what each original
 * instruction it stands for would: R(n) after each, with n the fetches.
 * The fetch of a CALL made with cpu_call_at is counted here; the callee's
 * own fetches are counted by the emulator if it runs as original code, or
 * not at all if it is a ported routine. Before the seed is taken, two such calls
 * ($9DBD, $70E2) are made: their fetches are known, so R is set after
 * them to what the original leaves, whichever way they ran. */
#include <stddef.h>

#include "cpu.h"
#include "routines.h"

#define STACK_TOP 0x5EFF     /* LD SP,$5EFF at the restart */
#define CLEAN_TABLES 0xF400  /* clean copy of $C11B ($615 bytes), then of $BA8A ($5D9) */
#define GAME_TABLES 0xC11B
#define GAME_TABLES2 0xBA8A
#define CLEAN_VARS 0x5F00    /* clean copy of $B6EB ($1D bytes), then of $CA84 ($BF) */
#define GAME_VARS 0xB6EB
#define TIMED_EVENTS 0xCA84
#define LOCATION_GFX 0xCC00  /* location graphics index, 3 bytes an entry */
#define TEXT_ONLY 0xB707     /* bit 3 of the N key at the title: 0 = held, text only */
#define RNG_SEED 0xB70E
#define SPEECH 0xB738        /* $C8 bytes */
#define INPUT_PROMPT 0x6FF2  /* "> LOOK" and $0D */
#define INPUT_BUFFER 0x6FF9
#define TOKENS 0x709C        /* the token list built from the input line, $40 bytes */
#define TOKEN_PTR 0xB6DC     /* where ParseSentence reads tokens */
#define IN_QUOTE 0xB71B      /* a quote has been opened */
#define WORD_START 0xB6DA    /* where the last word read starts */
#define SQUIGGLE_GFX 0x6DCC

/* Add n opcode fetches to R. */
#define R(n) (c->r = (uint8_t)((c->r & 0x80) | ((c->r + (n)) & 0x7F)))

static void hl_inc(Cpu *c) { set_hl(c, get_hl(c) + 1); }

/* The CALL at site, counting its fetch (cpu_call_at does not run the
 * instruction itself on the Z80). */
static void call(Cpu *c, uint16_t site) {
  R(1);
  cpu_call_at(c, site);
}

/* LDIR, counting its fetches. */
static void ldir_r(Cpu *c) {
  R(2 * get_bc(c));
  ldir(c);
}

/* $6FD3: clear the screen: border white, pixels 0, attributes $38 (black
 * ink on white paper). Like the original's LDIRs, it also writes $38 to
 * $5B00, the byte after the attributes. Keeps HL DE BC; A=7. */
static void m_clear_screen(Cpu *c) {
  push16(c, get_hl(c)), push16(c, get_de(c)), push16(c, get_bc(c));
  R(3);
  c->a = 0x07;
  c->out(c, (uint16_t)(c->a << 8 | 0xFE), c->a);
  R(2);
  set_hl(c, 0x4000), set_de(c, 0x4001), set_bc(c, 0x1800);
  c->mem[get_hl(c)] = 0x00;
  R(4);
  ldir_r(c);
  set_bc(c, 0x0300);
  c->mem[get_hl(c)] = 0x38;
  R(2);
  ldir_r(c);
  set_bc(c, pop16(c)), set_de(c, pop16(c)), set_hl(c, pop16(c));
  R(4); /* and the RET */
}

/* The opcode fetches of IndexIdTable ($9DBD) looking for id a in the
 * table at ix, up to and including its RET. */
static unsigned index_id_table_fetches(const Cpu *c, uint8_t a, uint16_t ix) {
  unsigned n = 7; /* EXX, PUSH IX, POP HL, LD B,A, LD E, LD D */
  for (uint16_t hl = ix;; hl += 3) {
    uint8_t id = c->mem[hl];
    if (id == a) { n += 3; break; }
    if (id == 0xFF) { n += 5; break; }
    n += 7;
  }
  return n + 6; /* PUSH HL, POP IX, CP, EXX, RET */
}

/* The CALL $70E2 (Blanker: zero B bytes from HL) at site. R is set to what
 * the original leaves (the CALL, XOR A, 3 for each byte, RET), whether or
 * not Blanker ran as original code. */
static void call_blanker(Cpu *c, uint16_t site) {
  uint8_t r = c->r, b = c->b;
  cpu_call_at(c, site);
  c->r = r, R(1 + 1 + 3 * (b ? b : 256) + 1);
}

/* $6CCD-$6D12 and the main loop, from $6CCD. */
static void m_main(Cpu *c);

/* $6C27: (re)start the game: reset the stack, black out the first
 * location's border and paper, copy the clean tables back over the
 * game's, wait for a key (N held: text only), take the random seed from
 * R, set the variables and go on to the screen and the main loop. Never
 * returns; it is also where every death, QUIT and tape error ends up. */
static void m_restart(Cpu *c) {
  R(1); /* DI */
  c->sp = STACK_TOP;
  c->frame_sp = STACK_TOP; /* the host: this is the outermost frame now */
  R(1);
  c->ix = LOCATION_GFX;
  c->a = 0x05;
  R(3);
  {
    uint8_t r = c->r;
    unsigned n = index_id_table_fetches(c, c->a, c->ix);
    cpu_call_at(c, 0x6C31); /* IndexIdTable */
    c->r = r, R(1 + n);
  }
  c->l = c->mem[(uint16_t)(c->ix + 1)];
  c->h = c->mem[(uint16_t)(c->ix + 2)];
  R(4);
  c->mem[get_hl(c)] = 0x00; /* border */
  hl_inc(c);
  c->mem[get_hl(c)] = 0x00; /* paper */
  R(3);
  set_hl(c, CLEAN_TABLES), set_de(c, GAME_TABLES), set_bc(c, 0x0615);
  R(3);
  ldir_r(c);
  set_de(c, GAME_TABLES2), set_bc(c, 0x05D9);
  R(2);
  ldir_r(c);
  set_hl(c, CLEAN_VARS), set_de(c, GAME_VARS), set_bc(c, 0x001D);
  R(3);
  ldir_r(c);
  set_de(c, TIMED_EVENTS), set_bc(c, 0x00BF);
  R(2);
  ldir_r(c);
  c->a = op_xor(c, c->a, c->a);
  c->out(c, (uint16_t)(c->a << 8 | 0xFE), c->a);
  c->a = 0x38;
  c->mem[0x5C48] = c->a; /* BORDCR */
  R(4);
  /* $6C6D: wait for any key. */
  do {
    c->a = op_xor(c, c->a, c->a);
    c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFE), 0x6C6E);
    c->a = op_and(c, c->a, 0x1F);
    op_cp(c, 0x1F);
    R(5);
  } while (c->zf);
  c->a = 0x7F;
  c->a = c->in_at(c, (uint16_t)(c->a << 8 | 0xFE), 0x6C78);
  c->a = op_and(c, c->a, 0x08); /* the N key */
  c->mem[TEXT_ONLY] = c->a;
  R(4);
  wr16(c, 0x85B4, 0x50E0);
  c->a = 0x2B;
  c->mem[0x85B6] = c->a;
  wr16(c, 0x869C, 0x5020);
  c->a = 0x01;
  c->mem[0x869E] = c->a;
  c->a = 0x20;
  c->mem[0x85B3] = c->a;
  c->a = 0x2A;
  c->mem[0x869B] = c->a;
  R(12);
  c->b = 0xC8;
  set_hl(c, SPEECH);
  R(2);
  call_blanker(c, 0x6CA4);
  R(2); /* LD A,R: A gets R after its own two fetches */
  c->a = c->r;
  c->sf = c->a >> 7, c->zf = c->a == 0, c->hf = 0, c->nf = 0, c->pf = 0; /* P/V = IFF2: DI */
  c->mem[RNG_SEED] = c->a;
  R(1);
  cpu_at(c, 0x6CAC); /* the seed is set (the test machine may replace it) */
  c->a = op_xor(c, c->a, c->a);
  c->mem[0x869F] = c->a;
  c->mem[0x86A0] = c->a;
  c->mem[0xB71A] = c->a;
  c->mem[0xB700] = c->a;
  c->mem[0xB6F2] = c->a;
  c->a = 0x01;
  c->mem[0xB702] = c->a;
  c->mem[0xB6FA] = c->a;
  c->mem[0xB704] = c->a;
  set_hl(c, 0x0000);
  wr16(c, 0xB6F7, get_hl(c));
  R(12);
  m_main(c);
}

/* $6C00: the entry point: keep a clean copy of the tables a game changes
 * (for the restart to copy back), and start. */
static void m_start(Cpu *c) {
  R(1); /* DI */
  set_de(c, CLEAN_TABLES), set_hl(c, GAME_TABLES), set_bc(c, 0x0615);
  R(3);
  ldir_r(c);
  set_hl(c, GAME_TABLES2), set_bc(c, 0x05D9);
  R(2);
  ldir_r(c);
  set_de(c, CLEAN_VARS), set_hl(c, GAME_VARS), set_bc(c, 0x001D);
  R(3);
  ldir_r(c);
  set_hl(c, TIMED_EVENTS), set_bc(c, 0x00BF);
  R(2);
  ldir_r(c);
  m_restart(c);
}

/* $6CCD: clear the screen, draw the squiggle between the picture and the
 * text (5 pixel lines of a 4-byte pattern), and unless $B706 is $FF
 * (a game loaded from tape), start the new game ($97AD) and put "LOOK" on
 * the input line as if typed. Then the main loop. */
static void m_main(Cpu *c) {
  call(c, 0x6CCD); /* ClearScreen */
  set_hl(c, 0x5140), set_de(c, SQUIGGLE_GFX);
  c->c = 0x05;
  R(3);
  do {
    c->b = 0x10;
    push16(c, get_hl(c));
    R(2);
    do {
      c->a = c->mem[get_de(c)];
      c->mem[get_hl(c)] = c->a;
      hl_inc(c), set_de(c, get_de(c) + 1);
      c->a = c->mem[get_de(c)];
      c->mem[get_hl(c)] = c->a;
      hl_inc(c), set_de(c, get_de(c) - 1);
      R(9);
    } while (--c->b);
    set_de(c, get_de(c) + 2);
    set_hl(c, pop16(c));
    c->h = op_inc(c, c->h);
    c->c = op_dec(c, c->c);
    R(6);
  } while (!c->zf);
  c->a = 0x11;
  c->mem[0xB716] = c->a;
  c->a = c->mem[0xB706];
  c->a = op_inc(c, c->a);
  R(5);
  if (c->zf) {
    call(c, 0x6CF7); /* $97AD: a new game */
    set_hl(c, INPUT_PROMPT);
    R(1);
    do { /* print "> LOOK" on the input line */
      c->a = c->mem[get_hl(c)];
      R(1);
      call(c, 0x6CFE);
      hl_inc(c);
      op_cp(c, 0x0D);
      R(3);
    } while (!c->zf);
    set_hl(c, INPUT_PROMPT + 2), set_de(c, INPUT_BUFFER), set_bc(c, 0x0005);
    R(3);
    ldir_r(c);
    R(1); /* JR $6D22 */
    goto L6D22;
  }
  for (;;) {
    /* $6D13: read a command. */
    c->a = 0x01;
    c->mem[0xB705] = c->a;
    c->a = 0x09;
    c->mem[0xB716] = c->a;
    R(4);
    call(c, 0x6D1D); /* ReadLine */
    R(1);
    if (c->zf) goto L6D8A; /* nothing new: the last command again */
  L6D22:
    /* Split it into tokens at TOKENS: two bytes a word (B C), with a
     * $B0 0 token added before a closing quote unless one is there. */
    set_hl(c, TOKENS);
    c->b = 0x40;
    R(2);
    call_blanker(c, 0x6D27);
    set_hl(c, INPUT_BUFFER);
    c->iy = TOKENS;
    R(3);
    do {
      call(c, 0x6D31); /* GetWord */
      op_cp(c, 0xD0);
      R(2);
      if (c->zf) goto L6DA2; /* not in the dictionary */
      op_cp(c, 0x90);
      R(2);
      if (c->zf) {
        c->a = op_and(c, c->b, 0x0F);
        c->a = op_or(c, c->a, c->c);
        R(4);
        if (c->zf) { /* a quote */
          c->a = c->mem[IN_QUOTE];
          c->a = op_and(c, c->a, c->a);
          R(3);
          if (c->zf) { /* it opens */
            c->a = op_inc(c, c->a);
            c->mem[IN_QUOTE] = c->a;
            R(3);
          } else { /* it closes */
            c->a = op_dec(c, c->a);
            c->mem[IN_QUOTE] = c->a;
            c->a = c->mem[(uint16_t)(c->iy - 2)];
            c->a = op_and(c, c->a, 0xF0);
            op_cp(c, 0xB0);
            R(7); /* DEC A, LD, LD A,(IY-2), AND, CP, JR */
            if (!c->zf) {
              op_cp(c, 0xA0);
              R(2);
              if (!c->zf) {
                c->a = 0xB0;
                c->mem[c->iy] = c->a;
                c->a = op_xor(c, c->a, c->a);
                c->mem[(uint16_t)(c->iy + 1)] = c->a;
                c->iy += 2;
                R(10);
              }
            }
          }
        }
      }
      /* $6D6C */
      c->mem[c->iy] = c->b;
      c->mem[(uint16_t)(c->iy + 1)] = c->c;
      c->iy += 2;
      c->a = c->d;
      op_cp(c, 0xC0);
      R(11);
    } while (!c->zf); /* D=$C0: the end of the line */
    c->a = c->mem[IN_QUOTE];
    c->a = op_and(c, c->a, c->a);
    R(3);
    if (!c->zf) { /* a quote left open */
      c->a = op_xor(c, c->a, c->a);
      c->mem[IN_QUOTE] = c->a;
      R(2);
      call(c, 0x6D85);
      R(1);
      continue;
    }
  L6D8A:
    set_hl(c, TOKENS);
    wr16(c, TOKEN_PTR, get_hl(c));
    R(2);
    do { /* parse and carry out each sentence of the command */
      call(c, 0x6D90); /* ParseSentence */
      R(1);
      if (!c->zf) break;
      call(c, 0x6D96); /* Execute */
      c->a = c->mem[0xB705];
      c->a = op_and(c, c->a, c->a);
      R(3);
    } while (!c->zf);
    if (c->zf) R(1); /* JP $6D13 */
    continue;
  L6DA2:
    /* "i do not know the word " and the word, in quotes. */
    set_hl(c, 0xAD93);
    c->a = 0x01;
    c->mem[0xB701] = c->a;
    R(3);
    call(c, 0x6DAA);
    set_hl(c, rd16(c, WORD_START));
    R(1);
    for (;;) {
      c->a = c->mem[get_hl(c)];
      op_cp(c, 0x0D);
      R(3);
      if (c->zf) break;
      op_cp(c, 0x22);
      R(2);
      if (c->zf) break;
      call(c, 0x6DB9);
      hl_inc(c);
      op_cp(c, 0x20);
      R(3);
      if (c->zf) break;
    }
    c->a = 0x22;
    R(1);
    call(c, 0x6DC3);
    call(c, 0x6DC6);
    R(1);
  }
}

const PortRoutine main_routines[] = {
    {0x6C00, "Start", m_start, 0, PORT_TOP},
    {0x6C27, "ReStart", m_restart, 0, PORT_TOP},
    {0x6FD3, "ClearScreen", m_clear_screen, OUT_REGS | OUT_ZF | OUT_CF},
    {0, NULL, NULL, 0},
};
