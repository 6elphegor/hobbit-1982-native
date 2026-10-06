/* The screen and the keyboard (clean edition).
 *
 * The display is the Spectrum's screen memory ($4000-$5AFF, its own
 * layout). The text window is character rows 0-17: text is printed on row
 * 17 in the game's proportional font (6 pixels a character, 42 to a line)
 * and a new line scrolls the window up. The input line is rows 19-23,
 * printed in the ROM font, with the cursor on row 23. */
#ifndef HOBBIT_CLEAN_SCREEN_H
#define HOBBIT_CLEAN_SCREEN_H

#include "clean.h"
#include "platform.h"

/* The devices: platform.h. */

/* ---------- printing ---------- */

/* Is printing on? (Off while a command is only being tried, or when
 * output is turned off.) */
bool can_print(void);

/* $858B: print a character, if printing is on: on the input line or in
 * the text window (CR: a new line; 8: delete). */
void print_char(uint8_t ch);

/* $8583: print a new line. */
void print_newline(void);

/* $85B8: print a character on the input line (CR: a new row; 8: delete
 * back; letters in capitals); the cursor follows. */
void input_line_put(uint8_t ch);

/* $86A1: print a character in the text window (CR or a full line: copy
 * the line to the printer, maybe wait, scroll; 8: delete). */
void text_window_put(uint8_t ch);

/* $876B: scroll the text window up one character row and blank row 17. */
void scroll_text_window(void);

/* $8B22: copy the line just finished to the ZX Printer, if PRINT is on. */
void printer_copy_line(void);

/* ---------- the keyboard ---------- */

/* $8B93: scan the keyboard; the character of a key that has gone down
 * since the last scan, or 0. */
uint8_t get_key(void);

/* $969A: wait for a key (after a picture), then set the border white. */
void wait_key_after_picture(void);

/* $90DF: wait for a key after "You are dead" (the game then restarts). */
void wait_key_after_death(void);

#endif
