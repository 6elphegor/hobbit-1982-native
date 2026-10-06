/* The clean edition: the game. See docs/CLEAN.md.
 *
 * The start and the restart ($6C00, $6C27), the main loop (read a line,
 * split it into tokens, parse and carry out each sentence), the screen
 * clearing ($6FD3), and the commands of their own: QUIT, HELP, SCORE,
 * PAUSE, LOAD and SAVE ($8391-$8575).
 *
 * The random seed: the original took it from the refresh register R; here
 * it is any byte the machine gives (hardware_random_byte), and the host may
 * replace it (seed_chosen: the test machine's --seed).
 *
 * The saved game is four data blocks on tape (flag $FF, no headers):
 * the variables, the objects, the timed events and the locations. A save
 * first copies the 3 bytes at $C9E2 over the start of the variables, so
 * they are what is saved there; a load copies them back to $C9E2. */
#include "game.h"

#include "characters.h"
#include "executor.h"
#include "objects.h"
#include "parser.h"
#include "screen.h"
#include "text.h"

#include <stddef.h>
#include <string.h>

/* ---------- the game's memory ---------- */

#define CLEAN_OBJECTS 0xF400   /* a clean copy of the objects, then of the locations */
#define CLEAN_LOCATIONS 0xFA15
#define CLEAN_VARS 0x5F00      /* a clean copy of the variables, then of the timed events */
#define CLEAN_EVENTS 0x5F1D
#define GAME_VARS 0xB6EB       /* the variables a game changes */
#define GAME_VARS_LEN 0x001D
#define GAME_OBJECTS 0xC11B
#define GAME_OBJECTS_LEN 0x0615
#define GAME_EVENTS TIMED_EVENTS /* and what follows them */
#define GAME_EVENTS_LEN 0x00BF
#define GAME_LOCATIONS 0xBA8A
#define GAME_LOCATIONS_LEN 0x05D9
#define SAVED_VARS_HEAD 0xC9E2 /* 3 bytes saved in place of the variables' first 3 */

#define LOCATION_GFX 0xCC00    /* [location][graphics data] triples */
#define FIRST_LOCATION 0x05    /* whose graphics' border and paper are reset */
#define SQUIGGLE_GFX 0x6DCC    /* the line between the picture and the text */
#define SQUIGGLE_SCREEN 0x5140
#define BORDCR 0x5C48          /* the ROM's border colour variable */

#define INPUT_LOOK 0x6FF2      /* "> LOOK" and CR, typed for a new game */
#define INPUT_BUFFER 0x6FF9
#define TOKENS 0x709C          /* the command as tokens: 2 bytes a word, $40 bytes */
#define TOKENS_LEN 0x40
#define TOKEN_PTR 0xB6DC       /* where the parser reads tokens */
#define WORD_START 0xB6DA      /* where the last word read starts */
#define HELP_TABLE 0x83CD      /* [location][message] triples */
#define YOUR_LOCATION_OBJ 0xC12B /* your location, in your object record */

/* Variables. */
#define V_PLAYER_STATE 0xB6F2
#define V_SCORE 0xB6F7         /* in tenths of a percent (word) */
#define V_B700 0xB700
#define V_DIGITS_DONE 0xB704   /* cleared after the score's digits */
#define V_MORE_SENTENCES 0xB705 /* the command has more sentences to carry out */
#define V_NEW_GAME 0xB706      /* $FF at the start: set up a new game (also the parser's) */
#define V_B716 0xB716          /* $11 when the screen is set up, $09 before each line */
#define V_869F 0x869F          /* text window state */
#define V_86A0 0x86A0
#define PERCENT_BUFFER 0x85B3  /* printing state, set up at the start */
#define TEXT_WINDOW 0x869B

/* Messages (PrintMsg). */
#define MSG_UNKNOWN_WORD 0xAD93 /* "i do not know the word "" */
#define MSG_MASTERED 0xB454     /* "you have mastered " */
#define MSG_OF_ADVENTURE 0xB462 /* "% of this adventure" */
#define MSG_DOING_FINE 0xB467   /* "you're doing fine" */
#define MSG_START_TAPE 0xB342   /* "start tape then press any key" */
#define MSG_TAPE_RESTART 0xB357 /* "tape error - hit any key to restart program" */
#define MSG_TAPE_CONTINUE 0xB381 /* "tape error - hit any key to continue" */
#define MSG_REWIND 0xB3A4       /* "rewind and prepare tape for verification ..." */

/* Word classes (GetWord). */
enum { CLASS_QUOTE = 0x90, CLASS_COMMA = 0xA0, CLASS_STOP = 0xB0, CLASS_END = 0xC0, CLASS_UNKNOWN = 0xD0 };

static const struct {
  uint16_t start, len;
} SAVE_BLOCKS[] = {
    {GAME_VARS, GAME_VARS_LEN},
    {GAME_OBJECTS, GAME_OBJECTS_LEN},
    {GAME_EVENTS, GAME_EVENTS_LEN},
    {GAME_LOCATIONS, GAME_LOCATIONS_LEN},
};
#define NBLOCKS (sizeof SAVE_BLOCKS / sizeof SAVE_BLOCKS[0])


/* ---------- the machine (implemented at the bottom, through the host) ---------- */

/* Where the game waits on the keyboard: the faithful IN instruction's
 * address, which tells the test typist what kind of wait it is. */
typedef enum {
  KEYS_START = 0x6C6E,        /* "press any key" before a game */
  KEYS_TEXT_ONLY = 0x6C78,    /* the N key, read at the start */
  KEYS_QUIT = 0x8395,         /* after QUIT's score */
  KEYS_PAUSE_UP = 0x8442,     /* PAUSE: waiting for the key to be let go */
  KEYS_TAPE_ERROR = 0x84A8,   /* a LOAD error */
  KEYS_ALL_UP = 0x84BA,       /* waiting for no key to be down */
  KEYS_ANY = 0x84C3,          /* waiting for a key (the tape prompts, PAUSE) */
  KEYS_VERIFY_ERROR = 0x856A, /* a verify error after SAVE */
} KeyWait;

/* The keyboard half-rows selected by row_select (the high byte of the
 * port: 0 for all of them), bits 0-4, 0 for a key down. */
static uint8_t keyboard_read(uint8_t row_select, KeyWait at);
/* Any key down, on any half-row. */
static bool any_key_down(KeyWait at);
/* Set the border colour (0-7). */
static void border_set(uint8_t colour);
/* A byte that differs from run to run (the Z80's refresh register). */
static uint8_t hardware_random_byte(void);
/* The seed (V_RANDOM) has been chosen: the host may replace it. */
static void seed_chosen(void);

/* ---------- other modules' routines ---------- */

/* Find id in a table of [id][word] triples: the entry's word, or false if
 * it is not there. */
static bool table_lookup(uint16_t table, uint8_t id, uint16_t *word) {
  uint16_t e = find_entry(table, id);
  if (entry_key(e) == 0xFF) return false;
  *word = entry_word(e);
  return true;
}

/* A message, in the text window (to_input 0) or on the input line (1). */
static void say(uint16_t msg, uint8_t to_input) {
  mem[V_PRINT_TO_INPUT] = to_input;
  print_message(msg, NULL);
}

/* The next word of the input line from *at (moved past it): its class,
 * and its token (class + dictionary offset). */
static uint8_t next_word(uint16_t *at, uint16_t *token) {
  Token t = get_word(at);
  *token = (uint16_t)((t.cls << 8) + t.offset);
  return t.cls;
}

/* The next sentence from TOKEN_PTR: false if there is none to carry out.
 * (The original's ParseSentence ends with A = $B71B - 1 when there is
 * nothing left: Z, so on to Execute, if a quote is open.) */
static bool next_sentence(void) {
  switch (parse_sentence()) {
  case PARSED: return true;
  case PARSE_NOTHING: return mem[V_IN_QUOTES] == 1;
  default: return false;
  }
}

/* ---------- the start and the restart ---------- */

/* Copy the tables a game changes (the objects, the locations, the
 * variables, the timed events) from the game to the clean copies
 * (to_clean) or back. */
static void copy_tables(bool to_clean) {
  static const struct {
    uint16_t game, clean, len;
  } TABLES[] = {
      {GAME_OBJECTS, CLEAN_OBJECTS, GAME_OBJECTS_LEN},
      {GAME_LOCATIONS, CLEAN_LOCATIONS, GAME_LOCATIONS_LEN},
      {GAME_VARS, CLEAN_VARS, GAME_VARS_LEN},
      {GAME_EVENTS, CLEAN_EVENTS, GAME_EVENTS_LEN},
  };
  for (size_t i = 0; i < sizeof TABLES / sizeof TABLES[0]; i++)
    if (to_clean)
      memcpy(&mem[TABLES[i].clean], &mem[TABLES[i].game], TABLES[i].len);
    else
      memcpy(&mem[TABLES[i].game], &mem[TABLES[i].clean], TABLES[i].len);
}

/* Set up a new game: the clean tables, a key to start (N held for text
 * only), the printing state, nobody told anything yet, the seed, the
 * score. */
static void new_session(void) {
  uint16_t gfx;
  /* The first location's border and paper go black (the original does
   * not check that it is found: it is). */
  table_lookup(LOCATION_GFX, FIRST_LOCATION, &gfx);
  mem[gfx] = 0x00;
  mem[(uint16_t)(gfx + 1)] = 0x00;
  copy_tables(false);
  border_set(0);
  mem[BORDCR] = 0x38;

  while (!any_key_down(KEYS_START)) continue;
  mem[V_GRAPHICS] = keyboard_read(0x7F, KEYS_TEXT_ONLY) & 0x08; /* the N key: 0 if held */

  set_word_at(PERCENT_BUFFER + 1, 0x50E0);
  mem[PERCENT_BUFFER + 3] = 0x2B;
  set_word_at(TEXT_WINDOW + 1, 0x5020);
  mem[TEXT_WINDOW + 3] = 0x01;
  mem[PERCENT_BUFFER] = 0x20;
  mem[TEXT_WINDOW] = 0x2A;
  memset(&mem[SPEECH_SLOTS], 0, 0xC8);

  mem[V_RANDOM] = hardware_random_byte();
  seed_chosen();

  mem[V_869F] = 0;
  mem[V_86A0] = 0;
  mem[V_QUESTION] = 0;
  mem[V_B700] = 0;
  mem[V_PLAYER_STATE] = 0;
  mem[V_OUTPUT_ON] = 1;
  mem[V_DOING] = 1;
  mem[V_DIGITS_DONE] = 1;
  set_word_at(V_SCORE, 0);
}

/* $6FD3 */
void clear_screen(void) {
  border_set(7);
  memset(&mem[0x4000], 0x00, 0x1800);
  /* The attributes, and (as the original's LDIR does) the byte after. */
  memset(&mem[0x5800], 0x38, 0x301);
}

/* Clear the screen and draw the line between the picture and the text:
 * 5 pixel lines, each a 2-byte pattern 16 times. */
static void draw_screen(void) {
  clear_screen();
  for (int line = 0; line < 5; line++) {
    uint16_t at = (uint16_t)(SQUIGGLE_SCREEN + line * 0x100);
    for (int i = 0; i < 16; i++) {
      mem[at++] = mem[SQUIGGLE_GFX + 2 * line];
      mem[at++] = mem[SQUIGGLE_GFX + 2 * line + 1];
    }
  }
}

/* ---------- the main loop ---------- */

/* "i do not know the word "..."": the word, up to a space (printed), a
 * quote or the end of the line. */
static void unknown_word(void) {
  say(MSG_UNKNOWN_WORD, 1);
  for (uint16_t at = word_at(WORD_START);; at++) {
    uint8_t ch = mem[at];
    if (ch == '\r' || ch == '"') break;
    print_char(ch);
    if (ch == ' ') break;
  }
  print_char('"');
  print_newline();
}

/* Split the input line into tokens at TOKENS (2 bytes a word, high byte
 * first: the class and dictionary offset), adding a full stop before a
 * closing quote unless one (or a comma) is there. False (after saying
 * why) for an unknown word or a quote left open. */
/* Add a token at *out. A line of more words than the tokens have room
 * for is refused (false, after "what ?"). (The original's bug writes on,
 * over PrintMsg's variables and its own code.) */
static bool put_token(uint16_t *out, uint16_t token) {
  if (*out + 2 > TOKENS + TOKENS_LEN && !original_bugs) {
    mem[V_IN_QUOTES] = 0;
    say_what();
    return false;
  }
  mem[*out] = token >> 8;
  mem[(uint16_t)(*out + 1)] = (uint8_t)token;
  *out += 2;
  return true;
}

static bool tokenize(void) {
  uint16_t at = INPUT_BUFFER, out = TOKENS;
  memset(&mem[TOKENS], 0, TOKENS_LEN);
  for (;;) {
    uint16_t token;
    uint8_t cls = next_word(&at, &token);
    if (cls == CLASS_UNKNOWN) {
      unknown_word();
      return false;
    }
    if (cls == CLASS_QUOTE && (token & 0x0FFF) == 0) { /* a quote mark */
      if (!mem[V_IN_QUOTES]) {
        mem[V_IN_QUOTES] = 1;
      } else {
        mem[V_IN_QUOTES] = 0;
        uint8_t before = mem[(uint16_t)(out - 2)] & 0xF0;
        if (before != CLASS_STOP && before != CLASS_COMMA && !put_token(&out, CLASS_STOP << 8)) return false;
      }
    }
    if (!put_token(&out, token)) return false;
    if (cls == CLASS_END) break;
  }
  if (mem[V_IN_QUOTES]) {
    mem[V_IN_QUOTES] = 0;
    say_what();
    return false;
  }
  return true;
}

/* Parse and carry out each sentence of the command in TOKENS. */
static void run_command(void) {
  set_word_at(TOKEN_PTR, TOKENS);
  do {
    if (!next_sentence()) return;
    execute_sentences();
  } while (mem[V_MORE_SENTENCES]);
}

/* Play the game set up by new_session: the screen, then (if $B706 is $FF,
 * as the clean variables have it) a new game, with "LOOK" as if typed,
 * and command after command. Never returns (a restart goes through the
 * host). */
static void play(void) {
  draw_screen();
  mem[V_B716] = 0x11;
  bool typed = false;
  if (mem[V_NEW_GAME] == 0xFF) {
    new_game_characters();
    for (uint16_t at = INPUT_LOOK;; at++) {
      input_line_put(mem[at]);
      if (mem[at] == '\r') break;
    }
    memcpy(&mem[INPUT_BUFFER], &mem[INPUT_LOOK + 2], 5); /* "LOOK" and CR */
    typed = true;
  }
  bool repeatable = true; /* the last line was taken (or there was none) */
  for (;;) {
    if (!typed) {
      mem[V_MORE_SENTENCES] = 1;
      mem[V_B716] = 0x09;
      typed = read_line();
      if (!typed) { /* nothing new: the same tokens again */
        /* A line that was refused is refused again. (The original's bug
         * runs its tokens without the checks: a quote left open crashes
         * the parser.) */
        if (!repeatable && !original_bugs) {
          say_what();
          continue;
        }
        run_command();
        continue;
      }
    }
    typed = false;
    repeatable = tokenize();
    if (repeatable) run_command();
  }
}

/* $6C27 */
void game_restart(void) {
  new_session();
  play();
}

/* $6C00 */
void game_start(void) {
  copy_tables(true);
  game_restart();
}


/* ---------- the commands of their own ---------- */

/* Wait for a key to go down. */
static void wait_for_key(KeyWait at) {
  while (!any_key_down(at)) continue;
}

/* $84B9 */
void wait_for_new_key(void) {
  while (any_key_down(KEYS_ALL_UP)) continue;
  wait_for_key(KEYS_ANY);
}

/* $842E: the next digit of the score: '0' + *value / unit, and *value
 * keeps the remainder. (Past '9', the original goes on to ':'...) */
static uint8_t score_digit(uint16_t *value, uint16_t unit) {
  uint8_t digit = (uint8_t)('0' + *value / unit);
  *value %= unit;
  return digit;
}

/* $83F5 */
void show_score(void) {
  uint16_t score = word_at(V_SCORE); /* tenths of a percent */
  say(MSG_MASTERED, 0);
  uint8_t tens = score_digit(&score, 100);
  if (tens != '0') print_char(tens);
  print_char(score_digit(&score, 10));
  print_char('.');
  print_char((uint8_t)('0' + score));
  mem[V_DIGITS_DONE] = 0;
  print_message(MSG_OF_ADVENTURE, NULL);
}

/* $8391 */
CommandResult cmd_quit(void) {
  show_score();
  wait_for_key(KEYS_QUIT);
  return CMD_RESTART;
}

/* $83A0: only for you; when a character is told HELP, the word is
 * skipped. */
CommandResult cmd_help(void) {
  if (mem[V_ACTOR] != YOU) return CMD_SKIP;
  uint16_t msg;
  if (!table_lookup(HELP_TABLE, mem[YOUR_LOCATION_OBJ], &msg)) msg = MSG_DOING_FINE;
  say(msg, 1);
  return CMD_CONTINUE;
}

/* $83EF */
CommandResult cmd_score(void) {
  show_score();
  return CMD_CONTINUE;
}

/* $843A */
CommandResult cmd_pause(void) {
  border_set(4);
  wait_for_new_key();
  while (any_key_down(KEYS_PAUSE_UP)) continue;
  border_set(7);
  return CMD_CONTINUE;
}

/* $84B3: the 3 bytes kept in place of the variables' first 3. */
static void copy3(uint16_t from, uint16_t to) { memcpy(&mem[to], &mem[from], 3); }

/* $8498: load a block; on a tape error, say so and wait for a key (the
 * game then restarts), and return false. */
static bool load_block(uint16_t start, uint16_t len) {
  if (tape_load_block(start, len, false)) return true;
  say(MSG_TAPE_RESTART, 1);
  wait_for_key(KEYS_TAPE_ERROR);
  return false;
}

/* $8451 */
CommandResult cmd_load(void) {
  for (size_t i = 0; i < NBLOCKS; i++)
    if (!load_block(SAVE_BLOCKS[i].start, SAVE_BLOCKS[i].len)) return CMD_RESTART;
  copy3(GAME_VARS, SAVED_VARS_HEAD);
  return CMD_CONTINUE;
}

/* $855A: verify a block; on an error, say so, wait for a key and return
 * false. */
static bool verify_block(uint16_t start, uint16_t len) {
  if (tape_load_block(start, len, true)) return true;
  say(MSG_TAPE_CONTINUE, 1);
  wait_for_key(KEYS_VERIFY_ERROR);
  return false;
}

/* $84CC: save, then verify (a verify error ends it). */
CommandResult cmd_save(void) {
  copy3(SAVED_VARS_HEAD, GAME_VARS);
  say(MSG_START_TAPE, 1);
  wait_for_new_key();
  for (size_t i = 0; i < NBLOCKS; i++) tape_save_block(SAVE_BLOCKS[i].start, SAVE_BLOCKS[i].len);
  print_message(MSG_REWIND, NULL);
  wait_for_new_key();
  for (size_t i = 0; i < NBLOCKS; i++)
    if (!verify_block(SAVE_BLOCKS[i].start, SAVE_BLOCKS[i].len)) break;
  return CMD_CONTINUE;
}

/* ---------- the machine (platform.h) ---------- */

static uint8_t keyboard_read(uint8_t row_select, KeyWait at) {
  return device_in((uint16_t)(row_select << 8 | 0xFE), (uint16_t)at) & 0x1F;
}

static bool any_key_down(KeyWait at) { return keyboard_read(0x00, at) != 0x1F; }

static void border_set(uint8_t colour) { device_out((uint16_t)(colour << 8 | 0xFE), colour); }

static uint8_t hardware_random_byte(void) { return device_random_byte(); }

/* The test machine watches $6CAC (A = the seed) and may replace it. */
static void seed_chosen(void) { device_at(0x6CAC, mem[V_RANDOM]); }

bool tape_save_block(uint16_t start, uint16_t len) { return device_tape(true, start, len, false); }

bool tape_load_block(uint16_t start, uint16_t len, bool verify) { return device_tape(false, start, len, verify); }

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ---------- adapters (for the faithful callers, while they remain) ---------- */

/* $6C00 and $6C27, top-level entries (PORT_TOP: the host runs them only
 * from the outermost loop; a jump to $6C27 from anywhere unwinds to there
 * first). The stack starts again at $5EFF, as in the original. */
static void a_start(Cpu *c) {
  c->sp = 0x5EFF;
  game_start();
}

static void a_restart(Cpu *c) {
  c->sp = 0x5EFF;
  game_restart();
}

/* $6FD3 ClearScreen: keeps HL DE BC and the flags it uses; A=7. */
static void a_clear_screen(Cpu *c) {
  clear_screen();
  c->a = 0x07;
}

/* After a command, where the original goes: on with the parser, entered
 * at $82B3 or $75B4 with its return address on top of the stack and its
 * registers, or the restart. */
static void finish_command(Cpu *c, CommandResult r) {
  static const uint16_t NEXT[] = {[CMD_CONTINUE] = 0x82B3, [CMD_SKIP] = 0x75B4, [CMD_RESTART] = 0x6C27};
  cpu_tail(c, NEXT[r]);
}

/* The registers a command keeps (it pushes them, or its callees keep
 * them), for the parser to carry on with. */
typedef struct {
  uint16_t hl, de, ix;
} Kept;
static Kept keep(const Cpu *c) { return (Kept){get_hl(c), get_de(c), c->ix}; }

static void a_quit(Cpu *c) {
  CommandResult r = cmd_quit();
  finish_command(c, r);
}

/* HELP pushes HL and IX. */
static void a_help(Cpu *c) {
  Kept k = keep(c);
  CommandResult r = cmd_help();
  set_hl(c, k.hl), c->ix = k.ix;
  finish_command(c, r);
}

/* SCORE: $83F5 pushes HL and DE. */
static void a_score(Cpu *c) {
  Kept k = keep(c);
  CommandResult r = cmd_score();
  set_hl(c, k.hl), set_de(c, k.de);
  finish_command(c, r);
}

static void a_pause(Cpu *c) {
  CommandResult r = cmd_pause();
  c->a = 0x07;
  finish_command(c, r);
}

/* LOAD and SAVE push IX and DE. */
static void a_load(Cpu *c) {
  Kept k = keep(c);
  CommandResult r = cmd_load();
  set_de(c, k.de), c->ix = k.ix;
  /* A tape error restarts from inside LoadBlock, with IX, DE and its
   * return address on the stack: the restart resets SP, but the check
   * compares it. */
  if (r == CMD_RESTART) c->sp -= 6;
  finish_command(c, r);
}

static void a_save(Cpu *c) {
  Kept k = keep(c);
  CommandResult r = cmd_save();
  set_de(c, k.de), c->ix = k.ix;
  finish_command(c, r);
}

/* $83F5 GameOver_Start: keeps HL and DE. Callers: QUIT and SCORE (clean)
 * and the death ($90DC), which waits for a key and restarts: none uses
 * anything else. */
static void a_show_score(Cpu *c) {
  Kept k = keep(c);
  show_score();
  set_hl(c, k.hl), set_de(c, k.de);
}

/* $842E PercentageAscii: HL / DE in A as a digit, HL the remainder; CP
 * '0' (the caller prints the tens with CALL NZ). */
static void a_score_digit(Cpu *c) {
  uint16_t v = get_hl(c);
  c->a = score_digit(&v, get_de(c));
  set_hl(c, v);
  op_cp(c, '0');
}

/* $84B3 ThreeByteCopy: LDIR 3 bytes from HL to DE. */
static void a_copy3(Cpu *c) {
  copy3(get_hl(c), get_de(c));
  set_hl(c, get_hl(c) + 3), set_de(c, get_de(c) + 3), set_bc(c, 0);
  c->hf = 0, c->nf = 0, c->pf = 0;
}

/* $84B9 DebounceAnyKey: its callers (PAUSE, SAVE) use nothing it leaves. */
static void a_wait_for_new_key(Cpu *c) {
  (void)c;
  wait_for_new_key();
}

/* $8498 LoadBlock (IX start, DE length): returns (carry set) only if the
 * block loaded; otherwise the game restarts. */
static void a_load_block(Cpu *c) {
  bool ok = load_block(c->ix, get_de(c));
  if (!ok) cpu_tail(c, 0x6C27);
}

/* $855A VerifyBlock (IX start, DE length): returns (carry set) only if
 * the block verified; otherwise it leaves SAVE's frame as the original
 * does ($8553): drop the return address, POP DE, POP IX, on to $82B3. */
static void a_verify_block(Cpu *c) {
  bool ok = verify_block(c->ix, get_de(c));
  if (ok) return;
  pop16(c);
  set_de(c, pop16(c));
  c->ix = pop16(c);
  cpu_tail(c, 0x82B3);
}

/* The commands' outputs are the parser's (they return through it, after
 * $82B3): its callers use Z. Their inputs in BC, DE and IY are the
 * parser's state, which they hand back to it as they had it (the check
 * does not mutate them: apart, they are not a state the parser has). */
#define COMMAND OUT_ZF
#define PARSER_STATE (OUT_BC | OUT_DE | OUT_IY)

const CleanRoutine game_clean[] = {
    {0x6C00, "game_start", a_start, 0},
    {0x6C27, "game_restart", a_restart, 0},
    {0x6FD3, "clear_screen", a_clear_screen, OUT_REGS | OUT_ZF | OUT_CF},
    {0x8391, "cmd_quit", a_quit, COMMAND, PARSER_STATE},
    {0x83A0, "cmd_help", a_help, COMMAND, PARSER_STATE},
    {0x83EF, "cmd_score", a_score, COMMAND, PARSER_STATE},
    {0x83F5, "show_score", a_show_score, OUT_HL | OUT_DE},
    {0x842E, "score_digit", a_score_digit, OUT_A | OUT_HL | OUT_ZF},
    {0x843A, "cmd_pause", a_pause, COMMAND, PARSER_STATE},
    {0x8451, "cmd_load", a_load, COMMAND, PARSER_STATE},
    {0x8498, "load_block", a_load_block, OUT_CF},
    {0x84B3, "copy3", a_copy3, OUT_HL | OUT_DE | OUT_BC},
    {0x84B9, "wait_for_new_key", a_wait_for_new_key, 0},
    {0x84CC, "cmd_save", a_save, COMMAND, PARSER_STATE},
    {0x855A, "verify_block", a_verify_block, OUT_CF},
    {0, NULL, NULL, 0},
};

const CleanScratch game_scratch[] = {
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
