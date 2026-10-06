/* The parser (clean edition): reading the input line, splitting it into
 * dictionary words, and parsing the words into noun-phrase records. */
#ifndef HOBBIT_CLEAN_PARSER_H
#define HOBBIT_CLEAN_PARSER_H

#include "clean.h"

#define INPUT_BUFFER 0x6FF9 /* the line typed: 128 characters, then a $0D at $7079 */
#define TOKENS 0x709C       /* the line as words: [class | offset high][offset low] pairs */
#define TOKEN_PTR 0xB6DC    /* the next token for the sentence parser */
#define WORD_START 0xB6DA   /* where the last word read starts (also the last token read) */
#define IN_QUOTES V_IN_QUOTES
#define PHRASE_RECORDS 0xB9C8 /* the first noun-phrase record; the rest go down by $18 */
#define PHRASE_COUNT 0xB706   /* how many records the sentence has */
#define MORE_ON_LINE 0xB705   /* cleared when the parser reaches the end of the line */

/* Word classes (the high nibble of a token's first byte). */
enum {
  WC_ADVERB = 0x00, WC_IN = 0x10, WC_DIRECTION = 0x20, WC_VERB = 0x30, WC_GO = 0x40,
  WC_NOUN = 0x50, WC_ADJECTIVE = 0x60, WC_PREPOSITION = 0x70, WC_ARTICLE = 0x80,
  WC_SPECIAL = 0x90, /* ALL, EXCEPT, IT, ONE, PRINT, NOPRINT, SAVE, ...; and '"' (offset 0) */
  WC_AND = 0xA0,     /* AND and ',' (offset 0) */
  WC_THEN = 0xB0,    /* THEN and '.' (offset 0) */
  WC_END = 0xC0,     /* the end of the line */
  WC_UNKNOWN = 0xD0, /* not in the dictionary */
};

/* A word read from the line: its class, and its dictionary offset (its
 * entry's address - $6000), 0 for punctuation and the end of the line. */
typedef struct {
  uint8_t cls;
  uint16_t offset;
} Token;

/* The line being typed: where the next character goes, and the room
 * left ($80 when empty, counting down). */
typedef struct {
  uint16_t end;
  uint8_t room;
} InputLine;

/* $6DD6: print the prompt and read a line into INPUT_BUFFER. True when
 * ENTER ends it; false when '@' on an empty line asks for the last
 * command again. */
bool read_line(void);

/* $6E8B: rub out everything typed on the line. */
void clear_line(InputLine *line);

/* $6E97: read the next word of the line at *pos, leaving *pos after it. */
Token get_word(uint16_t *pos);

/* What parse_sentence found. */
typedef enum {
  PARSED,        /* a sentence is ready in the records (Z) */
  PARSE_FAILED,  /* "what ?" was printed (NZ) */
  PARSE_NOTHING, /* nothing left to parse (NZ, unless a quote is open: then Z) */
} ParseResult;

/* $7585: parse the next sentence from the tokens at TOKEN_PTR into the
 * records at PHRASE_RECORDS. */
ParseResult parse_sentence(void);

/* $792F: print "what ?" on the input line. */
void say_what(void);

#endif
