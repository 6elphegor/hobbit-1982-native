/* Addresses of the game's routines and variables used by ported code.
 * Names follow pobtastic/hobbit where it has them; "unknown" marks those
 * whose meaning is not yet worked out. */
#ifndef HOBBIT_ADDRS_H
#define HOBBIT_ADDRS_H

/* Routines (original code, reached with cpu_call until ported). */
#define R_PRINT_CHAR 0x858B /* print A: text window, or input line if PRINT_TO_INPUT */
#define R_GET_INPUT_KEY 0x7249 /* GetKey with the idle timer: types WAIT on timeout */

/* Input line and tokenizer. */
#define INPUT_BUFFER 0x6FF9  /* 128 characters, then $0D at $7079 */
#define WORD_CODES 0x707A    /* the word being looked up, as 5-bit letter codes */
#define WORD_LEN 0x708A
#define DICT_CODES 0x708B    /* the dictionary entry being compared, decoded */
#define DICT_LEN 0x709B
#define WORD_START 0xB6DA    /* where the last word read starts, for error messages */
#define DICT_ENTRY 0xB717    /* the dictionary entry being compared */
#define DICT_BASE 0x6000     /* dictionary: index by first letter, then entries */

#define IDLE_TIMER 0xB714    /* GetKey scans left before WAIT is typed */
#define PRINT_TO_INPUT 0xB701 /* output goes to the input line */
#define LAST_KEY 0xB704      /* last character accepted into the input line */
#define VAR_B6FA 0xB6FA      /* unknown: output enable, with $B702 */
#define VAR_B71A 0xB71A      /* unknown: nonzero disables '@' (repeat last command) */

#endif
