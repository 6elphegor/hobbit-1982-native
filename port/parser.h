#ifndef HOBBIT_PARSER_H
#define HOBBIT_PARSER_H

#include "cpu.h"

void p_read_line(Cpu *c);  /* $6DD6 */
void p_clear_line(Cpu *c); /* $6E8B */
void p_get_word(Cpu *c);   /* $6E97 */

#endif
