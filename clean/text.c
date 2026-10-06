/* The clean edition: text. See docs/CLEAN.md and text.h.
 *
 * Words are printed through the output buffer at $74A6 (20 bytes, game
 * memory: a longer word runs on over the code after it, as in the
 * original), after a space unless at the start of a line, and on a new
 * line if they do not fit. */
#include "text.h"

#include "characters.h"
#include "objects.h"
#include "parser.h"
#include "screen.h"

#include <stddef.h>

/* Game memory used here. */
#define IN_ACTION 0x70DF     /* set while describe_action prints: articles become "the" */
#define WAIT_TEXT 0x7291     /* "WAIT", typed by the idle timer */
#define OUT_BUF 0x74A6       /* the word being printed */
#define OUT_BUF_LEN 20
#define DICTIONARY 0x6000    /* tokens are offsets from here */
#define ACTIONS 0xAB4B       /* action records, 8 bytes each */
#define ARTICLES 0xAD2D      /* 8 tokens, picked by bits 4-6 of a noun token */
#define ARTICLES_DEFINITE 0xAD35 /* the same, on the input line and in actions */
#define COMMON_WORDS 0xAD3D  /* 32 tokens (high byte less $50) */
#define MATCHED_FIRST 0xB6DF /* set when a phrase's first word matches */
#define TOKEN_ARG 0xB6FC     /* the token control code $03 prints */
#define OBJ1_IS_PLACE 0xB6FE /* V_OBJECT1 is a location */
#define OBJ2_IS_PLACE 0xB6FF /* V_OBJECT2 is a location */
#define ARTICLE 0xB703       /* print an article before the next noun */
#define CAPITAL 0xB704       /* the next word starts a sentence */
#define IDLE_TIMER 0xB714    /* keyboard scans before WAIT is typed */
#define ACTION_FLAGS1 0xB71D /* from the action record: bytes 1 and 3 */
#define ACTION_FLAGS2 0xB71E /* bytes 5 and 7 */
#define INFLECTIONS 0xB71F   /* 8 suffixes of 4 bytes: s es ies \bies d ing */
#define ROOM_INPUT 0x85B3    /* room left on the input line */
#define ROOM_WINDOW 0x869B   /* room left on the text window's line */
#define MID_LINE 0x86A0      /* nonzero when not at the start of a line */

/* Tokens. */
enum {
  TOK_CANNOT = 0x00EE,
  TOK_SOMEONE = 0x0AE3,
  TOK_SOMEWHERE = 0x0AEA,
  TOK_HIS = 0x0990,
  TOK_YOUR = 0x0BEA,
  TOK_IS = 0x039B,
  TOK_ARE = 0x0065,
  TOK_YOU = 0x07A8,
};

/* ---------- other modules ---------- */

static void put_char(uint8_t ch) { print_char(ch); }
static void newline(void) { print_newline(); }

/* ---------- tables and lookups ---------- */

void zero_bytes(uint16_t addr, unsigned n) {
  for (unsigned i = 0; i < n; i++) mem[(uint16_t)(addr + i)] = 0;
}

uint16_t action_record(uint8_t action) { return (uint16_t)(ACTIONS + 8 * action); }

/* The flags of an action are the high nibbles of its record's bytes 1
 * and 3 (into $B71D, low and high) and 5 and 7 (into $B71E). */
uint8_t load_action_flags(uint16_t record) {
  const uint8_t *r = &mem[record];
  mem[ACTION_FLAGS2] = (uint8_t)((r[7] & 0xF0) | r[5] >> 4);
  mem[ACTION_FLAGS1] = (uint8_t)((r[3] & 0xF0) | r[1] >> 4);
  return mem[ACTION_FLAGS1];
}

/* The name of location `loc` (for a location that does not exist, the
 * original's lookup leaves its pointer as it was: here 0, so the name is
 * at 2). */
uint16_t location_name(uint8_t loc) {
  LocationRecord *l = location_record(loc);
  return (uint16_t)((l ? addr_of(l) : 0) + 2);
}

uint16_t object_name(uint8_t obj) { return (uint16_t)(object_record_addr(obj) + 8); }

/* $722E: does word a (of an action) match word b (of a phrase)? An empty
 * word matches anything; otherwise their 12-bit offsets must be equal. */
static bool word_matches(uint16_t a, uint16_t b) {
  uint16_t wa = word_at(a), wb = word_at(b);
  return wa == 0 || ((wa ^ wb) & 0x0FFF) == 0;
}

bool phrase_matches(uint16_t a, uint16_t b, bool *swapped) {
  *swapped = false;
  if (!word_matches(a, b)) return false;
  mem[MATCHED_FIRST] = 1;
  if (word_matches(a + 2, b + 2) && word_matches(a + 4, b + 4)) return true;
  *swapped = true;
  return word_matches(a + 2, b + 4) && word_matches(a + 4, b + 2);
}

/* ---------- tokens ---------- */

/* Decode the letters of dictionary entry `entry` into the output buffer
 * from `out`; returns where they end. Each byte holds a letter in its low
 * 5 bits ($00 ends the entry early); bit 7 marks the last, except that an
 * entry runs to at least 3 letters (or 4, when the 2nd is marked too). */
/* (Only a word read from garbage can be longer than the buffer's 20
 * bytes; the original's bug writes on, over its code: it is stopped at
 * the end of the buffer.) */
static bool buffer_full(uint16_t out) { return out >= OUT_BUF + OUT_BUF_LEN && !original_bugs; }

static uint16_t decode_letters(uint16_t entry, uint16_t out) {
  uint16_t p = entry;
  uint8_t n = 0;
  for (;;) {
    uint8_t b = mem[p++];
    if ((b & 0x1F) == 0 || buffer_full(out)) break;
    n++;
    mem[out++] = (uint8_t)((b & 0x1F) + 0x60);
    if (!(b & 0x80)) continue;
    if (n == 2) continue;
    if (n == 3 && (mem[(uint16_t)(p - 2)] & 0x80)) continue;
    break;
  }
  return out;
}

/* Append the entry's inflection, if it has one (bit 7 of its second
 * byte; bits 5-7 of the third pick the suffix), to the buffer at `out`. */
static uint16_t add_inflection(uint16_t entry, uint16_t out) {
  if (!(mem[(uint16_t)(entry + 1)] & 0x80)) return out;
  uint16_t suffix = (uint16_t)(INFLECTIONS + ((mem[(uint16_t)(entry + 2)] & 0xE0) >> 3));
  for (int i = 0; i < 4 && mem[(uint16_t)(suffix + i)] && !buffer_full(out); i++) mem[out++] = mem[(uint16_t)(suffix + i)];
  return out;
}

/* Is a token of this kind (its high nibble) printed inflected? $5x
 * never, $4x always, $1x if there is a first object, others if the actor
 * is a character (a verb agreeing with its subject). */
static bool inflected(uint8_t kind) {
  if (kind == 0x50) return false;
  if (kind == 0x40) return true;
  return mem[kind == 0x10 ? V_OBJECT1 : V_ACTOR] != 0;
}

/* Print the word in the output buffer (len letters; 0 means 256, as in
 * the original): after a space unless at the start of a line, on a new
 * line if it does not fit; with $7x tokens the next word starts a
 * sentence. */
static void print_word(uint8_t len, uint8_t kind) {
  bool to_input = mem[V_PRINT_TO_INPUT] != 0;
  if (to_input || mem[MID_LINE]) put_char(' ');
  uint8_t room = mem[to_input ? ROOM_INPUT : ROOM_WINDOW];
  if (room < len) {
    uint8_t capital = mem[CAPITAL];
    newline();
    mem[CAPITAL] = capital;
  }
  if (kind == 0x70) mem[CAPITAL] = 1;
  uint16_t p = OUT_BUF;
  unsigned n = len ? len : 256;
  while (n--) put_char(mem[p++]);
}

void print_token(uint16_t token) {
  if ((token & 0x0FFF) == 0) return;
  uint8_t kind = (token >> 8) & 0xF0;
  uint16_t entry = (uint16_t)(DICTIONARY + (token & 0x0FFF));
  uint16_t end = decode_letters(entry, OUT_BUF);
  if (inflected(kind)) end = add_inflection(entry, end);
  print_word((uint8_t)(end - OUT_BUF), kind);
}

/* $74BA: print the action word at `at` (its low 12 bits; inflected if
 * the actor is a character). */
static void print_action_word(uint16_t at) { print_token(word_at(at) & 0x0FFF); }

void print_article(uint16_t token) {
  if (token & 0x8000) { /* a proper noun: a capital, except for "you" */
    if ((token & 0x0FFF) != TOK_YOU) mem[CAPITAL] = 1;
    return;
  }
  uint16_t table = (mem[V_PRINT_TO_INPUT] | mem[IN_ACTION]) ? ARTICLES_DEFINITE : ARTICLES;
  print_token(word_at((uint16_t)(table + ((token >> 11) & 0x1E))));
}

void print_noun_token(uint16_t token) {
  if (mem[ARTICLE]) print_article(token);
  print_token(token & 0x0FFF);
}

/* ---------- names ---------- */

/* Print thing `id`: a location's name if `place`, else an object's. For
 * a location that does not exist, the original's lookup leaves its
 * pointer as it was: at, the message (or record) being printed, whose
 * bytes from at + 2 are then taken for the name. */
static void print_thing(uint8_t id, bool place, uint16_t at) {
  if (!place) {
    print_name(object_name(id));
    return;
  }
  LocationRecord *l = location_record(id);
  print_name((uint16_t)((l ? addr_of(l) : at) + 2));
}

void print_character(uint8_t who) {
  if (who == OBJECT_NONE)
    print_token(TOK_SOMEONE);
  else
    print_thing(who, false, 0);
}

/* $73A7, $73B4, $73C7: the actor, the first object, the second object. */
static void print_actor(void) { print_character(mem[V_ACTOR]); }
static void print_object1(uint16_t at) { print_thing(mem[V_OBJECT1], mem[OBJ1_IS_PLACE] != 0, at); }
static void print_object2(uint16_t at) { print_thing(mem[V_OBJECT2], mem[OBJ2_IS_PLACE] != 0, at); }

/* $740C, $7425, $7430: "X is", or "you are" for character 0 (X with its
 * article if `article`). */
static void print_is_are(uint8_t who, bool article) {
  mem[ARTICLE] = article;
  print_character(who);
  print_token(who ? TOK_IS : TOK_ARE);
}

/* $73FC: "his", or "your" for character 0. */
static void print_his_your(uint8_t who) { print_token(who ? TOK_HIS : TOK_YOUR); }

/* $72C3: print a character; a new line ends the sentence's capital. */
static void print_char_cr(uint8_t ch) {
  put_char(ch);
  if (ch == 0x0D) mem[CAPITAL] = 0;
}

/* ---------- messages ---------- */

typedef struct {
  const uint16_t *v; /* the arguments, in order */
  int used;
  /* For the adapters: if set, stop at the first argument code (outside
   * sub-messages) and leave its address here. */
  uint16_t *stop_at_arg;
} Args;

static uint16_t next_arg(Args *a) { return a->v ? a->v[a->used++] : 0; }

/* The end of a message: after a $3x token or code $15, '.' and a new
 * line; after a $6x token or code $14, a new line. */
static void end_message(uint8_t kind) {
  if (kind & 0x40) {
    newline();
  } else if (kind & 0x10) {
    put_char('.');
    newline();
  }
}

/* Print the message at p (codes: see text.h). */
static void run_message(uint16_t p, Args *args, bool sub) {
  for (;;) {
    uint8_t b = mem[p];
    if (b & 0x80) { /* a token; some end the message */
      uint16_t token = (uint16_t)((b & 0x7F) << 8 | mem[(uint16_t)(p + 1)]);
      uint8_t kind = b & 0x70;
      print_token(token);
      if (kind == 0x20 || kind == 0x30 || kind == 0x60) {
        end_message(kind);
        return;
      }
      p += 2;
      continue;
    }
    if (b >= 0x60) { /* a common word */
      uint16_t w = word_at((uint16_t)(COMMON_WORDS + 2 * (b - 0x60)));
      print_token((uint16_t)(w + 0x5000));
      p++;
      continue;
    }
    if (b >= 0x20) {
      print_char_cr(b);
      p++;
      continue;
    }
    if (args->stop_at_arg && !sub && (b == 0x00 || b == 0x01 || b == 0x04 || b == 0x13)) {
      *args->stop_at_arg = p;
      return;
    }
    switch (b) { /* a control code */
    case 0x00: { /* the name at the argument's address */
      mem[ARTICLE] = 0;
      uint16_t name = next_arg(args);
      if (name) print_name(name);
      break;
    }
    case 0x01: print_token(next_arg(args)); break;
    case 0x02: p += (int8_t)mem[(uint16_t)(p + 1)]; break; /* jump */
    case 0x03: print_token(word_at(TOKEN_ARG)); break;
    case 0x04:
      mem[ARTICLE] = 1;
      print_noun_token(next_arg(args));
      break;
    case 0x06:
      mem[ARTICLE] = 0;
      print_actor();
      break;
    case 0x07:
      mem[ARTICLE] = 1;
      print_object1(p);
      break;
    case 0x08: put_char(0x08); break; /* delete */
    case 0x09:
      mem[ARTICLE] = 1;
      print_object2(p);
      break;
    case 0x0B: /* a sub-message, at the offset in the next byte */
      p++;
      run_message((uint16_t)(p + (int8_t)mem[p]), args, true);
      break;
    case 0x0C: print_his_your(mem[V_ACTOR]); break;
    case 0x0D: print_char_cr(0x0D); break;
    case 0x0E: print_his_your(mem[V_OBJECT1]); break;
    case 0x10: print_is_are(mem[V_ACTOR], false); break;
    case 0x11: print_is_are(mem[V_OBJECT1], true); break;
    case 0x13: print_is_are(next_arg(args) >> 8, true); break;
    case 0x14: end_message(0x60); return;
    case 0x15: end_message(0x30); return;
    case 0x05: case 0x0A: case 0x0F: case 0x12: break; /* nothing */
    default: return; /* $16 ends; $17-$1F jumped into code in the original */
    }
    p++;
  }
}

static int message(uint16_t msg, const uint16_t *args, uint16_t *stop_at_arg) {
  if (!mem[V_DOING]) mem[V_DONE] = 0;
  Args a = {args, 0, stop_at_arg};
  run_message(msg, &a, false);
  return a.used;
}

int print_message(uint16_t msg, const uint16_t *args) { return message(msg, args, NULL); }

int print_message_quoted(uint16_t msg, const uint16_t *args) {
  if (mem[V_IN_QUOTES]) mem[V_DOING] = 0;
  return print_message(msg, args);
}

void print_cannot_do_that(void) { print_message(MSG_CANNOT_DO_THAT, NULL); }

/* ---------- describing an action ---------- */

/* The sentence is built from the action record's four words (here w0-w3)
 * and its flags: "<actor> [cannot] <w3> [<w0>|somewhere] [<w1> <w2>
 * <object 1>] [<w1>/<w2> <object 2>]." on a line of its own; an action
 * that could not be done ($B6FB clear) goes on the input line. */
void describe_action(void) {
  mem[IN_ACTION] = 1;
  mem[ARTICLE] = 0;
  bool done = mem[V_DONE] != 0;
  mem[V_PRINT_TO_INPUT] = !done;
  uint16_t record = action_record(mem[V_ACTION]);
  if (done && mem[V_ACTOR] == YOU) newline();
  uint8_t flags = load_action_flags(record);
  if (!(flags & 0x10)) {
    print_actor();
    if (!done) print_token(TOK_CANNOT);
    print_action_word(record + 6);
    uint16_t word = record;
    if (player_in_dark() && mem[V_ACTION] < 0x0B)
      print_token(TOK_SOMEWHERE); /* the player cannot see what */
    else
      print_action_word(word);
    word += 2;
    if (flags & 0x08) {
      if (flags & 0x20) print_action_word(word), word += 2;
      if (mem[ACTION_FLAGS2] & 0x80) print_action_word(word), word += 2;
      print_object1(record);
    }
    if (mem[V_OBJECT2] != OBJECT_NONE && (flags & 0x04)) {
      if (!(flags & 0x20)) print_action_word(word), word += 2;
      if (!(mem[ACTION_FLAGS2] & 0x80)) print_action_word(word), word += 2;
      print_object2(record);
    }
    put_char('.');
    newline();
  }
  mem[IN_ACTION] = 0;
}

void describe_failed_action(void) {
  mem[V_DONE] = 0;
  mem[V_DOING] = 1;
  if (!mem[V_IN_QUOTES]) describe_action();
}

/* ---------- the keyboard ---------- */

/* The idle timer counts keyboard scans down; each key gives it 500 more,
 * up to 3000 (and typing WAIT sets it to 3000). */
uint8_t input_key(uint16_t *end, uint8_t *room) {
  uint16_t timer = word_at(IDLE_TIMER);
  uint8_t key;
  while ((key = get_key()) == 0) {
    if (--timer) continue;
    /* Idle: type WAIT for the player, and ENTER. */
    InputLine line = {*end, *room};
    clear_line(&line);
    uint16_t at = line.end;
    for (int i = 0; i < 4; i++) {
      mem[at] = mem[WAIT_TEXT + i];
      put_char(mem[at++]);
    }
    *end = at;
    *room = 0x7C;
    set_word_at(IDLE_TIMER, 3000);
    return 0x0D;
  }
  set_word_at(IDLE_TIMER, timer + 500u >= 3000 ? 3000 : (uint16_t)(timer + 500));
  return key;
}

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ---------- adapters (for the faithful callers, while they remain) ---------- */

/* The registers a routine keeps, put back after the clean code. */
typedef struct {
  uint8_t a, b, c, d, e, h, l;
  uint16_t ix, iy;
} Saved;

static Saved save_regs(const Cpu *c) { return (Saved){c->a, c->b, c->c, c->d, c->e, c->h, c->l, c->ix, c->iy}; }

static void restore_regs(Cpu *c, Saved s) {
  c->a = s.a, c->b = s.b, c->c = s.c, c->d = s.d, c->e = s.e, c->h = s.h, c->l = s.l;
  c->ix = s.ix, c->iy = s.iy;
}

/* A=0 with the flags of XOR A. */
static void xor_a_flags(Cpu *c) {
  c->a = 0;
  c->sf = 0, c->zf = 1, c->hf = 0, c->pf = 1, c->nf = 0, c->cf = 0;
}

/* $70E2 Blanker: B bytes (0: 256) from HL. Leaves HL after them, B=0 and
 * A=0, Z ($786C uses B; the others nothing, but $79B5 returns them). */
static void a_blanker(Cpu *c) {
  unsigned n = c->b ? c->b : 256;
  zero_bytes(get_hl(c), n);
  set_hl(c, (uint16_t)(get_hl(c) + n));
  c->b = 0;
  xor_a_flags(c);
}

/* $70E8 IndexAction: HL = the record of action A ($7B06 pushes it). */
static void a_index_action(Cpu *c) { set_hl(c, action_record(c->a)); }

/* $70F3 ActionFlags: from the record at IX; A = the first flags (its
 * callers read them from memory, but ActionMsg used A). */
static void a_action_flags(Cpu *c) { c->a = load_action_flags(c->ix); }

/* $711A ActionMsgSaid ($7E22 jumps to it, so its caller's caller gets the
 * registers): all kept but A=0, Z. */
static void a_action_msg_said(Cpu *c) {
  Saved s = save_regs(c);
  describe_failed_action();
  restore_regs(c, s);
  xor_a_flags(c);
}

/* $712B ActionMsg: registers kept, A=0, Z. */
static void a_action_msg(Cpu *c) {
  Saved s = save_regs(c);
  describe_action();
  restore_regs(c, s);
  xor_a_flags(c);
}

/* $71D5 LocatePlace: HL = the name of location A; DE and IX kept ($7B72
 * copies from HL to DE). For a location that does not exist the original
 * gives IX + 2. */
static void a_locate_place(Cpu *c) {
  Saved s = save_regs(c);
  uint16_t name = s.a < LOCATION_COUNT ? location_name(s.a) : (uint16_t)(s.ix + 2);
  restore_regs(c, s);
  set_hl(c, name);
}

/* $71E2 LocateObject: HL = the name of object A; DE and IX kept. */
static void a_locate_object(Cpu *c) {
  Saved s = save_regs(c);
  uint16_t name = object_name(s.a);
  restore_regs(c, s);
  set_hl(c, name);
}

/* What the original's MatchWord leaves in A for words a and b: 0 for an
 * empty word, the difference of the high nibbles, or a's low byte. */
static uint8_t match_word_a(uint16_t a, uint16_t b) {
  if (word_at(a) == 0) return 0;
  uint8_t hi = (mem[(uint16_t)(a + 1)] ^ mem[(uint16_t)(b + 1)]) & 0x0F;
  return hi ? hi : mem[a];
}

/* $71F3 MatchPair: the phrase at HL against the words at IY: Z if they
 * match, with A=0 in order, 1 swapped ($7C23 tests A after Z; A is as the
 * original leaves it otherwise too). HL, DE, IY kept. */
static void a_match_pair(Cpu *c) {
  uint16_t a = get_hl(c), b = c->iy;
  bool swapped;
  bool match = phrase_matches(a, b, &swapped);
  c->zf = match;
  if (match)
    c->a = swapped;
  else if (!swapped)
    c->a = match_word_a(a, b);
  else if (!word_matches(a + 2, b + 4))
    c->a = match_word_a(a + 2, b + 4);
  else
    c->a = 1;
}

/* $7249 GetInputKey: the input line ends at HL with B room left: A = the
 * key, HL and B as they are after it; C kept (the input loop at $6DF5
 * uses A, B, C and HL). */
static void a_get_input_key(Cpu *c) {
  Saved s = save_regs(c);
  uint16_t end = get_hl(c);
  uint8_t room = c->b;
  uint8_t key = input_key(&end, &room);
  restore_regs(c, s);
  c->a = key;
  c->b = room;
  set_hl(c, end);
}

/* The message adapters. A, DE and IX are kept (the original restores
 * them from $70DC-$70E1); BC and IY are too, and callers rely on it.
 *
 * Faithful callers push the arguments of codes $00 $01 $04 $13 before
 * the CALL, and the original's handler for each pops PrintMsg's return
 * address to reach it, leaving PrintMsg's frame; the rest of the message
 * is printed from a continuation entry, which the check follows as a
 * separate routine. So a message with arguments is printed here up to the
 * first of them, and handed to the faithful continuation from there, as
 * the original does: with the registers and stack it has at that point.
 * (Clean callers pass the arguments to print_message instead.) */
static void message_adapter(Cpu *c, uint16_t msg, bool quoted) {
  if (quoted) {
    /* PrintMsgQ tests the quote flag in A, and clears A inside quotes,
     * before PrintMsg keeps A: so it gives back A = 0 either way. */
    if (mem[V_IN_QUOTES]) mem[V_DOING] = 0;
    c->a = 0;
  }
  Saved s = save_regs(c);
  set_word_at(0x70DD, get_de(c));
  set_word_at(0x70E0, c->ix);
  mem[0x70DC] = c->a;
  uint16_t at = 0;
  message(msg, NULL, &at);
  restore_regs(c, s);
  if (!at) return;
  static const uint16_t continuation[0x14] = {[0x00] = 0x736D, [0x01] = 0x7378, [0x04] = 0x7396, [0x13] = 0x742F};
  uint8_t code = mem[at];
  if (code == 0x00) mem[ARTICLE] = 0;
  c->a = code == 0x00 ? 0 : code;
  c->ix = at;
  set_de(c, 0x733B); /* the handler's return address, into the loop */
  set_hl(c, pop16(c));
  cpu_tail(c, continuation[code]);
}

/* $72DD PrintMsg: the message at HL. */
static void a_print_msg(Cpu *c) { message_adapter(c, get_hl(c), false); }

/* $72D3 PrintMsgQ. */
static void a_print_msg_quoted(Cpu *c) { message_adapter(c, get_hl(c), true); }

/* $72CE ICannotDoThat (PrintMsg with HL set; HL is left so). */
static void a_cannot_do_that(Cpu *c) {
  set_hl(c, MSG_CANNOT_DO_THAT);
  message_adapter(c, MSG_CANNOT_DO_THAT, false);
}

/* $743F PrintArticle (from $9EE5): before noun token DE. BC, IX, IY kept;
 * the caller reloads the rest. */
static void a_print_article(Cpu *c) {
  Saved s = save_regs(c);
  print_article(get_de(c));
  restore_regs(c, s);
}

/* $7478 PrintNounTok (from $9F02): noun token DE. BC, IX, IY kept. */
static void a_print_noun_token(Cpu *c) {
  Saved s = save_regs(c);
  print_noun_token(get_de(c));
  restore_regs(c, s);
}

/* $74C1 PrintToken: token DE. BC, DE, HL (and IX, IY) kept. */
static void a_print_token(Cpu *c) {
  Saved s = save_regs(c);
  print_token(get_de(c));
  restore_regs(c, s);
}

#define KEPT (OUT_BC | OUT_IX | OUT_IY)

const CleanRoutine text_clean[] = {
    {0x70E2, "zero_bytes", a_blanker, OUT_A | OUT_B | OUT_HL | OUT_ZF | OUT_CF},
    {0x70E8, "action_record", a_index_action, OUT_HL},
    {0x70F3, "load_action_flags", a_action_flags, OUT_A},
    {0x711A, "describe_failed_action", a_action_msg_said, OUT_REGS | OUT_ZF | OUT_CF},
    {0x712B, "describe_action", a_action_msg, OUT_REGS | OUT_ZF | OUT_CF},
    {0x71D5, "location_name", a_locate_place, OUT_HL | OUT_DE | OUT_IX},
    {0x71E2, "object_name", a_locate_object, OUT_HL | OUT_DE | OUT_IX},
    {0x71F3, "phrase_matches", a_match_pair, OUT_A | OUT_ZF | OUT_HL | OUT_DE | OUT_IY},
    {0x7249, "input_key", a_get_input_key, OUT_A | OUT_BC | OUT_HL},
    {0x72CE, "print_cannot_do_that", a_cannot_do_that, OUT_A | OUT_DE | KEPT},
    {0x72D3, "print_message_quoted", a_print_msg_quoted, OUT_A | OUT_DE | KEPT},
    {0x72DD, "print_message", a_print_msg, OUT_A | OUT_DE | KEPT},
    {0x743F, "print_article", a_print_article, KEPT},
    {0x7478, "print_noun_token", a_print_noun_token, KEPT},
    {0x74C1, "print_token", a_print_token, OUT_DE | OUT_HL | KEPT},
    {0, NULL, NULL, 0},
};

const CleanScratch text_scratch[] = {
    {0x70DC, 0x70DE, "PrintMsg's save of A and DE: read back only by PrintMsg (the adapters keep the registers)"},
    {0x70E0, 0x70E1, "PrintMsg's save of IX: read back only by PrintMsg"},
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
