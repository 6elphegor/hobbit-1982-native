/* Scripted play, for the test machines: a script of commands typed into
 * the game as it asks for them, and a transcript of what it prints (see
 * script.c for the script's form). */
#ifndef HOBBIT_SCRIPT_H
#define HOBBIT_SCRIPT_H

#include <stdbool.h>
#include <stdio.h>

#include "spectrum.h"

/* Read a script; name is for messages. 0, or -1 (after saying why). */
int script_read(FILE *in, const char *name);
/* How many commands and directives it has. */
int script_length(void);
/* Play it into the machine's keyboard, and print the transcript on stdout:
 * the machine's input provider and output. pictures: mark the picture
 * pauses ([picture $NN]); text: hold N at the title (no pictures). */
void script_attach(Spectrum *s, bool pictures, bool text);
/* A line of our own in the transcript ([crash: ...], say). */
void script_annotate(Spectrum *s, const char *text);
/* The end of the run: finish the transcript's last line. */
void script_end(void);

#endif
