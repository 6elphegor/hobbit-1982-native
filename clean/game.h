/* The game (clean edition): the start and restart, the main loop, and the
 * commands of their own (QUIT, HELP, SCORE, PAUSE, LOAD, SAVE). */
#ifndef HOBBIT_CLEAN_GAME_H
#define HOBBIT_CLEAN_GAME_H

#include "clean.h"

/* ---------- the tape ----------
 *
 * Blocks of data with the flag byte $FF and no headers, one after
 * another, on the host's tape (platform.h). Each returns false on a tape
 * error (for verify: also when the data differs from memory). */
bool tape_save_block(uint16_t start, uint16_t len);
bool tape_load_block(uint16_t start, uint16_t len, bool verify);

/* ---------- the game ---------- */

/* $6C00: keep a clean copy of the game's tables and play, game after
 * game. Never returns. */
void game_start(void);
/* $6C27: a new game, and play. Never returns. Every death, QUIT and LOAD
 * error comes back here, through the host (device_restart, platform.h). */
void game_restart(void);

/* $6FD3: clear the screen: white border, black ink on white paper. */
void clear_screen(void);

/* ---------- the commands of their own ---------- */

/* What the parser does after one of these commands. */
typedef enum {
  CMD_CONTINUE, /* carry on with the sentence ($82B3) */
  CMD_SKIP,     /* the word is ignored: carry on with the next ($75B4) */
  CMD_RESTART,  /* start a new game ($6C27) */
} CommandResult;

CommandResult cmd_quit(void);  /* $8391: the score, a key, a new game */
CommandResult cmd_help(void);  /* $83A0: a hint for some places */
CommandResult cmd_score(void); /* $83EF */
CommandResult cmd_pause(void); /* $843A: green border until a key */
CommandResult cmd_load(void);  /* $8451: a saved game from tape */
CommandResult cmd_save(void);  /* $84CC: save to tape and verify */

/* $83F5: "you have mastered NN.N% of this adventure". */
void show_score(void);
/* $84B9: wait until no key is down, then for a key. */
void wait_for_new_key(void);

#endif
