/* The clean edition: the executor. See docs/CLEAN.md.
 *
 * The parser leaves its sentences as records of $18 bytes, from $B9C8 going
 * down, PHRASE_COUNT of them. A record holds the verb (a dictionary word;
 * bit 15 means ALL, and bit 14 marks a record that continues the one above
 * it: the EXCEPT list of an ALL), two words of prepositions at +4 and +6,
 * two more at +$0E and +$10, and two noun phrases of 6 bytes (adjective,
 * adjective, noun) at +8 and +$12.
 *
 * For each sentence the executor finds the verb in the action table
 * ($AB53, 8 bytes an entry), decides which noun phrase is the direct
 * object, and looks for objects (or places) that fit the phrases and that
 * the action can be tried on. Then it carries the action out, with the
 * game turn after it; or it says why not, or asks "which ...?" and keeps
 * the record at $B9C8 with V_QUESTION set, so that the next line is read
 * as the answer.
 *
 * Characters are given orders by quoted sentences: these are kept in the
 * speech slots ($B738, 8 of $19 bytes: the character, then the record),
 * and a character's turn carries them out (obey_orders).
 *
 * The executor's own variables are kept where the original kept them, at
 * $793D-$795F (ExecVars below): they last from one call to the next (the
 * search through the objects for an ALL goes on after the turn). */
#include "executor.h"

#include "characters.h"
#include "objects.h"
#include "text.h"

#include <stdarg.h>
#include <stddef.h>

/* ---------- data ---------- */

#define RECORDS 0xB9C8      /* the first sentence record; the rest go down */
#define RECORD_SIZE 0x18
#define PHRASE_COUNT 0xB706 /* records left */
#define MORE_SENTENCES 0xB705 /* cleared when a sentence stops the line */
#define ACTION_TABLE 0xAB53 /* action 1; 8 bytes an entry; a zero word ends it */
#define ACTION_COPY 0xB6E6  /* a copy of V_ACTION */
#define IT_PHRASE 0xB6E0    /* the noun phrase IT refers to */
#define VERB_MATCHED 0xB6DF /* set by $71F3 when a verb matched but not its words */
#define IS_PLACE1 0xB6FE    /* 1: V_OBJECT1 is a place (a location), not an object */
#define ANY_VISIBILITY 0xB70F /* nonzero: $9DD9 also takes objects the actor cannot see */
#define MATCH_KIND 0xB710   /* $9DD9: 0 objects, 1 characters, 2 either */
#define VAR_B711 0xB711     /* from the action's flags, for $950F */
#define ALL_MODE 0xB71C     /* repeating the action for each object of ALL */
#define ACTION_FLAGS1 0xB71D /* from the action table entry ($70F3) */
#define ACTION_FLAGS2 0xB71E
#define SPEECH_COUNT 0xB737 /* orders in the last quoted sentence */
#define SPEECH_SLOT_SIZE 0x19
#define SPEECH_SLOTS_N 8
#define OBJECT_LIST 0xC060  /* the object index, from 3 bytes before its start */

enum { RECORD_ALL = 0x80, RECORD_CONTINUES = 0x40 }; /* bits of a record's byte 1 */

/* Action flags. */
enum {
  F1_PLACE1 = 0x80,  /* the first object is a place */
  F1_PLACE2 = 0x40,  /* the second object is a place */
  F1_DIRECT = 0x20,  /* which noun phrase is the direct object */
  F1_OBJECTS = 0x0C, /* the action takes objects */
  F1_TWO = 0x04,     /* the action takes a second object */
  F1_TWO_OPTIONAL = 0x02,
  F1_ANY_VISIBILITY = 0x01,
  F2_B711 = 0x40,
};

/* The executor's variables, $793D-$795F. Index 0 is the first noun phrase
 * (the direct object), 1 the second. */
typedef struct __attribute__((packed)) {
  uint8_t given[2];     /* bit 0: the noun phrase was given; bit 1: something fitted it */
  uint8_t tries;        /* objects tried for the first noun phrase */
  uint8_t count[2];     /* objects that fitted */
  uint8_t phrase[2][6]; /* the noun phrases */
  uint16_t list[2];     /* where in the object index (or exits) the search is */
  uint8_t all;          /* bit 7 of the verb's record: ALL */
  uint8_t one_enough;   /* 1: the first object that will do is taken */
  uint8_t field[2];     /* the record offset ($08 or $12) of each noun phrase */
  uint8_t last[2];      /* the last object that fitted */
  uint16_t verb;        /* the verb (dictionary word) */
  uint16_t prep[2];     /* the sentence's first two prepositions */
  uint16_t entry;       /* the action table entry found */
} ExecVars;
#define EXEC_VARS 0x793D
#define EXEC_VARS_CLEARED 0x11 /* given..phrase: cleared for each sentence */

static ExecVars *ev(void) { return (ExecVars *)&mem[EXEC_VARS]; }

/* Messages. */
#define MSG_DONT_UNDERSTAND 0xADA3 /* the verb */
#define MSG_CANT_VERB 0xADB0       /* the verb and its two words */
#define MSG_WHICH 0xADCC           /* "which ...?" */
#define MSG_CANT_SEE 0xADD3        /* the noun phrase */
#define MSG_WHAT 0xADE1            /* no direct object given */
#define MSG_WHAT_KEPT 0xADC4       /* the same, as a question to be answered */
#define MSG_WHAT2 0xADE7           /* no second object given */
#define MSG_WHAT2_KEPT 0xADC0

/* What finds the next thing a noun phrase can mean: an object ($9DD9),
 * or a place, among the exits ($9EA0). */
typedef enum { MATCH_OBJECT, MATCH_PLACE } Matcher;

/* ---------- other modules ---------- */

/* From *at (moved on), the next object or place that fits the phrase at
 * phrase: its id, $FF if none. */
static uint8_t match_next(Matcher m, uint16_t phrase, uint16_t *at) {
  return m == MATCH_OBJECT ? match_object(at, phrase) : match_exit(at, phrase);
}

/* Zero n bytes from addr (0: 256, as the original's counter in B). */
static void blank_bytes(uint16_t addr, uint8_t n) { zero_bytes(addr, n ? n : 256); }

/* The 6-byte name of an object, or of a place. */
static uint16_t name_of(uint8_t id, bool place) { return place ? location_name(id) : object_name(id); }

/* Does the action table entry have the verb (and the prepositions) in
 * ExecVars? *swapped: the prepositions match the other way round. */
static bool entry_matches(uint16_t entry, bool *swapped) {
  return phrase_matches((uint16_t)(EXEC_VARS + offsetof(ExecVars, verb)), entry, swapped);
}

/* Try (V_DOING 0) or do (1) the action; true if it could be. */
static bool perform_action(void) {
  do_action();
  return mem[V_DONE] != 0;
}

/* V_DONE: whether the action may be tried on V_OBJECT1. */
static uint8_t may_try(void) {
  may_try_action();
  return mem[V_DONE];
}

/* A message ($72D3: as said inside quotes, only tried), with n parameter
 * words in the order it uses them. */
static void say_message(uint16_t msg, int n, ...) {
  uint16_t args[8];
  va_list ap;
  va_start(ap, n);
  for (int i = 0; i < n; i++) args[i] = (uint16_t)va_arg(ap, int);
  va_end(ap);
  print_message_quoted(msg, args);
}

/* ---------- small routines ---------- */

/* $79A9: clear the variables of a sentence. */
static void clear_vars(void) {
  mem[V_PRINT_TO_INPUT] = 0;
  blank_bytes(EXEC_VARS, EXEC_VARS_CLEARED);
}

/* $7D6B: output on, to the text window. */
static void output_on(void) {
  mem[V_DOING] = 1;
  mem[V_PRINT_TO_INPUT] = 1;
}

/* $7ACC: where to search the object index: from 3 bytes before it if
 * kind (bits 0-1 of an action's flags) is nonzero. */
static uint16_t object_list(uint8_t kind) { return kind & 3 ? OBJECT_LIST : OBJECT_INDEX; }

/* $7AA6 (n 0), $7ABA (n 1): start the search for objects for noun
 * phrase n: in the object index, or the exits of the actor's location if
 * it names a place. */
static void start_search(int n) {
  uint8_t flags2 = mem[ACTION_FLAGS2];
  uint16_t list = object_list(n == 0 ? flags2 >> 2 : flags2);
  if (mem[IS_PLACE1 + n]) list = actor_exits();
  ev()->list[n] = list;
}

/* $7AA1: start the search for the first noun phrase, unless going on
 * with an ALL. */
static void start_search_once(void) {
  if (!mem[ALL_MODE]) start_search(0);
}

/* $7AD8: should a second object be looked for: the action takes one, and
 * it was given or is not optional. */
static bool wants_object2(void) {
  uint8_t flags1 = mem[ACTION_FLAGS1];
  if (!(flags1 & F1_TWO)) return false;
  if (ev()->given[1] & 1) return true;
  return !(flags1 & F1_TWO_OPTIONAL);
}

/* $7AED: try the action; true if it could be done. */
static bool try_action(void) { return perform_action(); }

/* $7B63: unless id is none, copy the name of object (or place) id to
 * dest. */
static void copy_name(uint8_t id, bool place, uint16_t dest) {
  if (id == OBJECT_NONE) return;
  uint16_t name = name_of(id, place);
  for (int i = 0; i < 6; i++) mem[dest + i] = mem[name + i];
}

/* $7B78: decode the action's flags. */
static void decode_flags(void) {
  uint8_t flags1 = mem[ACTION_FLAGS1];
  mem[VAR_B711] = mem[ACTION_FLAGS2] & F2_B711;
  mem[ANY_VISIBILITY] = flags1 & F1_ANY_VISIBILITY;
  mem[IS_PLACE1] = (flags1 & F1_PLACE1) != 0;
  mem[IS_PLACE1 + 1] = (flags1 & F1_PLACE2) != 0;
}

/* $7C91: copy the noun phrase at src to noun phrase n; mark it given if it
 * is not empty. */
static void copy_phrase(uint16_t src, int n) {
  uint8_t any = 0;
  for (int i = 0; i < 6; i++) any |= ev()->phrase[n][i] = mem[(uint16_t)(src + i)];
  if (any) ev()->given[n] |= 1;
}

/* $7D74: keep the record of a sentence at $B9C8, with V_QUESTION = what,
 * for the answer to the question about to be asked to complete. */
static void keep_question(uint8_t what, uint16_t record) {
  mem[V_QUESTION] = what;
  for (int i = 0; i < RECORD_SIZE; i++) mem[(uint16_t)(RECORDS + i)] = mem[(uint16_t)(record + i)];
}

/* ---------- finding the action ---------- */

/* $7CAC: the step of collect_preps: if there is room, store the word at
 * record+offset in *dest, moving on if it is not empty. */
static void collect_word(uint16_t record, uint8_t offset, uint16_t *dest, uint8_t *room) {
  if (*room == 0) return;
  uint16_t w = word_at((uint16_t)(record + offset));
  set_word_at(*dest, w);
  if (w == 0) return;
  --*room;
  *dest += 2;
}

/* The first two prepositions of the record (+4, +$0E, +6, +$10 in turn). */
static void collect_preps(uint16_t record) {
  static const uint8_t offsets[] = {0x04, 0x0E, 0x06, 0x10};
  uint16_t dest = EXEC_VARS + offsetof(ExecVars, prep);
  uint8_t room = 2;
  ev()->prep[0] = ev()->prep[1] = 0;
  for (int i = 0; i < 4; i++) collect_word(record, offsets[i], &dest, &room);
}

/* $7C23: having found the action's entry for the record: swap the
 * prepositions if they matched the other way round, take the action's
 * flags, decide which noun phrase is the direct object (by the
 * prepositions and the flags), and take both phrases (the first is also
 * what IT means from now on). */
static void take_phrases(uint16_t record, uint16_t entry, bool swapped) {
  ExecVars *v = ev();
  if (swapped) {
    uint16_t t = v->prep[0];
    v->prep[0] = v->prep[1];
    v->prep[1] = t;
  }
  load_action_flags(entry);
  uint8_t flags;
  if (v->prep[0] == 0) {
    flags = mem[ACTION_FLAGS2] ^ F1_DIRECT;
  } else {
    bool in_record = v->prep[0] == word_at((uint16_t)(record + 0x0E)) ||
                     v->prep[0] == word_at((uint16_t)(record + 0x10));
    flags = mem[ACTION_FLAGS1] ^ (in_record ? F1_DIRECT : 0);
  }
  v->field[0] = flags & F1_DIRECT ? 0x08 : 0x12;
  v->field[1] = flags & F1_DIRECT ? 0x12 : 0x08;
  copy_phrase((uint16_t)(record + v->field[0]), 0);
  for (int i = 0; i < 6; i++) mem[IT_PHRASE + i] = v->phrase[0][i];
  copy_phrase((uint16_t)(record + v->field[1]), 1);
}

enum Found { NO_ACTION, ACTION_FOUND, NO_ACTION_LEAVE };

/* $7B9E: find the action for the record's verb (and prepositions) in the
 * action table, and take its phrases. If there is none, say so: "you
 * cannot <verb> <words>" when the verb is known with other words, else "i
 * don't understand"; outside a quoted command the latter leaves the
 * sentence and Execute goes on to the end of the turn (NO_ACTION_LEAVE). */
static enum Found find_action(uint16_t record, uint16_t *entry) {
  ExecVars *v = ev();
  uint16_t verb = word_at(record);
  v->all = (verb >> 8) & RECORD_ALL;
  v->verb = verb & 0x7FFF;
  collect_preps(record);
  mem[VERB_MATCHED] = 0;
  for (uint16_t e = ACTION_TABLE;; e += 8) {
    bool swapped;
    if (entry_matches(e, &swapped)) {
      take_phrases(record, e, swapped);
      *entry = e;
      return ACTION_FOUND;
    }
    if (word_at((uint16_t)(e + 8)) == 0) break;
  }
  if (mem[VERB_MATCHED]) {
    output_on();
    say_message(MSG_CANT_VERB, 3, v->verb, v->prep[0], v->prep[1]);
    return NO_ACTION;
  }
  mem[V_PRINT_TO_INPUT] = 0;
  mem[ALL_MODE] = 0;
  mem[V_DOING] = 1;
  say_message(MSG_DONT_UNDERSTAND, 1, v->verb);
  return mem[V_IN_QUOTES] == 1 ? NO_ACTION : NO_ACTION_LEAVE;
}

/* $7E78 (when_set false), $7E7C (true): the action's own words at +4 and
 * +2 of its entry, each kept if its flag (bit 7 of byte 7, bit 5 of byte
 * 3) is set (when_set) or clear (not), else 0: the parameters of the
 * "what ...?" messages. */
typedef struct {
  uint16_t w2, w4;
} EntryWords;

static EntryWords entry_words(bool when_set) {
  uint16_t entry = ev()->entry;
  EntryWords w;
  w.w4 = ((mem[(uint16_t)(entry + 7)] & 0x80) != 0) == when_set ? word_at((uint16_t)(entry + 4)) : 0;
  w.w2 = ((mem[(uint16_t)(entry + 3)] & 0x20) != 0) == when_set ? word_at((uint16_t)(entry + 2)) : 0;
  return w;
}

/* ---------- matching the noun phrases ---------- */

/* $7CFC (n 0), $7D54 (n 1): from *ix, the next object (or place, by
 * matcher) that fits noun phrase n and that the action can be tried on
 * (for the first; for the second, that it can be done with): it is put
 * in V_OBJECT1+n. Returns $FF if there is none, else V_DONE. */
static uint8_t next_object(int n, Matcher matcher, uint16_t *ix) {
  uint16_t phrase = (uint16_t)(EXEC_VARS + offsetof(ExecVars, phrase) + 6 * n);
  for (;;) {
    /* $7CC9: JP (IY), the matcher. */
    uint8_t id = match_next(matcher, phrase, ix);
    if (id == 0xFF) return id;
    mem[V_OBJECT1 + n] = id;
    ev()->given[n] |= 2;
    uint8_t done = n == 0 ? may_try() : (uint8_t)(try_action() ? mem[V_DONE] : 0);
    if (done) return done;
  }
}

/* $7CCB (n 0), $7D17 (n 1): go on with the search for noun phrase n;
 * true if an object was found. */
static bool find_object(int n) {
  ExecVars *v = ev();
  Matcher matcher = MATCH_PLACE;
  if (n == 1) mem[ANY_VISIBILITY] = 0;
  uint16_t ix = v->list[n];
  if (mem[IS_PLACE1 + n] != 1) {
    matcher = MATCH_OBJECT;
    uint8_t flags2 = mem[ACTION_FLAGS2];
    mem[MATCH_KIND] = (n == 0 ? flags2 >> 2 : flags2) & 3;
  }
  uint8_t r = next_object(n, matcher, &ix);
  ev()->list[n] = ix;
  if (n == 1) mem[ANY_VISIBILITY] = mem[ACTION_FLAGS1] & F1_ANY_VISIBILITY;
  return r != 0xFF;
}

/* $7A14: find objects for the noun phrases. True when the action is
 * settled: done (or, with one_enough, an object found that will do), or
 * exactly one object fitted each phrase that was wanted (left in
 * V_OBJECT1/2). False when it could not be: nothing, or more than one,
 * fitted. */
static bool match_objects(void) {
  ExecVars *v = ev();
  for (;;) {
    bool found;
    if (!find_object(0)) {
      if (v->count[0] != 1) return false;
      mem[V_OBJECT1] = v->last[0];
      start_search(1);
      if (!wants_object2()) return true;
      while (find_object(1)) {
        v->last[1] = mem[V_OBJECT2];
        v->count[1]++;
      }
      if (v->count[1] != 1) return false;
      mem[V_OBJECT2] = v->last[1];
      return true;
    }
    v->tries++;
    if (wants_object2()) {
      start_search(1);
      found = find_object(1);
    } else {
      found = try_action();
    }
    if (!found) continue;
    if (v->one_enough == 1) return true;
    v->last[0] = mem[V_OBJECT1];
    v->count[0]++;
  }
}

/* $7A73: for an ALL: is V_OBJECT1 named by one of the records continuing
 * the one at record (its EXCEPT list)? */
static bool is_excepted(uint16_t record) {
  for (uint16_t r = (uint16_t)(record - RECORD_SIZE); mem[(uint16_t)(r + 1)] & RECORD_CONTINUES;
       r = (uint16_t)(r - RECORD_SIZE)) {
    uint16_t ix = OBJECT_LIST;
    for (;;) {
      uint8_t id = match_next(MATCH_OBJECT, (uint16_t)(r + 8), &ix);
      if (id == 0xFF) break;
      if (id == mem[V_OBJECT1]) return true;
    }
  }
  return false;
}

/* ---------- carrying out a sentence ---------- */

/* How a sentence ended. The last three are where the original left
 * RunSentence through its caller's frame (by $7C16, $7DC7, $7DF5): which
 * point of Execute it goes on at. */
enum Outcome {
  STOPPED,      /* with a message or a question (Z) */
  DONE,         /* ready to be carried out, or settled for an ALL (NZ) */
  LEAVE_TURN,   /* "i don't understand": straight to the turn ($7C16) */
  LEAVE_NEXT,   /* an ALL ran out: on to the next sentence ($7DC7) */
  LEAVE_FAILED, /* the action was tried and failed; no turn ($7DF5) */
};

/* $7DF5: the action fails: done for real, to say why. */
static enum Outcome fail_action(void) {
  output_on();
  perform_action();
  return LEAVE_FAILED;
}

/* $7D83: ask "which <word>?" about the sentence at record. */
static enum Outcome ask_which(uint8_t what, uint16_t word, uint16_t record) {
  keep_question(what, record);
  output_on();
  say_message(MSG_WHICH, 1, word);
  return STOPPED;
}

/* $7DFE: nothing nearby fitted noun phrase n: look for a place, then any
 * object, it could mean. If there is one, it is the object, and the
 * action is described as done, which it is not (except in quotes); else
 * "you can't see <phrase>". */
static enum Outcome look_anywhere(int n) {
  uint16_t phrase = (uint16_t)(EXEC_VARS + offsetof(ExecVars, phrase) + 6 * n);
  uint16_t ix = actor_exits();
  mem[IS_PLACE1 + n] = 1;
  uint8_t id = match_next(MATCH_PLACE, phrase, &ix);
  if (id == 0xFF) {
    mem[MATCH_KIND] = 2;
    ix = OBJECT_LIST;
    mem[IS_PLACE1 + n] = 0;
    id = match_next(MATCH_OBJECT, phrase, &ix);
    if (id == 0xFF) {
      output_on();
      say_message(MSG_CANT_SEE, 1, phrase);
      return STOPPED;
    }
  }
  mem[V_OBJECT1 + n] = id;
  describe_failed_action();
  return STOPPED;
}

/* $7E30: no direct object was given: "what do you want to <verb> ...?",
 * kept as a question if something could have fitted. */
static enum Outcome ask_object1(uint16_t record) {
  ExecVars *v = ev();
  EntryWords w = entry_words(true);
  if (v->count[0] == 0) {
    output_on();
    say_message(MSG_WHAT, 3, v->verb, w.w2, w.w4);
    return STOPPED;
  }
  keep_question(v->field[0], record);
  output_on();
  say_message(MSG_WHAT_KEPT, 3, v->verb, w.w2, w.w4);
  return STOPPED;
}

/* $7E4D: no second object was given: the same, with the first object. */
static enum Outcome ask_object2(uint16_t record) {
  ExecVars *v = ev();
  EntryWords clear = entry_words(false);
  uint16_t name = name_of(mem[V_OBJECT1], false);
  EntryWords set = entry_words(true);
  uint16_t msg = MSG_WHAT2;
  if (v->count[1] != 0) {
    keep_question(v->field[1], record);
    msg = MSG_WHAT2_KEPT;
  }
  output_on();
  say_message(msg, 7, v->verb, set.w2, set.w4, name, 0, clear.w2, clear.w4);
  return STOPPED;
}

/* $7DBC: the objects did not work out: say why, or ask which. */
static enum Outcome explain(uint16_t record) {
  ExecVars *v = ev();
  if (mem[V_IN_QUOTES] == 1) return STOPPED;
  if (mem[ALL_MODE]) return LEAVE_NEXT;
  if (v->count[0] != 1) {
    if (!(v->given[0] & 1)) return ask_object1(record);
    if (!(v->given[0] & 2)) return look_anywhere(0);
    if (v->tries == 0) return fail_action();
    if (v->tries != 1) return ask_which(v->field[0], word_at(EXEC_VARS + offsetof(ExecVars, phrase)), record);
    if (!wants_object2()) return fail_action();
  }
  if (!(v->given[1] & 1)) return ask_object2(record);
  if (!(v->given[1] & 2)) return look_anywhere(1);
  if (v->count[1] == 0) return fail_action();
  /* About the second noun phrase, as about the first. (The original's bug
   * asks with the first byte of the phrase, and the address of the phrase
   * rather than the word in it: "which ting ?".) */
  uint16_t phrase2 = EXEC_VARS + offsetof(ExecVars, phrase) + 6;
  if (original_bugs) return ask_which(v->phrase[1][0], phrase2, record);
  return ask_which(v->field[1], word_at(phrase2), record);
}

/* $79B6: make ready the sentence at record: find its action and objects
 * (V_ACTION, V_OBJECT1/2), to be carried out by the caller. For an ALL,
 * one object at a time: ALL_MODE stays set while there may be more. */
static enum Outcome run_sentence(uint16_t record) {
  ExecVars *v = ev();
  uint16_t entry;
  mem[V_OBJECT2] = mem[V_OBJECT1] = OBJECT_NONE;
  clear_vars();
  switch (find_action(record, &entry)) {
  case NO_ACTION: return STOPPED;
  case NO_ACTION_LEAVE: return LEAVE_TURN;
  case ACTION_FOUND: break;
  }
  uint8_t action = (uint8_t)(1 + (entry - ACTION_TABLE) / 8);
  mem[V_ACTION] = mem[ACTION_COPY] = action;
  v->entry = entry;
  decode_flags();
  start_search_once();
  mem[V_DOING] = 0;
  if (!(mem[ACTION_FLAGS1] & F1_OBJECTS)) return DONE;
  mem[ALL_MODE] = v->all;
  v->one_enough = v->all >> 7;
  do {
    if (!match_objects()) return explain(record);
    if (!mem[ALL_MODE]) return DONE;
  } while (is_excepted(record));
  return DONE;
}

/* Where Execute goes on (the continuation points of the original). */
enum ExecPoint { AT_RUN, AT_TURN, AT_ALL, AT_NEXT };

/* $7960 from the point given, with the sentence at record: carry out each
 * sentence in turn. *left_7973 is set when the original would have left a
 * return address ($7973) on the stack (by LEAVE_FAILED): returning to it
 * at the end clears MORE_SENTENCES. */
static void execute_from(enum ExecPoint at, uint16_t record, bool *left_7973) {
  for (;;) {
    switch (at) {
    case AT_RUN:
      switch (run_sentence(record)) {
      case STOPPED:
        mem[MORE_SENTENCES] = 0;
        return;
      case LEAVE_TURN: at = AT_TURN; continue;
      case LEAVE_NEXT: at = AT_NEXT; continue;
      case LEAVE_FAILED:
        *left_7973 = true;
        at = AT_ALL;
        continue;
      case DONE: break;
      }
      if (!try_action()) { /* it cannot be done: do it, to say why ($7DF5) */
        output_on();
        perform_action();
        at = AT_ALL;
        continue;
      }
      mem[V_DOING] = 1;
      describe_action();
      perform_action();
      /* fall through */
    case AT_TURN:
      end_of_turn();
      /* fall through */
    case AT_ALL:
      if (mem[ALL_MODE]) {
        at = AT_RUN;
        continue;
      }
      /* fall through */
    case AT_NEXT:
      if (--mem[PHRASE_COUNT] == 0) return;
      do record = (uint16_t)(record - RECORD_SIZE);
      while (mem[(uint16_t)(record + 1)] & RECORD_CONTINUES);
      at = AT_RUN;
      continue;
    }
  }
}

/* Execute from a point, then what returning to $7973 does. */
static void execute_at(enum ExecPoint at, uint16_t record) {
  bool left_7973 = false;
  execute_from(at, record, &left_7973);
  if (left_7973) mem[MORE_SENTENCES] = 0;
}

void execute_sentences(void) {
  mem[ALL_MODE] = 0;
  bool answer = mem[V_QUESTION] != 0; /* the first record is the question's: skip it */
  mem[V_QUESTION] = 0;
  execute_at(answer ? AT_NEXT : AT_RUN, RECORDS);
}

/* ---------- called from the game code ---------- */

bool retry_action(void) {
  ExecVars *v = ev();
  uint16_t list = v->list[0];
  uint16_t entry = action_record(mem[V_ACTION]);
  bool done;
  clear_vars();
  load_action_flags(entry);
  decode_flags();
  copy_name(mem[V_OBJECT1], mem[IS_PLACE1], EXEC_VARS + offsetof(ExecVars, phrase));
  copy_name(mem[V_OBJECT2], mem[IS_PLACE1 + 1], EXEC_VARS + offsetof(ExecVars, phrase) + 6);
  start_search(0);
  mem[V_DOING] = 0;
  if (mem[ACTION_FLAGS1] & F1_OBJECTS) {
    v->one_enough = 1;
    done = match_objects();
  } else {
    done = try_action();
  }
  mem[V_DOING] = 1;
  v->list[0] = list;
  return done;
}

/* The next slot after slot (from SPEECH_SLOTS - $19) holding $FF: an order
 * just given, not yet assigned. */
static uint16_t next_new_order(uint16_t slot) {
  do slot = (uint16_t)(slot + SPEECH_SLOT_SIZE);
  while (mem[slot] != 0xFF);
  return slot;
}

void give_orders(uint8_t n) {
  uint8_t count = mem[SPEECH_COUNT];
  uint8_t given = count < n ? count : n;
  uint16_t slot = SPEECH_SLOTS - SPEECH_SLOT_SIZE;
  for (int i = 0; i < given; i++) {
    slot = next_new_order(slot);
    mem[slot] = mem[V_OBJECT1];
  }
  for (int i = 0; i < count - given; i++) {
    slot = next_new_order(slot);
    mem[slot] = 0;
  }
}

uint16_t find_orders(bool *found) {
  uint16_t slot = SPEECH_SLOTS;
  for (int i = 0; i < SPEECH_SLOTS_N; i++, slot += SPEECH_SLOT_SIZE)
    if (mem[slot] == mem[V_ACTOR]) {
      *found = true;
      return slot;
    }
  *found = false;
  return slot;
}

bool has_orders(void) {
  bool found;
  find_orders(&found);
  return found;
}

bool obey_orders(bool run, uint16_t *record) {
  bool found;
  uint16_t slot = find_orders(&found);
  mem[slot] = 0; /* (with no order, the byte after the slots) */
  *record = (uint16_t)(slot + 1);
  if (!run) return true;
  mem[V_IN_QUOTES] = 1;
  uint8_t all_mode = mem[ALL_MODE];
  /* In quotes, the sentence never leaves through Execute's frame. */
  enum Outcome r = run_sentence(*record);
  mem[V_IN_QUOTES] = 0;
  mem[ALL_MODE] = all_mode;
  if (r == DONE && try_action()) return true;
  drop_orders(mem[V_ACTOR]);
  return false;
}

void drop_orders(uint8_t who) {
  for (int i = 0; i < SPEECH_SLOTS_N; i++) {
    uint16_t slot = (uint16_t)(SPEECH_SLOTS + i * SPEECH_SLOT_SIZE);
    if (mem[slot] == who) mem[slot] = 0;
  }
}

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ---------- adapters (for the faithful callers, while they remain) ---------- */

/* Each adapter keeps the registers it does not set. */
typedef struct {
  Cpu regs;
} Saved;

static void enter(Cpu *c, Saved *s) { s->regs = *c; }

static void leave(Cpu *c, const Saved *s) {
  const Cpu *r = &s->regs;
  c->a = r->a, c->b = r->b, c->c = r->c, c->d = r->d, c->e = r->e, c->h = r->h, c->l = r->l;
  c->ix = r->ix, c->iy = r->iy;
  c->sf = r->sf, c->zf = r->zf, c->hf = r->hf, c->pf = r->pf, c->nf = r->nf, c->cf = r->cf;
  c->yf = r->yf, c->xf = r->xf;
}

#define ADAPT(c, body)    \
  do {                    \
    Saved saved_;         \
    enter((c), &saved_);  \
    body;                 \
    leave((c), &saved_);  \
  } while (0)

/* $7960 Execute: the caller ($6D96) reads MORE_SENTENCES; A=0, Z as the
 * original leaves them. */
static void a_execute(Cpu *c) {
  ADAPT(c, execute_sentences());
  c->a = 0, c->zf = true;
}

/* $798E, $7DC8, $7C18: Execute going on from where the original's
 * RunSentence left through its frame (only from the faithful RunSentence),
 * with the sentence at IY; then RET as Execute's. */
static void a_execute_798e(Cpu *c) {
  ADAPT(c, execute_at(AT_ALL, c->iy));
  c->a = 0, c->zf = true;
}
static void a_execute_7dc8(Cpu *c) {
  ADAPT(c, execute_at(AT_NEXT, c->iy));
  c->a = 0, c->zf = true;
}
static void a_execute_7c18(Cpu *c) {
  ADAPT(c, execute_at(AT_TURN, c->iy));
  c->a = 0, c->zf = true;
}
/* $7DC7, $7C16, $7C17: POP HL each, on to the next. */
static void a_pop_7dc7(Cpu *c) {
  set_hl(c, pop16(c));
  cpu_tail(c, 0x7DC8);
}
static void a_pop_7c16(Cpu *c) {
  set_hl(c, pop16(c));
  cpu_tail(c, 0x7C17);
}
static void a_pop_7c17(Cpu *c) {
  set_hl(c, pop16(c));
  cpu_tail(c, 0x7C18);
}

/* $79A9: no outputs used. */
static void a_clear_vars(Cpu *c) { ADAPT(c, clear_vars()); }

/* $79B6 RunSentence, record IY: Z when stopped (Obey uses it). When the
 * original would leave through its caller's frame, the faithful
 * continuation is jumped to, with the stack as the original's there. */
static void a_run_sentence(Cpu *c) {
  enum Outcome r;
  ADAPT(c, r = run_sentence(c->iy));
  c->zf = r == STOPPED;
  switch (r) {
  case LEAVE_TURN:
    push16(c, 0x79C4); /* the return address of CALL $7B9E */
    cpu_tail(c, 0x7C16);
    break;
  case LEAVE_NEXT: cpu_tail(c, 0x7DC7); break;
  case LEAVE_FAILED: cpu_tail(c, 0x798E); break;
  default: break;
  }
}

/* $7A14: Z when settled. */
static void a_match_objects(Cpu *c) {
  bool settled;
  ADAPT(c, settled = match_objects());
  c->zf = settled;
}

/* $7A73, record IY: NZ if excepted. */
static void a_is_excepted(Cpu *c) {
  bool r;
  ADAPT(c, r = is_excepted(c->iy));
  c->zf = !r;
}

/* $7AA1, $7AA6, $7ABA: no outputs used. */
static void a_start_search_once(Cpu *c) { ADAPT(c, start_search_once()); }
static void a_start_search1(Cpu *c) { ADAPT(c, start_search(0)); }
static void a_start_search2(Cpu *c) { ADAPT(c, start_search(1)); }

/* $7ACC: A the flags; IX the list. */
static void a_object_list(Cpu *c) {
  c->ix = object_list(c->a);
  c->a &= 3;
  c->zf = c->a == 0;
}

/* $7AD8: NZ if a second object is wanted. */
static void a_wants_object2(Cpu *c) {
  bool r;
  ADAPT(c, r = wants_object2());
  c->zf = !r;
}

/* $7AED: NZ if done, A = V_DONE. */
static void a_try_action(Cpu *c) {
  ADAPT(c, try_action());
  c->a = mem[V_DONE];
  c->zf = c->a == 0;
}

/* $7AF5 RetryAction (from $99C8): NZ if done; every register else kept. */
static void a_retry_action(Cpu *c) {
  bool r;
  ADAPT(c, r = retry_action());
  c->zf = !r;
}

/* $7B63: B the object, A nonzero for a place, DE where to. */
static void a_copy_name(Cpu *c) { ADAPT(c, copy_name(c->b, c->a != 0, get_de(c))); }

/* $7B78: no outputs used. */
static void a_decode_flags(Cpu *c) { ADAPT(c, decode_flags()); }

/* $7B9E, record IY: NZ with IX the entry; Z if none. Leaving for the turn
 * is the faithful $7C16's (its return address is on the stack). */
static void a_find_action(Cpu *c) {
  uint16_t entry = 0;
  enum Found r;
  ADAPT(c, r = find_action(c->iy, &entry));
  c->zf = r != ACTION_FOUND;
  if (r == ACTION_FOUND) c->ix = entry;
  if (r == NO_ACTION_LEAVE) cpu_tail(c, 0x7C16);
}

/* $7C23: record IY, entry IX, A nonzero if swapped. No outputs used. */
static void a_take_phrases(Cpu *c) { ADAPT(c, take_phrases(c->iy, c->ix, c->a != 0)); }

/* $7C91: the phrase at IY+A to DE (N1_PHRASE or N2_PHRASE, with HL their
 * flags: the only two callers, $7C76 and $7C8B, load them as constants).
 * No outputs used. */
static void a_copy_phrase(Cpu *c) {
  int n = get_de(c) != EXEC_VARS + offsetof(ExecVars, phrase);
  ADAPT(c, copy_phrase((uint16_t)(c->iy + c->a), n));
}

/* $7CAC: record IY, offset E, room B, HL where to; B and HL out (the next
 * call takes them), D = 0. */
static void a_collect_word(Cpu *c) {
  uint16_t dest = get_hl(c);
  uint8_t room = c->b;
  collect_word(c->iy, c->e, &dest, &room);
  if (c->b) c->d = 0;
  c->b = room;
  set_hl(c, dest);
}

/* $7CC9: JP (IY), the matcher: A and IX out. */
static void a_call_iy(Cpu *c) {
  Regs r = {c->a, c->b, c->c, c->d, c->e, c->h, c->l, c->ix, c->iy, c->zf, c->cf};
  bridge_call(c->iy, &r);
  c->a = r.a, c->b = r.b, c->c = r.c, c->d = r.d, c->e = r.e, c->h = r.h, c->l = r.l;
  c->ix = r.ix, c->iy = r.iy, c->zf = r.zf, c->cf = r.cf;
}

/* $7CCB, $7D17: NZ if found. */
static void a_find_object1(Cpu *c) {
  bool r;
  ADAPT(c, r = find_object(0));
  c->zf = !r;
}
static void a_find_object2(Cpu *c) {
  bool r;
  ADAPT(c, r = find_object(1));
  c->zf = !r;
}

/* $7CFC, $7D54: matcher IY ($9DD9 objects or $9EA0 places: every caller
 * loads one of the two, so the check does not mutate it), list IX; A ($FF
 * none) and IX out. */
static void a_next_object(Cpu *c, int n) {
  uint16_t ix = c->ix;
  uint8_t r;
  Matcher m = c->iy == 0x9EA0 ? MATCH_PLACE : MATCH_OBJECT;
  ADAPT(c, r = next_object(n, m, &ix));
  c->a = r;
  c->ix = ix;
}
static void a_next_object1(Cpu *c) { a_next_object(c, 0); }
static void a_next_object2(Cpu *c) { a_next_object(c, 1); }

/* $7D6B: A = 1. */
static void a_output_on(Cpu *c) {
  output_on();
  c->a = 1;
}

/* $7D74: A what, IY the record. No outputs used. */
static void a_keep_question(Cpu *c) { keep_question(c->a, c->iy); }

/* $7E78, $7E7C: return with the two words pushed (the parameters of a
 * message), +4's first; HL = the return address. */
static void a_entry_words(Cpu *c, bool when_set) {
  EntryWords w = entry_words(when_set);
  uint16_t ret = pop16(c);
  push16(c, w.w4);
  push16(c, w.w2);
  push16(c, ret);
  set_hl(c, ret);
  c->ix = ev()->entry;
}
static void a_entry_words_z(Cpu *c) { a_entry_words(c, false); }
static void a_entry_words_nz(Cpu *c) { a_entry_words(c, true); }

/* $7EBA (from $9058): A the number. Registers kept but A and F: A = $FF
 * if some were cancelled, else 0; Z; no carry. */
static void a_give_orders(Cpu *c) {
  uint8_t n = c->a;
  uint8_t rest = (uint8_t)(mem[SPEECH_COUNT] - (mem[SPEECH_COUNT] < n ? mem[SPEECH_COUNT] : n));
  give_orders(n);
  c->a = rest ? 0xFF : 0;
  c->zf = true, c->cf = false;
}

/* $7EFF: HL the slot, Z if found; A the actor, DE $19, B the slots left. */
static void a_find_orders(Cpu *c) {
  bool found;
  uint16_t slot = find_orders(&found);
  set_hl(c, slot);
  c->zf = found;
  c->a = mem[V_ACTOR];
  set_de(c, SPEECH_SLOT_SIZE);
  c->b = (uint8_t)(SPEECH_SLOTS_N - (slot - SPEECH_SLOTS) / SPEECH_SLOT_SIZE);
}

/* $7F10 (from $9874): Z if there is an order; A the actor. */
static void a_has_orders(Cpu *c) {
  c->zf = has_orders();
  c->a = mem[V_ACTOR];
}

/* $7F1A (from $98A9, $A8B6, $A8FD): A nonzero to carry the order out. NZ
 * when done (A = V_DONE), or with A = 0 in (A = 1, HL the record); else
 * Z, A = 0. Other registers kept. */
static void a_obey(Cpu *c) {
  bool run = c->a != 0, r;
  uint16_t record;
  ADAPT(c, r = obey_orders(run, &record));
  c->zf = !r;
  if (!run) {
    c->a = 1;
    set_hl(c, record);
  } else {
    c->a = r ? mem[V_DONE] : 0;
  }
}

/* $7F60 (from $97A4, $7F5A): A the character; registers kept, flags as
 * the CP with the last slot. */
static void a_drop_orders(Cpu *c) {
  uint8_t last = mem[SPEECH_SLOTS + (SPEECH_SLOTS_N - 1) * SPEECH_SLOT_SIZE];
  drop_orders(c->a);
  c->zf = last == c->a;
  c->cf = c->a < last;
}

#define KEPT (OUT_BC | OUT_DE | OUT_HL | OUT_IX | OUT_IY)

const CleanRoutine executor_clean[] = {
    {0x7960, "execute_sentences", a_execute, OUT_A | OUT_ZF},
    {0x798E, "execute_at(AT_ALL)", a_execute_798e, OUT_A | OUT_ZF},
    {0x7C16, "execute_at:pop", a_pop_7c16, 0},
    {0x7C17, "execute_at:pop2", a_pop_7c17, 0},
    {0x7C18, "execute_at(AT_TURN)", a_execute_7c18, OUT_A | OUT_ZF},
    {0x7DC7, "execute_at:pop3", a_pop_7dc7, 0},
    {0x7DC8, "execute_at(AT_NEXT)", a_execute_7dc8, OUT_A | OUT_ZF},
    {0x79A9, "clear_vars", a_clear_vars, 0},
    {0x79B6, "run_sentence", a_run_sentence, OUT_ZF},
    {0x7A14, "match_objects", a_match_objects, OUT_ZF},
    {0x7A73, "is_excepted", a_is_excepted, OUT_ZF | OUT_DE | OUT_HL | OUT_IY},
    {0x7AA1, "start_search_once", a_start_search_once, 0},
    {0x7AA6, "start_search(0)", a_start_search1, 0},
    {0x7ABA, "start_search(1)", a_start_search2, 0},
    {0x7ACC, "object_list", a_object_list, OUT_IX},
    {0x7AD8, "wants_object2", a_wants_object2, OUT_ZF},
    {0x7AED, "try_action", a_try_action, OUT_A | OUT_ZF},
    {0x7AF5, "retry_action", a_retry_action, OUT_ZF | KEPT},
    {0x7B63, "copy_name", a_copy_name, 0},
    {0x7B78, "decode_flags", a_decode_flags, 0},
    {0x7B9E, "find_action", a_find_action, OUT_ZF | OUT_IX},
    {0x7C23, "take_phrases", a_take_phrases, 0},
    {0x7C91, "copy_phrase", a_copy_phrase, 0, OUT_DE | OUT_HL},
    {0x7CAC, "collect_word", a_collect_word, OUT_B | OUT_D | OUT_HL},
    {0x7CC9, "call matcher", a_call_iy, OUT_A | OUT_IX},
    {0x7CCB, "find_object(0)", a_find_object1, OUT_ZF},
    {0x7CFC, "next_object(0)", a_next_object1, OUT_A | OUT_IX, OUT_IY},
    {0x7D17, "find_object(1)", a_find_object2, OUT_ZF},
    {0x7D54, "next_object(1)", a_next_object2, OUT_A | OUT_IX, OUT_IY},
    {0x7D6B, "output_on", a_output_on, 0},
    {0x7D74, "keep_question", a_keep_question, 0},
    {0x7E78, "entry_words(false)", a_entry_words_z, OUT_HL | OUT_IX},
    {0x7E7C, "entry_words(true)", a_entry_words_nz, OUT_HL | OUT_IX},
    {0x7EBA, "give_orders", a_give_orders, OUT_A | OUT_ZF | OUT_CF | KEPT},
    {0x7EFF, "find_orders", a_find_orders, OUT_A | OUT_B | OUT_DE | OUT_HL | OUT_ZF},
    {0x7F10, "has_orders", a_has_orders, OUT_A | OUT_ZF | KEPT},
    {0x7F1A, "obey_orders", a_obey, OUT_A | OUT_ZF | KEPT},
    {0x7F60, "drop_orders", a_drop_orders, OUT_A | OUT_ZF | OUT_CF | KEPT},
    {0, NULL, NULL, 0},
};

const CleanScratch executor_scratch[] = {
    /* $7E78/$7E7C write a JR Z or JR NZ into their own code; nothing else
     * reads it. */
    {0x7E92, 0x7E92, "entry_words: its own JR condition"},
    {0x7EA1, 0x7EA1, "entry_words: its own JR condition"},
    /* PrintMsg ($72DD) keeps the caller's A, DE and IX here and gives them
     * back at its end; the clean callers have other registers (and do not
     * read them back). Belongs to text: to move there. */
    {0x70DC, 0x70DE, "PrintMsg's saved A and DE"},
    {0x70E0, 0x70E1, "PrintMsg's saved IX"},
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
