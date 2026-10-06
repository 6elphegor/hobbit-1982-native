/* The commands of their own: SAVE, LOAD, QUIT, HELP, SCORE, PAUSE, and
 * carrying on after them ($82B3-$8575, not the special words ported in
 * sentence.c).
 * Translated from pobtastic/hobbit; see docs/PORTING.md.
 *
 * The commands are special words (class $90): the parser ($8251, in
 * sentence.c) finds their handler in the table at $8271 and jumps to it
 * with its own return address on top of the stack. A handler finishes by
 * jumping to $82B3 (carry on with the sentence) or by restarting the game
 * ($6C27).
 *
 * The saved game is four data blocks (flag $FF, no headers), saved with
 * the ROM's SA-BYTES ($04C2) and loaded or verified with LD-BYTES ($0556):
 *   $B6EB, $001D bytes: the game's variables (the 3 bytes at $B6EB are
 *          swapped with $C9E2 around a save and a load, see below);
 *   $C11B, $0615 bytes: the objects (and characters);
 *   $CA84, $00BF bytes: the timed events and what follows them;
 *   $BA8A, $05D9 bytes: the locations.
 * A save first copies the 3 bytes at $C9E2 over $B6EB, so they are what is
 * saved in their place; a load copies the 3 loaded bytes at $B6EB back to
 * $C9E2.
 *
 * Labels keep the original addresses. Interrupts are not modelled, so DI
 * ($8488, $8553) does nothing here. */
#include <stddef.h>

#include "cpu.h"
#include "routines.h"

/* Variables. */
#define ACTOR 0xB6EA      /* who is doing it: 0 = you */
#define SCORE 0xB6F7      /* the score, in tenths of a percent (word) */
#define VAR_B701 0xB701   /* set to 1 before some messages (0 before the score) */
#define VAR_B704 0xB704   /* cleared after printing the score digits */
#define PLAYER_LOC 0xC12B /* your location */
#define SAVED_B6EB 0xC9E2 /* 3 bytes swapped with $B6EB by SAVE and LOAD */

/* Save blocks: start and length. */
#define BLOCK_VARS 0xB6EB
#define LEN_VARS 0x001D
#define BLOCK_OBJECTS 0xC11B
#define LEN_OBJECTS 0x0615
#define BLOCK_EVENTS 0xCA84
#define LEN_EVENTS 0x00BF
#define BLOCK_LOCATIONS 0xBA8A
#define LEN_LOCATIONS 0x05D9

/* Tables. */
#define HELP_TABLE 0x83CD /* location id, message address; $FF ends */

/* Messages (through $72DD, PrintMsg). */
#define MSG_MASTERED 0xB454   /* "you have mastered " */
#define MSG_OF_ADVENTURE 0xB462 /* "% of this adventure" */
#define MSG_DOING_FINE 0xB467 /* "you're doing fine" (HELP anywhere else) */
#define MSG_START_TAPE 0xB342 /* "start tape then press any key" */
#define MSG_TAPE_RESTART 0xB357 /* "tape error - hit any key to restart program" */
#define MSG_TAPE_CONTINUE 0xB381 /* "tape error - hit any key to continue" */
#define MSG_REWIND 0xB3A4     /* "rewind and prepare tape for verification -- then hit any key" */

/* Code elsewhere. */
#define R_RESTART 0x6C27  /* restart the game */
#define R_CONTINUE 0x82B3 /* carry on with the sentence after a special word */
#define R_PARSE_ON 0x75B4 /* the parser's loop over the words */

#define KEYBOARD 0x00FE /* IN A,($FE) with A = 0: all half-rows */
#define BORDER(a) ((uint16_t)((a) << 8 | 0xFE))

/* IN A,($FE) with A = 0 at pc, keeping bits 0-4: CP $1F gives Z when no
 * key is down. */
static void read_keys(Cpu *c, uint16_t pc) {
  c->a = 0;
  flags_logic(c, false); /* XOR A */
  c->a = c->in_at(c, KEYBOARD, pc);
  c->a = op_and(c, c->a, 0x1F);
  op_cp(c, 0x1F);
}

/* $8391 QUIT: show the score, wait for a key, restart the game. */
static void quit(Cpu *c) {
  cpu_call_at(c, 0x8391); /* CALL $83F5 */
  do read_keys(c, 0x8395);
  while (c->zf);
  cpu_tail(c, R_RESTART);
}

/* $83A0 HELP: a hint for some locations, "you're doing fine" elsewhere.
 * Only for you: when someone else is doing it, the word is skipped. */
static void help(Cpu *c) {
  c->a = c->mem[ACTOR];
  c->a = op_and(c, c->a, c->a);
  if (!c->zf) {
    cpu_tail(c, R_PARSE_ON);
    return;
  }
  push16(c, get_hl(c));
  push16(c, c->ix);
  set_hl(c, MSG_DOING_FINE);
  c->a = c->mem[PLAYER_LOC];
  c->ix = HELP_TABLE;
  cpu_call_at(c, 0x83B4); /* CALL $9DBD: find A in the table at IX */
  if (!c->zf) {
    c->l = c->mem[(uint16_t)(c->ix + 1)];
    c->h = c->mem[(uint16_t)(c->ix + 2)];
  }
  /* $83BF */
  c->a = 1;
  c->mem[VAR_B701] = c->a;
  cpu_call_at(c, 0x83C4); /* CALL $72DD */
  c->ix = pop16(c);
  set_hl(c, pop16(c));
  cpu_tail(c, R_CONTINUE);
}

/* $83EF SCORE: show the score and carry on. */
static void score_cmd(Cpu *c) {
  cpu_call_at(c, 0x83EF); /* CALL $83F5 */
  cpu_tail(c, R_CONTINUE);
}

/* $83F5 GameOver_Start: "you have mastered NN.N% of this adventure", from
 * the score at $B6F7 (tenths of a percent; no leading zero in the tens). */
static void show_score(Cpu *c) {
  push16(c, get_hl(c));
  push16(c, get_de(c));
  c->a = 0;
  flags_logic(c, false); /* XOR A */
  c->mem[VAR_B701] = c->a;
  set_hl(c, MSG_MASTERED);
  cpu_call_at(c, 0x83FE); /* CALL $72DD */
  set_hl(c, rd16(c, SCORE));
  set_de(c, 100);
  cpu_call_at(c, 0x8407); /* CALL $842E: the tens digit */
  cpu_call_at(c, 0x840A); /* CALL NZ,$858B: print it, unless '0' */
  set_de(c, 10);
  cpu_call_at(c, 0x8410); /* CALL $842E: the units */
  cpu_call_at(c, 0x8413); /* CALL $858B */
  c->a = '.';
  cpu_call_at(c, 0x8418); /* CALL $858B */
  c->a = c->l;
  c->a = op_add(c, c->a, '0', 0);
  cpu_call_at(c, 0x841E); /* CALL $858B: the tenths */
  c->a = 0;
  flags_logic(c, false); /* XOR A */
  c->mem[VAR_B704] = c->a;
  set_hl(c, MSG_OF_ADVENTURE);
  cpu_call_at(c, 0x8428); /* CALL $72DD */
  set_de(c, pop16(c));
  set_hl(c, pop16(c));
}

/* $842E: A = '0' + HL / DE, HL = HL % DE; CP '0' (Z for a zero digit). */
static void digit(Cpu *c) {
  c->a = 0x2F;
  uint16_t hl = get_hl(c), de = get_de(c);
  do {
    c->a = op_inc(c, c->a);
    c->a = op_and(c, c->a, c->a);
    hl = op_sbc16(c, hl, de);
  } while (!c->cf);
  hl = op_add16(c, hl, de);
  set_hl(c, hl);
  op_cp(c, 0x30);
}

/* $843A PAUSE: green border until a key is pressed and let go. */
static void pause_cmd(Cpu *c) {
  c->a = 4;
  c->out(c, BORDER(c->a), c->a);
  cpu_call_at(c, 0x843E); /* CALL $84B9: wait for a key */
  do read_keys(c, 0x8442); /* $8441: wait for it to be let go */
  while (!c->zf);
  c->a = 7;
  c->out(c, BORDER(c->a), c->a);
  cpu_tail(c, R_CONTINUE);
}

/* LD A,$FF / SCF (load) or AND A (verify) / LD IX,start / LD DE,len. */
static void tape_block(Cpu *c, uint16_t start, uint16_t len, bool load) {
  c->a = 0xFF;
  if (load)
    op_scf(c);
  else
    c->a = op_and(c, c->a, c->a);
  c->ix = start;
  set_de(c, len);
}

/* $8451 LOAD: the four blocks of a saved game (see the top of the file);
 * a tape error restarts the game ($8498). */
static void load_game(Cpu *c) {
  push16(c, c->ix);
  push16(c, get_de(c));
  tape_block(c, BLOCK_VARS, LEN_VARS, true);
  cpu_call_at(c, 0x845E); /* CALL $8498 */
  tape_block(c, BLOCK_OBJECTS, LEN_OBJECTS, true);
  cpu_call_at(c, 0x846B);
  tape_block(c, BLOCK_EVENTS, LEN_EVENTS, true);
  cpu_call_at(c, 0x8478);
  tape_block(c, BLOCK_LOCATIONS, LEN_LOCATIONS, true);
  cpu_call_at(c, 0x8485);
  /* $8488 DI */
  set_hl(c, BLOCK_VARS);
  set_de(c, SAVED_B6EB);
  cpu_call_at(c, 0x848F); /* CALL $84B3 */
  set_de(c, pop16(c));
  c->ix = pop16(c);
  cpu_tail(c, R_CONTINUE);
}

/* $8498 LoadBlock: LD-BYTES; on an error, say so, wait for a key and
 * restart the game. */
static void load_block(Cpu *c) {
  cpu_call_at(c, 0x8498); /* CALL $0556 LD-BYTES */
  if (c->cf) return;
  c->a = 1;
  c->mem[VAR_B701] = c->a;
  set_hl(c, MSG_TAPE_RESTART);
  cpu_call_at(c, 0x84A4); /* CALL $72DD */
  do read_keys(c, 0x84A8);
  while (c->zf);
  cpu_tail(c, R_RESTART);
}

/* $84B3 ThreeByteCopy: LDIR 3 bytes from HL to DE. */
static void three_byte_copy(Cpu *c) {
  set_bc(c, 3);
  ldir(c);
}

/* $84B9 DebounceAnyKey: wait until no key is down, then for a key. */
static void debounce_any_key(Cpu *c) {
  do read_keys(c, 0x84BA);
  while (!c->zf);
  do read_keys(c, 0x84C3); /* $84C2 NormalAnyKey */
  while (c->zf);
}

/* $8553: the end of SAVE (also reached from a verify error, $8573). */
static void save_done(Cpu *c) {
  /* DI */
  set_de(c, pop16(c));
  c->ix = pop16(c);
  cpu_tail(c, R_CONTINUE);
}

/* $84CC SAVE: the four blocks, then verify them ($855A; an error there
 * says so and ends the SAVE). */
static void save_game(Cpu *c) {
  push16(c, c->ix);
  push16(c, get_de(c));
  set_de(c, BLOCK_VARS);
  set_hl(c, SAVED_B6EB);
  cpu_call_at(c, 0x84D5); /* CALL $84B3 */
  c->a = 1;
  c->mem[VAR_B701] = c->a;
  set_hl(c, MSG_START_TAPE);
  cpu_call_at(c, 0x84E0); /* CALL $72DD */
  cpu_call_at(c, 0x84E3); /* CALL $84B9 */
  c->a = 0xFF, c->ix = BLOCK_VARS, set_de(c, LEN_VARS);
  cpu_call_at(c, 0x84EF); /* CALL $04C2 SA-BYTES */
  c->a = 0xFF, c->ix = BLOCK_OBJECTS, set_de(c, LEN_OBJECTS);
  cpu_call_at(c, 0x84FB);
  c->a = 0xFF, c->ix = BLOCK_EVENTS, set_de(c, LEN_EVENTS);
  cpu_call_at(c, 0x8507);
  c->a = 0xFF, c->ix = BLOCK_LOCATIONS, set_de(c, LEN_LOCATIONS);
  cpu_call_at(c, 0x8513);
  set_hl(c, MSG_REWIND);
  cpu_call_at(c, 0x8519); /* CALL $72DD */
  cpu_call_at(c, 0x851C); /* CALL $84B9 */
  tape_block(c, BLOCK_VARS, LEN_VARS, false);
  cpu_call_at(c, 0x8529); /* CALL $855A */
  tape_block(c, BLOCK_OBJECTS, LEN_OBJECTS, false);
  cpu_call_at(c, 0x8536);
  tape_block(c, BLOCK_EVENTS, LEN_EVENTS, false);
  cpu_call_at(c, 0x8543);
  tape_block(c, BLOCK_LOCATIONS, LEN_LOCATIONS, false);
  cpu_call_at(c, 0x8550);
  save_done(c);
}

/* $855A VerifyBlock: LD-BYTES verifying; on an error, say so, wait for a
 * key, drop the return address and finish the SAVE ($8553). */
static void verify_block(Cpu *c) {
  cpu_call_at(c, 0x855A); /* CALL $0556 LD-BYTES */
  if (c->cf) return;
  c->a = 1;
  c->mem[VAR_B701] = c->a;
  set_hl(c, MSG_TAPE_CONTINUE);
  cpu_call_at(c, 0x8566); /* CALL $72DD */
  do read_keys(c, 0x856A); /* $8569 */
  while (c->zf);
  set_de(c, pop16(c)); /* POP DE: the return address into SAVE */
  save_done(c);
}

#define DEFAULT (OUT_REGS | OUT_ZF | OUT_CF)

const PortRoutine meta_routines[] = {
    {0x8391, "Quit", quit, DEFAULT},
    {0x83A0, "DisplayLocHlpMsg", help, DEFAULT},
    {0x83EF, "GameOver", score_cmd, DEFAULT},
    {0x83F5, "GameOver_Start", show_score, DEFAULT},
    {0x842E, "PercentageAscii", digit, DEFAULT},
    {0x843A, "Pause", pause_cmd, DEFAULT},
    {0x8451, "LoadGame", load_game, DEFAULT},
    {0x8498, "LoadBlock", load_block, DEFAULT},
    {0x84B3, "ThreeByteCopy", three_byte_copy, DEFAULT},
    {0x84B9, "DebounceAnyKey", debounce_any_key, DEFAULT},
    {0x84CC, "SaveGame", save_game, DEFAULT},
    {0x855A, "VerifyBlock", verify_block, DEFAULT},
    {0, NULL, NULL, 0},
};
