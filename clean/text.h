/* Text (clean edition): messages and their tokens, the names of things,
 * the sentence that describes an action, and the keyboard read with the
 * idle timer.
 *
 * Messages are strings of bytes in the game's data:
 *   $80-$FF  with the next byte, a dictionary token (see print_token);
 *            if bits 4-6 of the first byte are 2, 3 or 6 the message
 *            ends after it (with nothing, '.' and a new line, a new line);
 *   $60-$7F  one of 32 common words (the token table at $AD3D);
 *   $20-$5F  a character;
 *   $00-$1F  a control code: print the actor, an object, "is"/"are",
 *            "his"/"your", an argument, a sub-message, jump, end...
 * Four codes print an argument given by the caller (the original took
 * them from the stack): $00 the name at an address, $01 a token, $04 a
 * noun token with its article, $13 "X is"/"you are" for character X
 * (the argument's high byte). They take the arguments in order. */
#ifndef HOBBIT_CLEAN_TEXT_H
#define HOBBIT_CLEAN_TEXT_H

#include "clean.h"

/* Tokens: a 12-bit offset from $6000 into the dictionary, and in the
 * high nibble how it is printed (see print_token). */
enum {
  TOKEN_AS_IS = 0x5000,     /* no inflection */
  TOKEN_INFLECTED = 0x4000, /* with the entry's inflection */
  TOKEN_AGREES_OBJECT = 0x1000, /* inflected if there is a first object */
  TOKEN_SENTENCE_END = 0x7000,  /* the next word starts a sentence */
};

/* Messages with a fixed address. */
#define MSG_CANNOT_DO_THAT 0xAFBF /* "i cannot do that" */

/* $70E2: zero n bytes from addr (n = 0: 256). */
void zero_bytes(uint16_t addr, unsigned n);

/* $70E8: the address of action `action`'s record (8 bytes at $AB4B +
 * 8 * action: four words that make its sentence; flags in the high
 * nibbles of bytes 1, 3, 5 and 7). */
uint16_t action_record(uint8_t action);

/* $70F3: set the action flags ($B71D, $B71E) from the record; returns
 * the first ($B71D). */
uint8_t load_action_flags(uint16_t record);

/* $71D5 / $71E2: the address of the name (three dictionary words) of
 * location `loc` / object `obj`. */
uint16_t location_name(uint8_t loc);
uint16_t object_name(uint8_t obj);

/* $71F3: do the four words at `a` match the four at `b` (an empty word
 * of a matches anything; otherwise the 12-bit offsets must be equal)?
 * The first word must, and then the 2nd and 3rd in order or swapped
 * (*swapped then set). Sets $B6DF when the first word matches. */
bool phrase_matches(uint16_t a, uint16_t b, bool *swapped);

/* $7249: wait for a key and return it. If the player is idle for the
 * timer's count of scans, the line typed so far (ending at *end, with
 * *room left) is cleared, "WAIT" typed instead, and ENTER returned. */
uint8_t input_key(uint16_t *end, uint8_t *room);

/* $74C1: print dictionary token `token` (0 prints nothing). */
void print_token(uint16_t token);

/* $7478: print a noun token, after its article if $B703 is set. */
void print_noun_token(uint16_t token);

/* $743F: the article (or the capital of a proper noun) before noun
 * token `token`. */
void print_article(uint16_t token);

/* $7488: print character `who` ($FF: "someone"). */
void print_character(uint8_t who);

/* $72DD: print message `msg`, its argument codes taking `args` in order
 * (NULL if it has none). Returns how many it took. */
int print_message(uint16_t msg, const uint16_t *args);

/* $72D3: the same, inside quotes as if the action were only tried. */
int print_message_quoted(uint16_t msg, const uint16_t *args);

/* $72CE: "i cannot do that". */
void print_cannot_do_that(void);

/* $712B: describe the action being done (the actor, the verb, "cannot"
 * if it could not be done, the objects) as a sentence. */
void describe_action(void);

/* $711A: describe the action as one that could not be done, unless
 * inside quotes. */
void describe_failed_action(void);

#endif
