/* The clean edition: the parser. See docs/CLEAN.md.
 *
 * Three parts: reading the input line (ReadLine $6DD6), splitting it into
 * dictionary words (GetWord $6E97), and the sentence parser
 * (ParseSentence $7585), which turns the token list at TOKENS into
 * noun-phrase records at PHRASE_RECORDS. All of it works on the game's
 * own buffers in the memory image, and writes them as the original does:
 * other code reads them (and a very long word overflows its buffer into
 * the token list, as in the original). */
#include "parser.h"

#include "game.h"
#include "screen.h"
#include "text.h"
#include "platform.h"

#include <stddef.h>
#include <string.h>

/* ---------- other modules' routines ---------- */

/* Wait for a key. If the player waits too long, it rubs out the line
 * (clear_line), types WAIT into it itself and returns ENTER: so the
 * line's end and room come back changed. */
static uint8_t get_input_key(InputLine *line) { return input_key(&line->end, &line->room); }

/* ---------- reading the input line ---------- */

#define IDLE_TIMER 0xB714  /* key scans left before WAIT is typed for you */
#define LAST_KEY 0xB704    /* the last character accepted into the line */
#define MSG_WHAT 0xAD9F    /* "what ?" */

enum { KEY_ENTER = 0x0D, KEY_DELETE = 0x08, KEY_CLEAR = 0x18 /* CAPS SHIFT+0 */, KEY_REPEAT = '@' };

static bool line_empty(const InputLine *line) { return line->room & 0x80; }

void clear_line(InputLine *line) {
  while (!line_empty(line)) {
    print_char(KEY_DELETE);
    line->room++;
    line->end--;
  }
}

/* As the first key of a line, a cursor key types a direction and ENTER:
 * right E, left W, down S, up N. Returns ENTER if so, else the key. */
static uint8_t direction_key(uint8_t key, InputLine *line) {
  uint8_t letter;
  switch (key) {
  case 0x09: letter = 'E'; break;
  case 0x08: letter = 'W'; break;
  case 0x0A: letter = 'S'; break;
  case 0x5B: letter = 'N'; break;
  default: return key;
  }
  mem[line->end++] = letter;
  print_char(letter);
  line->room--;
  mem[line->end++] = KEY_ENTER;
  line->room--;
  return KEY_ENTER;
}

/* Letters (and anything from '@' up), space, ENTER, '"', '.' and ','. */
static bool accepted(uint8_t ch) {
  return ch >= 0x40 || ch == '"' || ch == ' ' || ch == KEY_ENTER || ch == '.' || ch == ',';
}

/* The line as read_line leaves it (for the adapter). */
static InputLine last_line;
static bool last_typed;

bool read_line(void) {
  set_word_at(IDLE_TIMER, 3000);
  mem[V_PRINT_TO_INPUT] = 1;
  mem[V_DOING] = 1;
  print_char('>');
  print_char(' ');
  InputLine line = {INPUT_BUFFER, 0x80};
  bool typed = false; /* a key other than '@' has been read since the line was last empty */
  for (;;) {
    uint8_t key = get_input_key(&line);
    if (line_empty(&line) && key == KEY_REPEAT) {
      if (mem[V_QUESTION]) continue; /* not while a question waits for its answer */
      print_char(KEY_DELETE);
      print_char(KEY_DELETE);
      last_line = line, last_typed = typed;
      return false;
    }
    if (!typed) key = direction_key(key, &line);
    typed = true;
    if (key == KEY_CLEAR) {
      clear_line(&line);
      typed = false;
      continue;
    }
    if (key == KEY_DELETE) {
      if (line_empty(&line)) continue;
      print_char(KEY_DELETE);
      line.room++;
      line.end--;
      continue;
    }
    if (!accepted(key)) continue;
    mem[LAST_KEY] = key;
    if (line.room != 0) {
      print_char(key);
      mem[line.end++] = key;
      line.room--;
    }
    if (key == KEY_ENTER) {
      last_line = line, last_typed = typed;
      return true;
    }
  }
}

/* ---------- splitting the line into words ---------- */

/* The dictionary, at $6000: a word per letter code (0-31) giving the
 * offset of the first entry for words starting with it, then the entries.
 * An entry is a run of bytes, each with a 5-bit letter code (0 for none)
 * in bits 0-4. It ends at a byte with bit 7 set, except that it always
 * runs to at least 3 bytes, and a third byte with bit 7 set only ends it
 * if the second did not have bit 7 set as well. If bit 6 of its last byte
 * is set, a word follows: the offset of the entry it is a synonym of.
 * Bits 5-6 of the first two bytes give the word's class. */
#define DICTIONARY 0x6000
#define WORD_CODES 0x707A  /* the word being looked up, as letter codes */
#define WORD_LEN 0x708A
#define ENTRY_CODES 0x708B /* the entry being compared, as letter codes */
#define ENTRY_LEN 0x709B
#define ENTRY 0xB717       /* the entry being compared (a word) */

/* The address just after the entry at e's letters (not its link). */
static uint16_t entry_letters_end(uint16_t e) {
  uint8_t n = 0;
  for (;;) {
    e++, n++;
    if (!(mem[(uint16_t)(e - 1)] & 0x80)) continue;
    if (n == 2) continue;
    if (n == 3 && (mem[(uint16_t)(e - 2)] & 0x80)) continue;
    return e;
  }
}

/* Decode the entry at *e into ENTRY_CODES and ENTRY_LEN, and move *e to
 * the next entry. False, with nothing done, if its first letter is not
 * the word's (the entries for the word's letter are over). */
static bool decode_entry(uint16_t *e) {
  if ((mem[*e] & 0x1F) != mem[WORD_CODES]) return false;
  uint16_t end = entry_letters_end(*e);
  uint8_t len = 0;
  for (uint16_t a = *e; a != end; a++)
    if (mem[a] & 0x1F) mem[(uint16_t)(ENTRY_CODES + len++)] = mem[a] & 0x1F;
  mem[ENTRY_LEN] = len;
  *e = mem[(uint16_t)(end - 1)] & 0x40 ? (uint16_t)(end + 2) : end;
  return true;
}

/* Make *e the entry being compared (ENTRY), and decode it. */
static bool next_entry(uint16_t *e) {
  set_word_at(ENTRY, *e);
  return decode_entry(e);
}

/* Do the word and the decoded entry agree, over the shorter length? */
static bool word_matches_entry(void) {
  uint8_t n = mem[ENTRY_LEN] < mem[WORD_LEN] ? mem[ENTRY_LEN] : mem[WORD_LEN];
  uint8_t i = 0;
  do { /* (a length of 0 would compare 256 codes, as the original does) */
    if (mem[(uint16_t)(ENTRY_CODES + i)] != mem[(uint16_t)(WORD_CODES + i)]) return false;
    i++;
  } while (--n);
  return true;
}

/* The word's entry: the first that matches it, where a word matches an
 * entry if one is a prefix of the other; but a word longer than an entry
 * only matches an entry of 4 or more letters, and only if the next entry
 * does not match as well. Returns the entry (ENTRY), or 0 if none. */
static uint16_t look_up(void) {
  uint16_t e = (uint16_t)(DICTIONARY + word_at((uint16_t)(DICTIONARY + 2 * mem[WORD_CODES])));
  if (!next_entry(&e)) return 0;
  for (;;) {
    if (word_matches_entry()) {
      if (mem[ENTRY_LEN] >= mem[WORD_LEN]) break;
      if (mem[ENTRY_LEN] >= 4) {
        uint16_t after = e;
        bool next_matches = decode_entry(&after) && word_matches_entry();
        if (!next_matches) break;
      }
    }
    if (!next_entry(&e)) return 0;
  }
  return word_at(ENTRY);
}

Token get_word(uint16_t *pos) {
  while (mem[*pos] == ' ') (*pos)++;
  set_word_at(WORD_START, *pos);
  uint8_t ch = mem[*pos];
  if (ch == KEY_ENTER) return (Token){WC_END, 0};
  if (ch == '.' || ch == ',' || ch == '"') {
    (*pos)++;
    return (Token){ch == '.' ? WC_THEN : ch == ',' ? WC_AND : WC_SPECIAL, 0};
  }
  /* The letters, as codes (a word of more than 16 letters runs on over
   * what follows, the token list included, as in the original). */
  uint8_t len = 0;
  while (mem[*pos] >= 0x40) {
    mem[(uint16_t)(WORD_CODES + len)] = mem[*pos] & 0x1F;
    (*pos)++, len++;
  }
  mem[WORD_LEN] = len;
  uint16_t e = look_up();
  if (!e) return (Token){WC_UNKNOWN, 0};
  /* A synonym's class and offset are those of the entry it links to. */
  uint16_t end = entry_letters_end(e);
  if (mem[(uint16_t)(end - 1)] & 0x40) e = (uint16_t)(DICTIONARY + word_at(end));
  uint8_t b0 = mem[e], b1 = mem[(uint16_t)(e + 1)];
  uint8_t cls = (uint8_t)(((b0 << 1 | b0 >> 7) & 0xC0) + ((b1 >> 1 | b1 << 7) & 0x30));
  return (Token){cls, (uint16_t)(e - DICTIONARY)};
}

/* ---------- the sentence parser ---------- */

/* The parser reads the tokens one by one, building a noun-phrase record
 * for each phrase of the sentence: the first at PHRASE_RECORDS, the
 * others going down by $18. A record:
 *   +0  the verb (a word; bits 6-7 of its high byte are flags: REC_ALL,
 *       REC_LINKED)
 *   +2  a second word: an adverb, or for GO NORTH, GO (NORTH is the verb)
 *   +4  a phrase: two prepositions, then a noun and two adjectives
 *   +$E another phrase, the same way
 * A phrase is gathered first in the phrase buffer (PHRASE), then copied
 * into the record.
 *
 * Its state is what the original keeps in registers: the class of the
 * word just read, or the class it is being taken as (D); flags saying
 * what may still come (E); the word just read (BC); the record being
 * built (IY). An opening quote saves this state and parses the quoted
 * sentence into the records below; the closing quote copies those records
 * into the speech slots and restores it. The original saves it on the
 * Z80 stack, and leaves it there if the quote is never closed, so that
 * its final RET returns to the saved record pointer (a crash: see
 * crash-repeat.txt). Here it is kept on the parser's own stack of saved
 * words, pushed and popped as the original does, which the adapters copy
 * to and from the Z80 stack where the original has it. */

#define RECORD_SIZE 0x18
enum { REC_VERB = 0, REC_SECOND = 2, REC_PHRASE1 = 4, REC_PHRASE2 = 0x0E, REC_PHRASE1_NOUN = 8, REC_PHRASE2_NOUN = 0x12 };
enum { REC_ALL = 0x80, REC_LINKED = 0x40 }; /* bits of the record's byte +1 */

#define PHRASE 0x757B      /* the phrase being gathered: 5 words */
#define PHRASE_PREPS 0x757B /* two prepositions */
#define PHRASE_NOUN 0x757F
#define PHRASE_ADJS 0x7581 /* two adjectives */
#define PHRASE_LEN 10
#define PHRASE_WORDS 0x757A /* prepositions counted in the phrase */

#define PREV_CLASS 0xB6DE   /* the class (D) before the last token was read */
#define MODE 0xB719         /* 1 after ALL, 2 after ALL EXCEPT */
#define LAST_PHRASE 0xB6E0  /* the phrase IT stands for */
#define PRINTER_ON 0xB6F2   /* copy the output to the ZX Printer */

/* Where the sentence was at its last ',' or AND: if a verb follows, the
 * sentence ends there. */
#define COMMA_TOKEN 0x7574
#define COMMA_FLAGS 0x7576
#define COMMA_RECORD 0x7577
#define COMMA_COUNT 0x7579

#define QUOTE_CLASS 0x824F  /* the class (D) at an opening quote */
#define QUOTE_COUNT 0x8250  /* PHRASE_COUNT at an opening quote */
#define SPEECH_COUNT 0xB737 /* records copied into the speech slots */
#define SPEECH_SLOT_SIZE 0x19
#define SPEECH_SLOTS_N 8

#define CLASS_TABLE 0x75D2   /* the original's jump table on the class */
#define SPECIAL_WORDS 0x8271 /* 13 words (offsets) handled by the routines in SPECIAL_ROUTINES */
#define SPECIAL_ROUTINES 0x828B
#define SPECIAL_COUNT 13

/* Flags (E): what may still come in the phrase or sentence. */
enum {
  F_NOT_GO = 0x01,     /* cleared by GO or RUN: a direction may follow */
  F_VERB_OK = 0x02,    /* no verb yet */
  F_ADVERB_OK = 0x04,  /* no adverb yet */
  F_NO_COMMA = 0x08,   /* cleared at ',' or AND */
  F_NO_ARTICLE = 0x10, /* no article yet in this group of words */
  F_PHRASE1_OK = 0x40, /* the record's first phrase is free */
  F_PHRASE2_OK = 0x80, /* its second phrase is free */
};

#define SAVED_MAX 1024

typedef struct {
  uint8_t cls;      /* D */
  uint8_t flags;    /* E */
  uint16_t word;    /* BC: the word just read (its offset) */
  uint16_t record;  /* IY */
  int saved_n;      /* the stack of saved words */
  uint16_t saved[SAVED_MAX];
} Parser;

static void push_saved(Parser *p, uint16_t w) {
  if (p->saved_n < SAVED_MAX) p->saved[p->saved_n++] = w;
}
static uint16_t pop_saved(Parser *p) { return p->saved_n ? p->saved[--p->saved_n] : 0; }

static uint8_t rec(uint16_t r, int off) { return mem[(uint16_t)(r + off)]; }
static void set_rec(uint16_t r, int off, uint8_t v) { mem[(uint16_t)(r + off)] = v; }
static uint16_t rec_word(uint16_t r, int off) { return word_at((uint16_t)(r + off)); }

/* Copy n bytes forwards, a byte at a time (as LDIR). */
static void copy_bytes(uint16_t to, uint16_t from, unsigned n) {
  while (n--) mem[to++] = mem[from++];
}

static bool in_quotes(void) { return mem[IN_QUOTES] != 0; }

/* Read the next token. */
static void next_token(Parser *p) {
  uint16_t t = word_at(TOKEN_PTR);
  set_word_at(WORD_START, t);
  mem[PREV_CLASS] = p->cls;
  p->cls = mem[t] & 0xF0;
  p->word = (uint16_t)((mem[t] & 0x0F) << 8 | mem[(uint16_t)(t + 1)]);
  set_word_at(TOKEN_PTR, (uint16_t)(t + 2));
}

/* Clear the record r, and the first word of the one below it. */
static void clear_record(uint16_t r) {
  zero_bytes(r, RECORD_SIZE);
  set_rec(r, -0x18, 0);
  set_rec(r, -0x17, 0);
}

/* The nearest record above (step $18) or below (-$18) r that is not
 * linked to the one above it. */
static uint16_t find_record(uint16_t r, int step) {
  do r = (uint16_t)(r + step);
  while (rec(r, 1) & REC_LINKED);
  return r;
}

/* Move up to the next record, out of ALL. There is none above the first:
 * false then. (The original's bug moves up anyway, to the location table
 * above the records, and writes the phrase over it: "ALL BUT AN ALL ...".) */
static bool record_up(Parser *p) {
  uint16_t r = find_record(p->record, RECORD_SIZE);
  if (r > PHRASE_RECORDS && !original_bugs) return false;
  p->record = r;
  mem[MODE] = 0;
  return true;
}

/* Store the word in the first free one of the two slots at slots. False
 * if both are taken. */
static bool store_in_slot(uint16_t slots, uint16_t word) {
  if (word_at(slots) != 0) {
    slots += 2;
    if (word_at(slots) != 0) return false;
  }
  set_word_at(slots, word);
  return true;
}

/* Copy the phrase buffer into the record's first or second phrase. */
static void phrase_to_first(Parser *p) {
  p->flags &= ~F_PHRASE1_OK;
  copy_bytes((uint16_t)(p->record + REC_PHRASE1), PHRASE, PHRASE_LEN);
}
static void phrase_to_second(Parser *p) {
  p->flags &= ~F_PHRASE2_OK;
  copy_bytes((uint16_t)(p->record + REC_PHRASE2), PHRASE, PHRASE_LEN);
}

/* Store a word as the record's verb. */
static void store_verb(Parser *p, uint16_t word) {
  p->flags &= ~F_VERB_OK;
  set_word_at(p->record, word);
}

/* Where the parser goes next (the original's labels in brackets). */
typedef enum {
  BEGIN,        /* a sentence [$758A] */
  BEGIN_QUOTED, /* a quoted sentence, inside the one being parsed */
  NEW_PHRASE,   /* a new record [$75A0] */
  NEW_GROUP,    /* a new group of words in it [$75B4] */
  NEXT_WORD,    /* [$75BE] */
  RESUME,       /* after a special word: carry on [$82B3] */
  END_SENTENCE, /* [$75FA] */
  CLOSE,        /* finish the records [$7614] */
  REJECT,       /* a word out of place: "what ?", unless quoted [$7929] */
  FAIL,         /* "what ?" [$792F] */
  DONE,         /* a sentence is ready (Z) */
  NOTHING,      /* nothing to parse ($75EC) */
} Step;

static Step run_command(uint16_t routine);

/* A word that cannot be taken: stop with "what ?", unless quoted. */
static bool give_up(void) { return !in_quotes(); }

/* An adverb (or a direction after the verb) [$76F2]. */
static Step on_adverb(Parser *p) {
  if (!(p->flags & F_ADVERB_OK)) return REJECT;
  if (mem[MODE] == 2 && !record_up(p)) return REJECT;
  p->flags &= ~F_ADVERB_OK;
  set_word_at((uint16_t)(p->record + REC_SECOND), p->word);
  return NEW_GROUP;
}

/* A verb [$7733]. */
static Step on_verb(Parser *p) {
  if (!(p->flags & F_NO_COMMA) && !in_quotes()) {
    /* After a ',' or AND: the sentence ended there. */
    set_word_at(TOKEN_PTR, word_at(COMMA_TOKEN));
    p->cls = WC_THEN;
    p->flags = mem[COMMA_FLAGS];
    mem[PHRASE_COUNT] = mem[COMMA_COUNT];
    p->record = word_at(COMMA_RECORD);
    return END_SENTENCE;
  }
  mem[MODE] = 0;
  if (!(p->flags & F_VERB_OK)) return REJECT;
  if (p->flags & F_NOT_GO) {
    store_verb(p, p->word);
    return NEW_GROUP;
  }
  /* After GO or RUN: past any adverbs, a direction is the verb, and GO
   * the second word. Otherwise GO is the verb, and the words after it are
   * read again. */
  uint8_t cls = p->cls, flags = p->flags;
  uint16_t go = p->word, after = word_at(TOKEN_PTR);
  do next_token(p);
  while (p->cls == WC_ADVERB);
  p->flags |= F_NOT_GO;
  if (p->cls != WC_DIRECTION) {
    set_word_at(TOKEN_PTR, after);
    p->word = go, p->cls = cls, p->flags = flags;
    store_verb(p, go);
    return NEW_GROUP;
  }
  store_verb(p, p->word);
  p->word = go;
  set_word_at((uint16_t)(p->record + REC_SECOND), go);
  return NEW_GROUP;
}

/* GO, RUN [$772F]. */
static Step on_go(Parser *p) {
  p->flags &= ~F_NOT_GO;
  p->cls = WC_VERB;
  return on_verb(p);
}

/* A direction: the verb if there is none yet, else an adverb [$76EC]. */
static Step on_direction(Parser *p) {
  if (p->flags & F_VERB_OK) {
    p->cls = WC_VERB;
    return on_verb(p);
  }
  p->cls = WC_ADVERB;
  return on_adverb(p);
}

/* ',' or AND [$770B]: note where the sentence is, in case a verb follows
 * (then the sentence ends here), and end the phrase. */
static Step on_and(Parser *p) {
  p->flags &= ~F_NO_COMMA;
  uint8_t cls = p->cls, flags = p->flags;
  do next_token(p);
  while (p->cls == WC_AND);
  p->cls = cls, p->flags = flags;
  uint16_t t = (uint16_t)(word_at(TOKEN_PTR) - 2);
  set_word_at(COMMA_TOKEN, t);
  set_word_at(TOKEN_PTR, t);
  mem[COMMA_FLAGS] = p->flags;
  set_word_at(COMMA_RECORD, p->record);
  mem[COMMA_COUNT] = mem[PHRASE_COUNT];
  return END_SENTENCE;
}

/* The end of a phrase [$77D7]: into the record. */
static Step end_phrase(Parser *p) {
  p->cls = WC_NOUN;
  if (mem[MODE] == 2) { /* ALL EXCEPT this */
    if (word_at(PHRASE_PREPS) == 0) {
      if (!(rec(p->record, RECORD_SIZE + 1) & REC_LINKED)) {
        p->record -= RECORD_SIZE;
        clear_record(p->record);
      }
      phrase_to_first(p);
      set_rec(p->record, 1, REC_LINKED);
      return NEW_GROUP;
    }
    if (!record_up(p)) return REJECT;
  }
  if (p->flags & F_PHRASE1_OK)
    phrase_to_first(p);
  else if (p->flags & F_PHRASE2_OK)
    phrase_to_second(p);
  else if (give_up())
    return FAIL;
  return NEW_GROUP;
}

/* Adjectives, then the noun, if there is one [$77B9]. */
static Step adjectives_and_noun(Parser *p) {
  for (;;) {
    if (p->cls == WC_ADJECTIVE) {
      if (!store_in_slot(PHRASE_ADJS, p->word) && give_up()) return FAIL;
      next_token(p);
      continue;
    }
    if (p->cls == WC_NOUN)
      set_word_at(PHRASE_NOUN, p->word);
    else /* not part of the phrase: read it again */
      set_word_at(TOKEN_PTR, word_at(WORD_START));
    return end_phrase(p);
  }
}

/* A preposition [$781C]: at most two to a phrase. False to give up. */
static bool store_preposition(Parser *p) {
  if (++mem[PHRASE_WORDS] >= 3 || !store_in_slot(PHRASE_PREPS, p->word)) return !give_up();
  return true;
}

/* Prepositions and articles, then adjectives and the noun [$77A2]. */
static Step on_preposition(Parser *p) {
  for (;;) {
    if (!store_preposition(p)) return FAIL;
    do {
      next_token(p);
      if (p->cls == WC_ARTICLE) {
        if (!(p->flags & F_NO_ARTICLE) && give_up()) return FAIL;
        p->flags &= ~F_NO_ARTICLE;
      }
    } while (p->cls == WC_ARTICLE);
    if (p->cls != WC_PREPOSITION) return adjectives_and_noun(p);
  }
}

/* IN, INTO [$7795]: the verb (ENTER) straight after a ',' or AND, while
 * there is no verb; otherwise a preposition. */
static Step on_in(Parser *p) {
  if ((p->flags & F_VERB_OK) && mem[PREV_CLASS] == WC_AND) {
    p->cls = WC_VERB;
    return on_verb(p);
  }
  p->cls = WC_PREPOSITION;
  return on_preposition(p);
}

/* An opening quote [$82FD]: save the sentence's state, and parse the
 * quoted sentence into the records below. */
static Step open_quote(Parser *p) {
  mem[QUOTE_CLASS] = p->cls;
  push_saved(p, (uint16_t)(p->cls << 8 | p->flags));
  push_saved(p, p->word);
  push_saved(p, p->record);
  mem[QUOTE_COUNT] = mem[PHRASE_COUNT];
  p->record -= RECORD_SIZE;
  return BEGIN_QUOTED;
}

/* A closing quote [$8315]: copy the quoted records into free speech
 * slots (a slot is $FF then the record; $00 for a linked record), and go
 * back to the sentence the quote is in. */
static Step close_quote(Parser *p) {
  mem[IN_QUOTES]--;
  uint8_t n = (uint8_t)(mem[PHRASE_COUNT] - mem[QUOTE_COUNT]);
  uint8_t copied = 0;
  if (n) {
    uint16_t r = (uint16_t)(p->saved_n ? p->saved[p->saved_n - 1] - RECORD_SIZE : -RECORD_SIZE);
    do {
      uint16_t slot = SPEECH_SLOTS;
      int k = 0;
      while (k < SPEECH_SLOTS_N && mem[slot]) slot += SPEECH_SLOT_SIZE, k++;
      mem[slot] = 0xFF; /* (all full: the byte after the slots) */
      if (k == SPEECH_SLOTS_N) break;
      if (rec(r, 1) & REC_LINKED)
        mem[slot] = 0;
      else
        copy_bytes((uint16_t)(slot + 1), r, RECORD_SIZE);
      r -= RECORD_SIZE;
      copied++;
    } while (--n);
  }
  mem[MODE] = 0;
  mem[SPEECH_COUNT] = copied;
  p->record = pop_saved(p);
  mem[PHRASE_COUNT] = mem[QUOTE_COUNT];
  p->word = pop_saved(p);
  p->flags = (uint8_t)pop_saved(p);
  p->cls = mem[QUOTE_CLASS];
  return NEW_GROUP;
}

/* IT: the phrase last mentioned. */
static Step on_it(Parser *p) {
  copy_bytes(PHRASE_NOUN, LAST_PHRASE, 6);
  p->word = 0;
  if (p->flags & F_PHRASE2_OK)
    phrase_to_first(p);
  else
    phrase_to_second(p);
  return NEW_GROUP;
}

/* The special words [$8251]: each has its routine, named in a table. */
static Step on_special(Parser *p) {
  for (int i = 0; i < SPECIAL_COUNT; i++) {
    uint16_t w = (uint16_t)(SPECIAL_WORDS + 2 * i);
    if (mem[w] != (uint8_t)p->word || mem[(uint16_t)(w + 1)] != p->word >> 8) continue;
    uint16_t routine = word_at((uint16_t)(SPECIAL_ROUTINES + 2 * i));
    switch (routine) {
    case 0x8315: return in_quotes() ? close_quote(p) : open_quote(p); /* '"' */
    case 0x82D2: /* ALL */
      if (mem[MODE] != 2) mem[MODE] = 1;
      return NEW_GROUP;
    case 0x82BA: /* EXCEPT, BUT: only after ALL */
      if (mem[MODE] != 1) return REJECT;
      mem[MODE] = 2;
      set_rec(p->record, 1, rec(p->record, 1) | REC_ALL);
      return NEW_GROUP;
    case 0x82E2: return on_it(p);
    case 0x75B4: return NEW_GROUP; /* ONE */
    case 0x82A5: /* PRINT: if there is a ZX Printer (bit 6 of its port clear) */
      if (!(device_in((uint16_t)((p->word >> 8) << 8 | 0xFB), 0x82A5) & 0x40)) mem[PRINTER_ON] = 1;
      return RESUME;
    case 0x82AF: /* NOPRINT */
      mem[PRINTER_ON] = 0;
      return RESUME;
    default: return run_command(routine); /* SAVE, LOAD, QUIT, HELP, SCORE, PAUSE */
    }
  }
  /* Not in the table (no word of the dictionary, but a token read from
   * past the end of the list can be anything): the original goes on
   * with a new phrase, leaving the class and flags pushed. */
  push_saved(p, (uint16_t)(p->cls << 8 | p->flags));
  return NEW_PHRASE;
}

/* The end of a sentence ('.', THEN, the end of the line) [$75FA]. */
static Step end_sentence(Parser *p) {
  if ((p->flags & F_VERB_OK) && mem[PHRASE_COUNT] == 0) { /* nothing in it */
    if (mem[PREV_CLASS] == WC_END && p->cls == WC_END) return NOTHING;
    if (give_up()) return FAIL;
  }
  return CLOSE;
}

/* The answer to a "which ...?" question: fill in the words it leaves out
 * from the question's phrase (V_QUESTION: its offset in the first
 * record) [$7638]. The answer's phrase is the first of the record below
 * the first, if it is empty or has the same noun, else its second, if
 * that is empty. */
static void complete_answer(void) {
  uint16_t question = (uint16_t)(PHRASE_RECORDS + mem[V_QUESTION]);
  uint16_t answer = PHRASE_RECORDS - RECORD_SIZE, to;
  if (rec_word(answer, REC_PHRASE1_NOUN) == 0 || rec_word(answer, REC_PHRASE1_NOUN) == word_at(question))
    to = (uint16_t)(answer + REC_PHRASE1_NOUN);
  else if (rec_word(answer, REC_PHRASE2_NOUN) == 0)
    to = (uint16_t)(answer + REC_PHRASE2_NOUN);
  else
    return;
  for (int i = 0; i < 3; i++)
    if (word_at((uint16_t)(to + 2 * i)) == 0) set_word_at((uint16_t)(to + 2 * i), word_at((uint16_t)(question + 2 * i)));
}

/* Copy a phrase (at off) and, if to has none, the second word, from one
 * record to another [$78EA]. */
static void copy_phrase_fields(uint16_t to, uint16_t from, int off) {
  copy_bytes((uint16_t)(to + off), (uint16_t)(from + off), PHRASE_LEN);
  if (rec_word(to, REC_SECOND) == 0) copy_bytes((uint16_t)(to + REC_SECOND), (uint16_t)(from + REC_SECOND), 2);
}

/* A record with no verb takes the one above's [$78B7]; and if the
 * record above has a first phrase, and the second phrases are free, the
 * record's own first phrase becomes its second and the one above's
 * first is copied in. */
static void merge_into(const Parser *p, uint16_t to, uint16_t from) {
  set_rec(to, 1, rec(to, 1) | (rec(from, 1) & 0x7F));
  set_rec(to, 0, rec(from, 0));
  if (rec_word(to, RECORD_SIZE + REC_PHRASE1) == 0) return;
  if (!(p->flags & F_PHRASE2_OK)) return;
  copy_bytes((uint16_t)(to + REC_PHRASE2_NOUN), (uint16_t)(to + REC_PHRASE1_NOUN), 6);
  copy_phrase_fields(to, from, REC_PHRASE1);
}

/* Share out the verbs and phrases between the sentence's records
 * [$7682]: going down, a record with no verb takes the one above's;
 * going up, a record with the same verb as the one below, and no second
 * phrase, takes the one below's. */
static void share_records(const Parser *p) {
  uint8_t count = mem[PHRASE_COUNT];
  uint8_t n = (uint8_t)(count - 1);
  uint16_t r = find_record(0xB9E0, -RECORD_SIZE);
  while (n) {
    n--;
    uint16_t above = r;
    r = find_record(r, -RECORD_SIZE);
    if (((rec(r, 1) & 0x7F) | rec(r, 0)) == 0) merge_into(p, r, above);
  }
  n = (uint8_t)(count - 1);
  r = find_record(p->record, RECORD_SIZE);
  while (n) {
    n--;
    uint16_t below = r;
    r = find_record(r, RECORD_SIZE);
    if (rec_word(r, REC_PHRASE2_NOUN) != 0) continue;
    if (rec(r, 0) != rec(below, 0) || rec(r, 1) != rec(below, 1)) continue;
    if (rec_word(below, REC_PHRASE2) == 0) continue;
    copy_phrase_fields(r, below, REC_PHRASE2);
  }
}

/* Finish the sentence's records [$7614]. */
static Step close_sentence(Parser *p) {
  if (mem[MODE] == 1) set_rec(p->record, 1, rec(p->record, 1) | REC_ALL);
  if (mem[MODE] != 2 || p->cls != WC_AND) mem[PHRASE_COUNT]++;
  p->record = find_record(p->record, -RECORD_SIZE);
  if (p->cls == WC_AND) return NEW_PHRASE;
  if (mem[V_QUESTION]) complete_answer();
  share_records(p);
  if (mem[MORE_ON_LINE] && in_quotes()) return NEW_PHRASE; /* the next quoted sentence */
  return DONE;
}

/* Read a word and act on its class [$75BE]. */
static Step next_word(Parser *p) {
  next_token(p);
  switch (p->cls) {
  case WC_ADVERB: return on_adverb(p);
  case WC_IN: return on_in(p);
  case WC_DIRECTION: return on_direction(p);
  case WC_VERB: return on_verb(p);
  case WC_GO: return on_go(p);
  case WC_NOUN:
  case WC_ADJECTIVE: return adjectives_and_noun(p);
  case WC_PREPOSITION: return on_preposition(p);
  case WC_ARTICLE:
    p->flags &= ~F_NO_ARTICLE;
    return NEXT_WORD;
  case WC_SPECIAL: return on_special(p);
  case WC_AND: return on_and(p);
  case WC_THEN: return END_SENTENCE;
  case WC_END:
    mem[MORE_ON_LINE] = 0;
    return END_SENTENCE;
  default: /* junk read from past the tokens: the original's bug runs its
            * jump table on into code, and jumps to what that says (into
            * the ROM, for each class there can be); it is the end of the
            * line */
    if (original_bugs) device_crash(word_at((uint16_t)(CLASS_TABLE + (p->cls >> 3))));
    mem[MORE_ON_LINE] = 0;
    return END_SENTENCE;
  }
}

static uint8_t what_a; /* A as the original leaves it after "what ?" */

static uint8_t what(void) {
  mem[V_PRINT_TO_INPUT] = 1;
  print_message(MSG_WHAT, NULL);
  return what_a = 1; /* PrintMsg gives back the A it had: 1, ORed with 1 */
}

void say_what(void) { what(); }

/* Parse from step until the sentence is done or abandoned. */
static ParseResult run(Parser *p, Step step) {
  for (;;) {
    switch (step) {
    case BEGIN:
    case BEGIN_QUOTED:
      mem[IN_QUOTES] = step == BEGIN_QUOTED;
      mem[MODE] = 0;
      mem[PHRASE_COUNT] = 0;
      p->flags = 0xFF;
      if (mem[V_QUESTION]) { /* this line answers a question */
        p->cls = WC_AND;
        step = CLOSE;
        break;
      }
      p->cls = WC_END;
      step = NEW_PHRASE;
      break;
    case NEW_PHRASE:
      mem[PHRASE_WORDS] = 0;
      p->flags |= F_PHRASE2_OK | F_PHRASE1_OK | F_ADVERB_OK | F_VERB_OK | F_NOT_GO;
      if (mem[MODE] == 2) p->flags &= ~F_VERB_OK;
      clear_record(p->record);
      step = NEW_GROUP;
      break;
    case NEW_GROUP:
      zero_bytes(PHRASE, PHRASE_LEN);
      p->flags |= F_NO_ARTICLE;
      step = NEXT_WORD;
      break;
    case NEXT_WORD: step = next_word(p); break;
    case RESUME:
      p->cls = mem[PREV_CLASS];
      step = NEW_GROUP;
      break;
    case END_SENTENCE: step = end_sentence(p); break;
    case CLOSE: step = close_sentence(p); break;
    case REJECT: step = in_quotes() ? NEW_GROUP : FAIL; break;
    case FAIL: what(); return PARSE_FAILED;
    case DONE: return PARSED;
    case NOTHING: return PARSE_NOTHING;
    }
  }
}

/* SAVE, LOAD, QUIT, HELP, SCORE, PAUSE: the game module's commands. The
 * original jumps to them, and they come back to the parser at $82B3 (or
 * $75B4: HELP, when someone else is doing it), or restart the game. */
static Step run_command(uint16_t routine) {
  CommandResult r;
  switch (routine) {
  case 0x8391: r = cmd_quit(); break;
  case 0x83A0: r = cmd_help(); break;
  case 0x83EF: r = cmd_score(); break;
  case 0x843A: r = cmd_pause(); break;
  case 0x8451: r = cmd_load(); break;
  case 0x84CC: r = cmd_save(); break;
  default: device_crash(routine); /* (the table has no other) */
  }
  switch (r) {
  case CMD_CONTINUE: return RESUME;
  case CMD_SKIP: return NEW_GROUP;
  default: device_restart();
  }
}

/* A sentence. If the parser's stack is not empty at the end (a quote
 * left open: the main loop no longer lets one through), the original's
 * RET returns into its top word, a crash: with the bugs fixed, what is
 * left is dropped. */
ParseResult parse_sentence(void) {
  Parser p = {.record = PHRASE_RECORDS};
  ParseResult r = run(&p, BEGIN);
  if (p.saved_n && original_bugs) device_crash(p.saved[p.saved_n - 1]);
  return r;
}

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ---------- adapters (for the faithful callers, while they remain) ---------- */


/* The parser's stack of saved words, to the Z80 stack, where the
 * original keeps it. */
static void push_saved_words(Cpu *c, const Parser *p) {
  for (int i = 0; i < p->saved_n; i++) push16(c, p->saved[i]);
}

/* The parser's state in the original's registers. */
static void parser_to_regs(Cpu *c, const Parser *p) {
  c->d = p->cls, c->e = p->flags;
  set_bc(c, p->word);
  c->iy = p->record;
}

/* $7585, a new sentence. The result: Z (A = 0) when a sentence is ready,
 * else NZ; the main loop uses Z only (Execute starts with XOR A and sets
 * IY). If the parser's stack is not empty at the end (a quote never
 * closed), the RET returns to its top word, as in the original.
 * (The commands come back to the parser at $82B3 and $75B4 only from
 * faithful parsers now; the clean parser calls them.) */
static void a_parse_sentence(Cpu *c) {
  Parser p = {.cls = c->d, .flags = c->e, .word = get_bc(c), .record = PHRASE_RECORDS};
  ParseResult r = run(&p, BEGIN);
  push_saved_words(c, &p);
  parser_to_regs(c, &p);
  c->cf = false;
  switch (r) {
  case PARSED: c->a = 0; break;
  case PARSE_FAILED: c->a = what_a; break;
  default: c->a = (uint8_t)(mem[IN_QUOTES] - 1); break; /* LD A,($B71B); DEC A */
  }
  c->zf = c->a == 0;
}

/* $792F: A|1 and NZ; its callers (the main loop, for an unclosed quote)
 * use nothing. */
static void a_what(Cpu *c) {
  c->a = what();
  c->zf = false, c->cf = false;
}

/* $6DD6: Z (A = 0) for '@', NZ (A = ENTER) for a line; B the room left,
 * HL the end of the line, C 1 once a key has been typed. The main loop
 * uses Z. */
static void a_read_line(Cpu *c) {
  bool line = read_line();
  c->a = line ? KEY_ENTER : 0;
  c->zf = !line;
  c->b = last_line.room;
  c->c = last_typed;
  set_hl(c, last_line.end);
}

/* $6E8B: B (room) and HL (end) in and out, A the last character printed.
 * Its callers ($6E0C in ReadLine, $725A in GetInputKey) use B and HL. */
static void a_clear_line(Cpu *c) {
  InputLine line = {get_hl(c), c->b};
  bool any = !line_empty(&line);
  clear_line(&line);
  if (any) c->a = KEY_DELETE;
  c->b = line.room;
  set_hl(c, line.end);
  c->zf = false;
}

/* $6E97: HL in, the line; out: A and D the class, BC the offset with
 * the class added to B, HL after the word; E kept. The main loop ($6D31)
 * uses A, BC, D and HL (not IX, which the original leaves in the
 * dictionary). HL is always in the input line, which ends in CR: the one
 * caller starts it at $6FF9 and goes on from where the last word ended;
 * the check does not mutate it (elsewhere a word can run on for ever). */
static void a_get_word(Cpu *c) {
  uint16_t pos = get_hl(c);
  Token t = get_word(&pos);
  set_hl(c, pos);
  c->a = c->d = t.cls;
  c->b = (uint8_t)(t.cls + (t.offset >> 8));
  c->c = (uint8_t)t.offset;
}

const CleanRoutine parser_clean[] = {
    {0x6DD6, "read_line", a_read_line, OUT_A | OUT_BC | OUT_HL | OUT_ZF},
    {0x6E8B, "clear_line", a_clear_line, OUT_B | OUT_HL},
    {0x6E97, "get_word", a_get_word, OUT_A | OUT_BC | OUT_DE | OUT_HL, OUT_HL},
    {0x7585, "parse_sentence", a_parse_sentence, OUT_A | OUT_ZF},
    {0x792F, "say_what", a_what, OUT_A | OUT_ZF | OUT_CF},
    {0, NULL, NULL, 0},
};

const CleanScratch parser_scratch[] = {
    /* PrintMsg ($72DD, text) stashes DE and IX here on entry, and reads
     * them back only while printing that message (for its parameters).
     * "what ?" has none, and the parser does not keep the registers the
     * original happens to have there (the dictionary or a record pointer
     * in IX). */
    {0x70DD, 0x70DE, "PrintMsg's copy of DE, not read after the message"},
    {0x70E0, 0x70E1, "PrintMsg's copy of IX, not read after the message"},
};

#endif /* CLEAN_ADAPTERS */
