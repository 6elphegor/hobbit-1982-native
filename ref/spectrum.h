/* Reference machine: a minimal 48K ZX Spectrum that runs the original
 * Hobbit code, with hooks for scripted input and text capture.
 *
 * It is not a general emulator: there is no ULA contention and no
 * interrupts (the game runs with them disabled). The game calls the ROM
 * only for tape loading and saving, which are trapped, but it reads ROM
 * bytes as data: the CHARSET for the input line, and the whole address
 * space, ROM included, in its random number generator (CalcRandom, $9CA8). */
#ifndef HOBBIT_SPECTRUM_H
#define HOBBIT_SPECTRUM_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../third_party/z80/z80.h"

#define SPEC_FRAME_T 69888 /* T-states per 50Hz frame */

/* Addresses in the game that the machine hooks (see pobtastic/hobbit). */
#define HOOK_PRINT_CHAR 0x86A1 /* draws character A in the text window */
#define HOOK_PRINT_INPUT 0x85B8 /* draws character A on the input line */
#define HOOK_GETKEY 0x8B93     /* GetKey: one keyboard scan of the input line */
#define HOOK_SEED_SET 0x6CAC   /* just after LD ($B70E),A stores the RNG seed */
#define ADDR_RNG_SEED 0xB70E
#define ADDR_PICTURE_WAIT 0x969B     /* IN in WaitForKey2, after a picture */
#define ADDR_LOCATION_PICTURE 0x7F77 /* LocationID: picture drawn, $FF if none */
#define ROM_SA_BYTES 0x04C2
#define ROM_LD_BYTES 0x0556

/* Keyboard matrix half-rows, in the order of port bits A8..A15. */
enum { ROW_CAPS_V, ROW_A_G, ROW_Q_T, ROW_1_5, ROW_0_6, ROW_P_Y, ROW_ENTER_H, ROW_SPACE_B };

/* Special characters understood by the typist, besides letters, digits
 * (pressed as raw keys), space and , . " @ */
#define KEY_ENTER '\n'
#define KEY_DELETE '\b' /* the 0 key */
#define KEY_CLEAR 0x18  /* CAPS SHIFT+0: clear the input line */

typedef struct Spectrum Spectrum;

struct Spectrum {
  z80 cpu;
  uint8_t mem[0x10000];
  uint8_t border;
  uint16_t ipc; /* address of the instruction being executed */

  /* Keyboard. Bits 0-4 of each row are set while the key is down.
   * live: from the host (SDL). typed: from the typist. */
  uint8_t live[8];
  uint8_t typed[8];

  /* Typist: plays a queue of characters through the keyboard. A key goes
   * down only when the game is waiting for input (see typist_tick). */
  char queue[8192];
  size_t qhead, qtail;
  int tstate; /* 0 idle, 1 holding, 2 gap */
  bool press_in_getkey;
  unsigned long mark_scans, mark_reads;
  unsigned long reads; /* keyboard port reads */

  /* Input provider: called when the game is waiting for a key and the
   * typist has nothing queued; it may queue more with spec_type. Its
   * position (in_pos, in_wait) is part of the machine state, so that a
   * replay from a snapshot sees the same input. */
  void (*need_input)(Spectrum *s);
  int in_pos;
  unsigned long in_wait;
  /* spec_stop jumps here. */
  jmp_buf *stop_jmp;
  /* If idle_limit is set, a run of more than that many instructions
   * without the game asking for input (need_input) is a hang: hung is set
   * and spec_stop called, wherever the machine is being stepped from. */
  unsigned long idle_steps, idle_limit;
  bool hung;
  bool waiting_for_input; /* the last keyboard read was a "listening" one */
  unsigned long ready_at;  /* GetKey scan from which a key may go down */

  unsigned long getkey_calls;

  int seed;         /* -1: keep the game's own seed from R */
  int initial_seed; /* the seed the game started with, -1 before it is set */

  /* Output: called for every character the game prints, in the text
   * window (stream 0) or on the input line (stream 1: the prompt, the echo
   * of what is typed, and parser errors). Stream 2 is for annotations from
   * the input provider. */
  void (*on_char)(void *ud, int stream, uint8_t ch);
  void *ud;

  /* Tape traps write and read headerless blocks in this .tap file. */
  char tape_path[1024];
  int tape_last_op; /* 0 none, 1 save, 2 load */
  long tape_pos;
  unsigned long tape_last_reads; /* keyboard reads at the last tape operation */

  bool trace_rom; /* report reads of ROM outside the CHARSET */
  bool trace_keys; /* report typist key presses */

  /* Called before each instruction; returns true if it ran a replacement
   * for the code at PC (see hybrid.c). */
  bool (*hook)(Spectrum *s);
  /* No Z80: everything must be ported. Reaching code with no C version
   * calls on_missing (which must not return: it ends the run). */
  bool no_cpu;
  void (*on_missing)(Spectrum *s, uint16_t pc);
  /* Called on every keyboard read, before it is answered (a front end
   * paces the game, draws the screen and takes input here). */
  void (*on_read)(Spectrum *s);
};

/* Set up an empty machine (ROM area $FF, PC at the game's entry point). */
void spec_setup(Spectrum *s);

/* Set up the machine and load the game from the user's tape. rom_path may
 * be NULL, in which case the game's own font stands in for the ROM CHARSET.
 * Returns 0, or -1 with a message in err. */
int spec_init(Spectrum *s, const char *tzx_path, const char *rom_path, char *err, size_t errlen);

/* Execute one instruction (with traps and hooks). */
void spec_step(Spectrum *s);

/* Read or write an I/O port, as an IN or OUT instruction would. */
uint8_t spec_port_in(Spectrum *s, uint16_t port);
void spec_port_out(Spectrum *s, uint16_t port, uint8_t v);

/* What the machine does when execution reaches addr, besides running the
 * instruction there: capturing printed characters ($86A1, $85B8),
 * counting GetKey scans ($8B93), the seed override ($6CAC). Called for
 * original code and for ported routines entered at addr. */
void spec_at(Spectrum *s, uint16_t addr);

/* Run until at least t more T-states have elapsed. */
void spec_run(Spectrum *s, unsigned long t);

/* The tape (trapped SA-BYTES and LD-BYTES): a block with this flag byte,
 * len bytes at start, saved to the .tap file (a group of saves starts it
 * afresh), or read from it (a group of reads starts at its beginning):
 * loaded, or with load false compared with memory. True if all went well. */
bool spec_tape_save(Spectrum *s, uint8_t flag, uint16_t start, uint16_t len);
bool spec_tape_load(Spectrum *s, uint8_t flag, uint16_t start, uint16_t len, bool load);
/* Queue text for the typist. Returns false if the queue is full. */
bool spec_type(Spectrum *s, const char *text);
bool spec_typist_idle(const Spectrum *s);

/* True if the keyboard read being executed is in GetKey (the input line). */
bool spec_in_getkey(const Spectrum *s);

/* End the run: longjmp to *s->stop_jmp. */
void spec_stop(Spectrum *s);

/* Map a host key press to the matrix (for live play). */
void spec_key(Spectrum *s, int row, int bit, bool down);

/* Matrix keys (one or two) needed to type ch. Returns the count, 0 if the
 * character cannot be typed. */
int spec_char_keys(char ch, int rows[2], int bits[2]);

/* Render the display (with border) as 0xRRGGBB pixels. */
#define SPEC_BORDER_X 32
#define SPEC_BORDER_Y 24
#define SPEC_SCREEN_W (256 + 2 * SPEC_BORDER_X)
#define SPEC_SCREEN_H (192 + 2 * SPEC_BORDER_Y)
void spec_render(const Spectrum *s, uint32_t *px, bool flash_phase);

#endif
