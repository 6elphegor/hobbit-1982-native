/* The clean edition: characters and events. See docs/CLEAN.md.
 *
 * What the faithful port has in port/actions2.c ($93DA-$9A84: examine,
 * light and dark, describing a location, the end of a turn, the
 * characters' scripts, killing) and port/events.c ($A541-$AB52 and
 * $C7A4-$C7FB: climbing in and out, and the events of the characters and
 * places: the trolls, Gollum, the dragon, the barrels, the spiders, the
 * magic door, the timed events, the location events).
 *
 * The original's handlers stop themselves with stack tricks: $9D44 (when
 * only trying an action) and $97FF (unless the action was confirmed) pop
 * their caller's return address. Here they are only_trying() and
 * action_confirmed(), and the handler returns. */
#include <stddef.h>

#include "actions.h"
#include "characters.h"
#include "drawing.h"
#include "executor.h"
#include "handlers.h"
#include "objects.h"
#include "platform.h"
#include "rng.h"
#include "screen.h"
#include "text.h"

/* ---------- the game's data used here ---------- */

#define V_TIMED_FIRED 0xB6F0 /* a timed event has reached zero this turn */
#define V_MAP_SHOWN 0xB6F1   /* the map's clue has been written into it */
#define V_FOREST_ENTRY 0xB6F3 /* where you came into the forest */
#define V_HAS_ORDERS 0xB6F4  /* the acting character has an order waiting (1) */
#define V_RIDDLE_ASKED 0xB6F9
#define V_SAID 0xB6E6        /* what was said (an action), for $A8AB */
#define V_RIDDLE 0xB6EE      /* the riddle chosen at the start: 4 bytes at $C7FC */
#define V_OBJECT1_SPECIAL 0xB6FE /* the first object is not a real one (1) */
#define V_OBJECT2_SPECIAL 0xB6FF
#define V_TIMER_RUNNING 0xB700
#define V_LAST_KEY 0xB704
#define V_B711 0xB711        /* unknown: nonzero, nothing is seen in the dark */
#define V_PICTURE 0x7F77     /* the picture drawn last: $FF none */
#define V_DARKNESS 0x980C    /* (in code space) 0 light, 1 dark, 2 dark and "you hear" said */
#define V_OBJECT1_PLACE 0x980D /* (in code space) the first object's location, $FF if many */
#define V_BIT_OPERATION 0x948B /* (in code space) the operand of SET/RES 0,(IX+7) at $9488 */
#define V_MAP_RECORD 0xA7D1  /* (in code space) operand of LD IY,nn at $A7CF: the map's clue */
#define V_ENTERING 0x8D9B    /* the location being entered */

#define PLAYER 0xC11B        /* your object record */
#define SWORD 0x0E
#define SWORD_RECORD 0xC305
#define BARREL 0x13
#define PREPOSITIONS 0xBA80  /* the words "in", "on", "at"... by location attributes bits 1-3 */
#define RIDDLES 0xC7FC
#define MAP_CLUES 0xC808
#define PICTURES 0xCC00      /* [location][picture data] triples */
#define CHARACTER_LIMIT 6    /* actions a character may fail in one turn */

enum {
  ACTION_EAT_YOU = 0x1B,
  ACTION_GIVE = 0x1D,
  ACTION_SAY_TO = 0x1E,
  ACTION_CLIMB_OUT = 0x37,
};

/* Script ops: the type is the low nibble. */
enum {
  OP_ROUTINE = 0x01,   /* (types 0-3) a routine, not an action */
  OP_JUMP_ON_FAIL = 0x10, /* a jump address follows */
  OP_ONCE = 0x20,      /* cleared once done */
  OP_ALWAYS = 0x40,    /* done even when the character was told something */
  OP_TYPE_ACTION = 4,  /* below: an action with objects, or a routine */
  OP_TYPE_FIRST_SPECIAL = 5,
  OP_TYPE_CHOOSE = 0x0C,
  OP_TYPE_GOTO = 0x0E,
  OP_TYPE_RANDOM = 0x0F,
};

/* Messages. */
enum {
  MSG_DARK = 0xAE1F,          /* "it is dark" */
  MSG_NOT_CARRYING = 0xADF1,  /* "You are not carrying it" */
  MSG_SWEPT_AWAY = 0xADFC,    /* "and it gets swept away" */
  MSG_SEE_NOTHING = 0xAFC4,   /* "i see nothing here" */
  MSG_CARRYING = 0xAFE4,      /* "[0x04] is carrying[0x04]" */
  MSG_YOU_ARE_IN = 0xAFFC,    /* "You are in[0x16]": the preposition is written at +1 */
  MSG_YOU_SEE = 0xB000,
  MSG_YOU_SAY = 0xB009,       /* 'You say "' */
  MSG_END_QUOTE = 0xB00F,     /* '".' */
  MSG_ENTERS = 0xB017,        /* "[0x00] enters." */
  MSG_GOES = 0xB01C,
  MSG_YOU_HEAR = 0xB027,      /* "you hear a noise" */
};

/* ---------- records ---------- */

static CharacterRecord *character_at(uint16_t record) { return (CharacterRecord *)&mem[record]; }
static ObjectRecord *you(void) { return object_at(PLAYER); }

/* $9D44 CheckPhase: when only trying the action, say it can be done
 * (V_DONE = V_DOING + 1) and stop: true. */
static bool only_trying(void) {
  if (mem[V_DOING] == 1) return false;
  mem[V_DONE] = (uint8_t)(mem[V_DOING] + 1);
  return true;
}

/* ---------- other modules' routines ----------
 * Their clean functions, called directly; these few helpers give them
 * the shape the original's callers here use. */

/* $9BCA LocateObject: the object's record; *found false (if asked) when
 * it is not in the index, and then the word after the index's end. */
static uint16_t locate_object(uint8_t id, bool *found) {
  uint16_t entry = find_entry(OBJECT_INDEX, id);
  if (found) *found = entry_key(entry) != 0xFF;
  return entry_word(entry);
}

/* $9BB1 LocateLocation: the location's record, 0 if there is none. */
static uint16_t locate_location(uint8_t location) {
  LocationRecord *l = location_record(location);
  return l ? addr_of(l) : 0;
}

/* The same, with fallback for a location that does not exist. */
static uint16_t locate_location_or(uint8_t location, uint16_t fallback) {
  LocationRecord *l = location_record(location);
  return l ? addr_of(l) : fallback;
}

/* $9DBD IndexIdTable: the table's entry for key, or its end ($FF): then
 * *found (if asked) is false. */
static uint16_t find_in_table(uint16_t table, uint8_t key, bool *found) {
  uint16_t entry = find_entry(table, key);
  if (found) *found = entry_key(entry) != 0xFF;
  return entry;
}

/* $9B81: whether the object (its record) has its own entry for the
 * action; *entry (if asked) the entry, or the end of its list. */
static bool object_action(uint16_t record, uint8_t action, uint16_t *entry) {
  uint16_t e = object_action_entry(object_at(record), action);
  if (entry) *entry = e;
  return entry_key(e) != 0xFF;
}

/* $9F82: the object's first location ($FF if it is in many places) and
 * its record (0 for no object). */
static uint8_t first_location(uint8_t id, uint16_t *record) {
  *record = id == OBJECT_NONE ? 0 : object_record_addr(id);
  return object_first_location(id);
}

/* $9F28 / $9F2D: the actor's exit through the door / to the location.
 * The original answers "not found" when looking for $FF, even if an exit
 * has $FF there. */
static bool exit_through_door(uint8_t door, uint16_t *exit) {
  return find_exit_door(door, exit) && door != 0xFF;
}
static bool exit_to(uint8_t location, uint16_t *exit) {
  return find_exit_to(location, exit) && location != 0xFF;
}

/* $9A85: the character's entry in the characters' table; *found false if
 * it has none (the end, $FF). */
static uint16_t character_entry(uint8_t id, bool *found) {
  uint16_t entry = find_character(id);
  *found = mem[entry] != 0xFF;
  return entry;
}

/* $7F1A: the actor obeys the order it was given: true if it was done. */
static bool obeys(void) {
  uint16_t record;
  return obey_orders(true, &record);
}

/* $90DF: wait for a key, and start again. Never returns. */
static void wait_and_restart(void) {
  wait_key_after_death();
  device_restart();
}

/* ========== object action handlers ($93DA-$94D5) ========== */

/* $93DA: examine the first object: its own description if it has one,
 * else "You see <its name>." */
void examine(void) {
  if (only_trying()) return;
  uint16_t record = locate_object(mem[V_OBJECT1], NULL);
  uint16_t help = object_at(record)->help;
  if (help) {
    print_message(help, NULL);
    return;
  }
  print_message(MSG_YOU_SEE, NULL);
  print_object_name(object_at(record));
  print_char('.');
  print_newline();
}

/* $9404 (the barrel, action $22): empty the first object: refused if it
 * is not open, or has nothing in it; else what is in it falls out, and it
 * is no longer full. */
void empty_out(void) {
  uint16_t record = word_at(V_OBJECT1_RECORD);
  if (!object_is_open(record)) {
    say_object_is(record, 0x05);
    return;
  }
  if (object_count(mem[V_OBJECT1]) == 0) {
    say_object_is(record, 0x02);
    return;
  }
  if (only_trying()) return;
  empty_object(mem[V_OBJECT1]);
  object_at(record)->attrs &= (uint8_t)~ATTR_FULL;
}

/* $9428 (the rivers, put in and throw in): the first object, if it is in
 * one place and not a liquid, is swept away to the river's next location
 * after the one it is put in from, the actor's (none after the last); you
 * die if that took you nowhere. (The original's bug takes the location of
 * the last character to have had its turn, V_ACTOR_LOCATION, as the
 * actor's: for you it almost always says "i cannot do that".) */
void river_sweeps_away(void) {
  ObjectRecord *thing = object_at(word_at(V_OBJECT1_RECORD));
  if (thing->nlocations != 1 || (thing->attrs & ATTR_LIQUID)) { /* $8D25 */
    finish_action();
    return;
  }
  if (only_trying()) return;
  uint8_t from = original_bugs ? mem[V_ACTOR_LOCATION] : object_at(word_at(V_ACTOR_RECORD))->locations[0];
  ObjectRecord *river = object_at(word_at(V_OBJECT2_RECORD));
  unsigned n = river->nlocations ? river->nlocations : 256, i = 0;
  while (river->locations[i] != from)
    if (++i == n) {
      print_cannot_do_that();
      return;
    }
  uint8_t to = i == n - 1 ? 0 : river->locations[i + 1];
  print_message(MSG_SWEPT_AWAY, NULL);
  uint16_t record = word_at(V_OBJECT1_RECORD);
  object_at(record)->locations[0] = to;
  object_at(record)->container = OBJECT_NONE;
  object_moved(mem[V_OBJECT1], to, 0);
  if (you()->locations[0] == 0) you_are_dead();
}

/* $9475: lock (set bit 0 of) or unlock the first object, unless the second
 * (the key) is broken. (The original writes the SET or RES opcode into
 * the instruction at $9488 and runs it: V_BIT_OPERATION, scratch.) */
static void lock_or_unlock(bool lock) {
  uint16_t key = word_at(V_OBJECT2_RECORD);
  if (object_at(key)->attrs & ATTR_BROKEN) {
    say_object_is(key, 0x83);
    return;
  }
  if (only_trying()) return;
  ObjectRecord *thing = object_at(word_at(V_OBJECT1_RECORD));
  if (lock)
    thing->attrs |= ATTR_LOCKED;
  else
    thing->attrs &= (uint8_t)~ATTR_LOCKED;
}

/* $946D: lock the first object: refused if it is locked already, or open. */
void lock_object(void) {
  uint16_t record = word_at(V_OBJECT1_RECORD);
  uint8_t attrs = object_at(record)->attrs;
  if (attrs & ATTR_LOCKED) {
    say_object_is(record, 0x80);
    return;
  }
  if (attrs & ATTR_OPEN) {
    say_object_is(record, 0x85);
    return;
  }
  lock_or_unlock(true);
}

/* $948D: unlock the first object: refused if it is not locked, or open. */
void unlock_object(void) {
  uint16_t record = word_at(V_OBJECT1_RECORD);
  uint8_t attrs = object_at(record)->attrs;
  if (!(attrs & ATTR_LOCKED)) {
    say_object_is(record, 0x00);
    return;
  }
  if (attrs & ATTR_OPEN) {
    say_object_is(record, 0x85);
    return;
  }
  lock_or_unlock(false);
}

/* $94A4 (action $2C): put the first object (which must be carried)
 * through the second, an open door, to the place beyond it. */
void send_through_door(void) {
  if (!object_inside(mem[V_OBJECT1], mem[V_ACTOR], NULL)) { /* $8C9B */
    print_message(MSG_NOT_CARRYING, NULL);
    return;
  }
  uint16_t exit;
  bool found = exit_through_door(mem[V_OBJECT2], &exit);
  if (!found) {
    finish_action();
    return;
  }
  uint16_t door = word_at(V_OBJECT2_RECORD);
  if (!object_is_open(door)) {
    say_object_is(door, 0x05);
    return;
  }
  if (only_trying()) return;
  uint8_t beyond = mem[(uint16_t)(exit + 2)];
  uint16_t record = word_at(V_OBJECT1_RECORD);
  object_at(record)->container = OBJECT_NONE;
  object_at(record)->locations[0] = beyond;
  object_moved(mem[V_OBJECT1], beyond, 0);
}

/* ========== trying and doing actions ($94D6-$962A) ========== */

/* $94D6: may V_ACTION be tried on the first object? Not if the actor is
 * one of the objects; yes if the action table has a routine for it, or it
 * is one of the five actions at $A20B, or the object has a routine for
 * it. The answer is left in V_DONE. */
bool may_try_action(void) {
  uint8_t ok;
  bool found;
  if (actor_is_object())
    ok = 0;
  else if (find_in_table(DEFAULT_ACTIONS, mem[V_ACTION], &found), found)
    ok = 1;
  else if (action_in_list())
    ok = 1;
  else
    ok = object_action(locate_object(mem[V_OBJECT1], NULL), mem[V_ACTION], NULL);
  mem[V_DONE] = ok;
  return ok;
}

/* Run the handler at entry (an [action][routine] triple) and those that
 * follow it with action 0. */
static void run_handlers(uint16_t entry) {
  do {
    run_routine(word_at((uint16_t)(entry + 1)));
    entry += 3;
  } while (mem[entry] == 0);
}

/* $950F: carry out V_ACTION on the objects for V_ACTOR: unless the player
 * cannot see them in the dark, or a character is carrying one ($9728),
 * run the objects' own handlers for it, or the action table's. Then, when
 * doing it, say "it is dark" if it is, and let the characters among the
 * objects react. */
void do_action(void) {
  uint16_t entry;
  bool found;
  if (actor_is_object()) {
    print_cannot_do_that();
    return;
  }
  bool seen = true;
  if (player_in_dark())
    seen = mem[V_B711] == 0 && object_inside(mem[V_OBJECT1], mem[V_ACTOR], NULL) &&
           object_inside(mem[V_OBJECT2], mem[V_ACTOR], NULL);
  if (!seen) {
    print_message(MSG_SEE_NOTHING, NULL);
  } else {
    bool own = false; /* the objects have handlers of their own */
    uint8_t object1 = mem[V_OBJECT1], object2 = mem[V_OBJECT2];
    if (mem[V_OBJECT1_SPECIAL] != 1 && object1 != OBJECT_NONE) {
      uint16_t record1 = locate_object(object1, &found);
      set_word_at(V_OBJECT1_RECORD, record1);
      if (say_who_carries(found ? object1 : OBJECT_NONE)) goto check_light;
      uint16_t record = record1;
      if (object2 == OBJECT_NONE) {
        own = true;
      } else if (mem[V_OBJECT2_SPECIAL] != 1) {
        uint16_t record2 = locate_object(object2, &found);
        set_word_at(V_OBJECT2_RECORD, record2);
        if (say_who_carries(found ? object2 : OBJECT_NONE)) goto check_light;
        if (action_in_list()) record = record2;
        own = true;
      }
      if (own) own = object_action(record, mem[V_ACTION], &entry);
    }
    if (!own) {
      entry = find_in_table(DEFAULT_ACTIONS, mem[V_ACTION], &found);
      if (!found) {
        print_cannot_do_that();
        return;
      }
    }
    run_handlers(entry);
  }
check_light:
  if (mem[V_DOING] != 1) return;
  if (mem[V_ACTOR] == YOU && player_in_dark()) print_message(MSG_DARK, NULL);
  character_reacts(mem[V_OBJECT1], word_at(V_OBJECT1_RECORD), mem[V_ACTION]);
  character_reacts(mem[V_OBJECT2], word_at(V_OBJECT2_RECORD), mem[V_ACTION]);
}

/* $95DF: if object id (record) is a living character, it reacts to the
 * action: its script changes to its entry for the action, if it has one. */
void character_reacts(uint8_t id, uint16_t record, uint8_t action) {
  /* (With no object, the original reads its record from 0, the ROM.) */
  if (id == OBJECT_NONE && !original_bugs) return;
  uint8_t attrs = object_at(record)->attrs;
  if ((attrs & ATTR_ANIMATE) && !(attrs & ATTR_BROKEN)) character_set_entry(id, action);
}

/* $95ED: is it dark for the player (only when the player acts)? Dark
 * unless the player is in a lit location and not inside anything that
 * cannot be seen out of, or the sword is here glowing (light and full, not
 * broken). */
bool player_in_dark(void) {
  if (mem[V_ACTOR] != YOU) return false;
  if (opaque_container(object_at(PLAYER)) == OBJECT_NONE &&
      (location_at(actor_location_addr())->attrs & LOC_LIT))
    return false;
  if (!actor_can_see(SWORD, object_at(SWORD_RECORD))) return true;
  return (object_at(SWORD_RECORD)->attrs & (ATTR_LIGHT | ATTR_BROKEN | ATTR_FULL)) != (ATTR_LIGHT | ATTR_FULL);
}

/* ========== describing a location ($962B-$96B2) ========== */

/* $962B: describe the location, starting "You see". */
void describe_location_you_see(uint8_t location, uint16_t fallback) {
  describe_location_after(MSG_YOU_SEE, location, fallback);
}

/* $9630: describe the location, starting "You are" and its own
 * preposition ("in", "on", "at"...), which is written into the message. */
void describe_location(uint8_t location, uint16_t fallback) {
  uint8_t attrs = location_at(locate_location_or(location, fallback))->attrs;
  uint16_t word = word_at((uint16_t)(PREPOSITIONS + (attrs & 0x0E)));
  mem[MSG_YOU_ARE_IN + 1] = word >> 8;
  mem[MSG_YOU_ARE_IN + 2] = (uint8_t)word;
  describe_location_after(MSG_YOU_ARE_IN, location, fallback);
}

/* $965B (and $964D, which keeps the registers): the message, then the
 * location's description, its picture (waiting for a key if one was
 * drawn), the doors, the exits and the objects there. */
void describe_location_after(uint16_t message, uint8_t location, uint16_t fallback) {
  uint16_t record = locate_location_or(location, fallback);
  print_message(message, NULL);
  print_location_text(record);
  draw_location_picture(location);
  if (mem[V_PICTURE] != 0xFF) wait_key_after_picture();
  print_newline();
  list_doors(location, fallback);
  list_exits(location, fallback);
  list_you_see();
}

/* $9686: the location's own description if it has one, else its words. */
void print_location_text(uint16_t record) {
  uint16_t description = location_at(record)->description;
  if (description)
    print_message(description, NULL);
  else
    print_location_words(record);
}

/* $9689: the location's words (its name). */
void print_location_words(uint16_t record) { print_name((uint16_t)(record + 2)); }

/* $96A8: the short description of a location: its words, then the exits
 * (of exits_of: the original lists those of the location in B, which its
 * one caller sets to the same place) and the objects. */
void describe_location_briefly(uint8_t location, uint8_t exits_of, uint16_t fallback) {
  print_location_words(locate_location_or(location, fallback));
  print_newline();
  list_exits(exits_of, fallback);
  list_you_see();
}

/* ========== the turn ($96B3-$97FF) ========== */

/* $96B3: the end of a turn: is the game won, the characters act, then the
 * timed events count down. When one reaches zero its routine runs; only
 * one may do so in a turn (the others wait at 1). While one is at or
 * below its threshold, its other routine runs. */
void end_of_turn(void) {
  check_game_won();
  characters_act();
  mem[V_TIMED_FIRED] = 0;
  mem[V_DONE] = mem[V_DOING] = 1;
  for (uint16_t at = TIMED_EVENTS; mem[at] != 0xFF; at += sizeof(TimedEvent)) {
    TimedEvent *event = (TimedEvent *)&mem[at];
    if (event->countdown == 0) continue;
    if (--event->countdown == 0) {
      uint8_t fired = mem[V_TIMED_FIRED];
      event->countdown = fired;
      if (fired != 1) {
        mem[V_TIMED_FIRED] = (uint8_t)(fired + 1);
        run_routine(event->at_zero);
        continue;
      }
    }
    if (event->threshold != 0 && event->countdown <= event->threshold)
      run_routine(event->below);
  }
  mem[V_OUTPUT_ON] = 1;
}

/* $9728: if the player tries to use an object (not $FF) that a living
 * character is carrying, and the player is visible, say "<character> is
 * carrying <object>": true. */
bool say_who_carries(uint8_t object) {
  if (object == OBJECT_NONE) return false;
  if (mem[V_ACTOR] != YOU) return false;
  uint16_t record = locate_object(object, NULL);
  if (object_at(record)->container == OBJECT_NONE) return false;
  uint8_t holder;
  if (object_inside(object, mem[V_ACTOR], &holder)) return false;
  uint16_t holder_record = locate_object(holder, NULL);
  if (!(object_at(holder_record)->attrs & ATTR_ANIMATE)) return false;
  if (!(you()->attrs & ATTR_VISIBLE)) return false;
  uint16_t names[2] = {object_at(holder_record)->name[0], object_at(record)->name[0]};
  print_message(MSG_CARRYING, names);
  return true;
}

/* $977F: character or object id dies (or is destroyed); id 0 is the
 * player. It is marked dead, what it holds falls, it stops acting, it is
 * reset, and what it was told is forgotten. */
void kill(uint8_t id) {
  if (id == YOU) {
    you_are_dead();
    return;
  }
  uint16_t record = locate_object(id, NULL);
  object_at(record)->attrs |= ATTR_BROKEN;
  empty_object(id);
  bool found;
  uint16_t entry = character_entry(id, &found);
  if (found) mem[entry] = 0;
  reset_object(id);
  drop_orders(id);
}

/* $977C: the first object dies. */
void kill_first_object(void) { kill(mem[V_OBJECT1]); }

/* $97AD: at the start of a game: the player acts; one of the map's five
 * clues is chosen (and three bytes where it points are cleared), and one
 * of the four riddles. */
void new_game_characters(void) {
  mem[V_ACTOR] = YOU;
  mem[V_MAP_SHOWN] = 0;
  mem[V_RIDDLE_ASKED] = 0;
  set_word_at(V_ACTOR_RECORD, PLAYER);
  uint8_t clue = (uint8_t)(random_upto(4) + 1);
  uint16_t record = (uint16_t)(MAP_CLUES + 6 * clue);
  set_word_at(V_MAP_RECORD, record);
  uint16_t text = word_at((uint16_t)(record + 1));
  for (int i = 0; i < 3; i++) mem[(uint16_t)(text + i)] = 0;
  uint8_t riddle = random_upto(3);
  set_word_at(V_RIDDLE, (uint16_t)(RIDDLES + 4 * riddle));
}

/* $97F4: the message, a full stop and a newline. */
void say_sentence(uint16_t message) {
  print_message(message, NULL);
  print_char('.');
  print_newline();
}

/* $97FF: has the action been confirmed (V_DOING and V_DONE both set)?
 * Handlers stop if not. */
bool action_confirmed(void) { return (mem[V_DOING] & mem[V_DONE]) != 0; }

/* ========== the characters' scripts ($980E-$9A84) ========== */

/* $9918: the op at op (of length bytes, two more if a jump address
 * follows) is done with: the character's script moves past it. */
uint16_t script_skip(uint16_t character, uint16_t op, uint16_t length) {
  uint16_t next = (uint16_t)(op + length);
  if (mem[op] & OP_JUMP_ON_FAIL) next += 2;
  character_at(character)->current = next;
  return next;
}

/* $9A68: the character's script becomes its entry number entry (at most
 * the last). */
void script_choose(uint16_t character, uint8_t entry) {
  CharacterRecord *ch = character_at(character);
  if (ch->entries < entry) entry = ch->entries;
  ch->current = word_at((uint16_t)(ch->table + 3 * entry + 1));
}

/* $9A59: the character's script becomes one of its entries at random, up
 * to the op's limit (op+1). */
void script_random(uint16_t character, uint16_t op) {
  uint8_t limit = mem[(uint16_t)(op + 1)], entries = character_at(character)->entries;
  if (limit >= entries) limit = entries;
  script_choose(character, random_upto(limit));
}

/* $99CE: the actor does V_ACTION, saying so where the player can see
 * (where the first object is somewhere the player is, though the actor is
 * not, it is "someone" who does it); then says where the actor and the
 * first object went. */
static void character_acts(void) {
  if (mem[V_OBJECT1_SPECIAL] == 1) goto act;
  if (mem[V_ACTION] == ACTION_SAY_TO && mem[V_ACTOR_LOCATION] != you()->locations[0]) goto act;
  if (mem[V_OBJECT1] != OBJECT_NONE) {
    uint16_t record;
    uint8_t place = first_location(mem[V_OBJECT1], &record);
    mem[V_OBJECT1_PLACE] = place;
    uint8_t here = mem[V_YOUR_LOCATION];
    if (place == 0xFF && here != mem[V_ACTOR_LOCATION]) {
      ObjectRecord *object = object_at(record);
      unsigned n = object->nlocations ? object->nlocations : 256;
      for (unsigned i = 0; i < n; i++)
        if (object->locations[i] == here) {
          uint8_t actor = mem[V_ACTOR];
          mem[V_ACTOR] = 0xFF; /* someone */
          mem[V_OUTPUT_ON] = 1;
          describe_action();
          mem[V_OUTPUT_ON] = 0;
          mem[V_ACTOR] = actor;
          goto act;
        }
    }
  }
  describe_action();
act:
  do_action();
  mention_object(mem[V_ACTOR], mem[V_ACTOR_LOCATION], MSG_ENTERS);
  if (mem[V_OBJECT1_SPECIAL] != 1) mention_object(mem[V_OBJECT1], mem[V_OBJECT1_PLACE], MSG_GOES);
}

/* $99C6: the actor tries V_ACTION on the objects; if it can, does it:
 * true. */
bool character_tries(void) {
  if (!retry_action()) return false;
  character_acts();
  return true;
}

/* One op of a character's script. Returns true when the character's turn
 * is over, false to go on with its script (from ch->current). */
static bool script_step(uint16_t character, uint16_t op, uint8_t *failures) {
  CharacterRecord *ch = character_at(character);
  uint8_t type = mem[op] & 0x0F;
  uint16_t length = 4;
  bool done;
  if (type >= OP_TYPE_FIRST_SPECIAL) {
    if (type == OP_TYPE_GOTO) {
      ch->current = word_at((uint16_t)(op + 1));
      return false;
    }
    if (type == OP_TYPE_CHOOSE) {
      character_set_entry(ch->id, mem[(uint16_t)(op + 1)]);
      return false;
    }
    if (type == OP_TYPE_RANDOM) {
      script_random(character, op);
      return false;
    }
    script_choose(character, 0);
    return true;
  }
  /* Told to do something: that comes first (unless the op is always done). */
  if (mem[V_HAS_ORDERS] == 1 && !(mem[op] & OP_ALWAYS)) {
    mem[V_HAS_ORDERS] = 0;
    if (obeys()) {
      mem[V_DOING] = mem[V_DONE] = 1;
      character_acts();
      return true;
    }
  }
  if ((mem[op] & 0x0F) == OP_TYPE_ACTION) {
    length = 2;
    script_skip(character, op, length);
    uint8_t action = mem[(uint16_t)(op + 1)];
    if (action == 0xFF) { /* the end: jump, if there is somewhere to */
      if (mem[op] & OP_JUMP_ON_FAIL) ch->current = word_at((uint16_t)(op + 2));
      return true;
    }
    mem[V_ACTION] = action;
    mem[V_OBJECT1] = mem[V_OBJECT2] = OBJECT_NONE;
    if (character_tries()) return true;
  } else {
    script_skip(character, op, length);
    if (!(mem[op] & OP_ROUTINE)) {
      mem[V_ACTION] = mem[(uint16_t)(op + 1)];
      mem[V_OBJECT1] = mem[(uint16_t)(op + 2)];
      mem[V_OBJECT2] = mem[(uint16_t)(op + 3)];
      done = character_tries();
    } else { /* a routine: tried, then done if it may be */
      uint16_t routine = word_at((uint16_t)(op + 1));
      mem[V_DOING] = mem[V_DONE] = 0;
      run_routine(routine);
      done = mem[V_DONE] == 1;
      if (done) {
        mem[V_DOING] = 1;
        run_routine(routine);
      }
    }
    if (done) {
      if (mem[op] & OP_ONCE) mem[op] = 0;
      return true;
    }
  }
  /* Not done: count it, and take the jump if there is one. */
  (*failures)++;
  if (mem[op] & OP_JUMP_ON_FAIL) ch->current = word_at((uint16_t)(op + length));
  return false;
}

/* The character, which is inside something: if that is a dead thing or a
 * character, it acts as usual (true); otherwise, if it is open, it climbs
 * out; and that is its turn (false). */
static bool inside_something(uint16_t record) {
  uint8_t container = object_at(record)->container;
  mem[V_OBJECT2] = OBJECT_NONE;
  mem[V_OBJECT1] = container;
  uint8_t attrs = object_at(locate_object(container, NULL))->attrs;
  if (attrs & (ATTR_BROKEN | ATTR_ANIMATE)) return true;
  if (attrs & ATTR_OPEN) {
    mem[V_ACTION] = ACTION_CLIMB_OUT;
    character_tries();
  }
  return false;
}

/* $980E: each character in the table takes its turn, running its script
 * until it does something, or has failed six times. If the player is in
 * the dark, the first character the player could see says "you hear a
 * noise" (once). */
void characters_act(void) {
  note_your_location();
  for (uint16_t at = CHARACTER_TABLE; mem[at] != 0xFF; at += 7) {
    CharacterRecord *ch = character_at(at);
    uint8_t failures = 0;
    if (ch->id == 0) continue;
    mem[V_ACTOR] = ch->id;
    uint16_t record;
    mem[V_ACTOR_LOCATION] = first_location(ch->id, &record);
    set_word_at(V_ACTOR_RECORD, record);
    mem[V_OUTPUT_ON] = 0;
    if (can_see(object_at(PLAYER), ch->id, object_at(record)) && mem[V_DARKNESS] != 2) {
      mem[V_OUTPUT_ON] = 1;
      if (mem[V_DARKNESS] == 1) {
        mem[V_DARKNESS] = 2;
        print_message(MSG_YOU_HEAR, NULL);
        mem[V_OUTPUT_ON] = 0;
      }
    }
    if (object_at(record)->container != OBJECT_NONE && !inside_something(record)) continue;
    mem[V_HAS_ORDERS] = has_orders();
    while (failures != CHARACTER_LIMIT)
      if (script_step(at, ch->current, &failures)) break;
  }
  mem[V_ACTOR] = YOU;
  mem[V_OUTPUT_ON] = 1;
  set_word_at(V_ACTOR_RECORD, PLAYER);
}

/* ========== saying things, climbing in and out ($A1E3, $A541-$A5D0) ========== */

/* $A1E3: 'You say "message".' (with the message's arguments, if any) */
void say_aloud(uint16_t message, const uint16_t *args) {
  print_message(MSG_YOU_SAY, NULL);
  mem[V_LAST_KEY] = 1;
  print_message(message, args);
  print_message(MSG_END_QUOTE, NULL);
}

/* $A5CA: is the object open? */
bool object_is_open(uint16_t record) { return object_at(record)->attrs & ATTR_OPEN; }

/* $A541 (action $37): the actor climbs out of the first object, if it is
 * in it, and it is open. */
void climb_out(void) {
  ObjectRecord *actor = object_at(word_at(V_ACTOR_RECORD));
  if (mem[V_OBJECT1] != actor->container) {
    finish_action();
    return;
  }
  uint16_t record = locate_object(mem[V_OBJECT1], NULL);
  if (!object_is_open(record)) {
    say_object_is(record, 0x05);
    return;
  }
  if (only_trying()) return;
  actor->container = OBJECT_NONE;
}

/* $A55F (action $36): the actor climbs into the first object, if it can
 * see it, it is open, and the actor fits. If the actor is carrying it (at
 * any depth), it is put down first. The size test is the original's,
 * which almost always lets the actor in: the room needed is $FF unless
 * the actor's size and what it carries come to more than $FF, and only
 * an object of size $FF (none) or of at least that size refuses. */
void climb_in(void) {
  uint16_t actor_record = word_at(V_ACTOR_RECORD);
  if (mem[V_OBJECT1] == object_at(actor_record)->container) {
    finish_action();
    return;
  }
  if (!actor_can_see(mem[V_OBJECT1], object_at(word_at(V_OBJECT1_RECORD)))) {
    finish_action();
    return;
  }
  for (uint8_t id = mem[V_OBJECT1];;) {
    uint8_t container = object_at(locate_object(id, NULL))->container;
    if (container == OBJECT_NONE) break;
    if (container == mem[V_ACTOR]) {
      object_at(word_at(V_OBJECT1_RECORD))->container = OBJECT_NONE;
      break;
    }
    id = container;
  }
  actor_record = word_at(V_ACTOR_RECORD);
  unsigned total = contents_size(mem[V_ACTOR]) + object_at(actor_record)->size;
  uint8_t needed = total > 0xFF ? (uint8_t)total : 0xFF;
  uint16_t record = word_at(V_OBJECT1_RECORD);
  if (!object_is_open(record)) {
    say_object_is(record, 0x05);
    return;
  }
  uint8_t size = object_at(record)->size;
  if (size != 0xFF && size >= needed) {
    print_message(0xB13E, NULL);
    return;
  }
  if (only_trying()) return;
  object_at(actor_record)->container = mem[V_OBJECT1];
}

/* $8EF8 (the rivers, action $19, look across; the second half of $8EEC in
 * actions1): look at the place beyond the first object, if the actor is
 * not shut in something, there is a way through it, and it is lit. */
void look_across(void) {
  if (inside_closed(mem[V_ACTOR])) return;
  uint16_t exit;
  bool found = exit_through_door(mem[V_OBJECT1], &exit);
  if (!found || mem[(uint16_t)(exit + 2)] == 0) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  uint8_t beyond = mem[(uint16_t)(exit + 2)];
  if (!(location_at(locate_location(beyond))->attrs & LOC_LIT)) {
    print_message(MSG_DARK, NULL);
    return;
  }
  ObjectRecord *actor = object_at(word_at(V_ACTOR_RECORD));
  uint8_t here = actor->locations[0];
  actor->locations[0] = beyond;
  describe_location_you_see(beyond, 0);
  actor->locations[0] = here;
}

/* ========== the characters' and places' events ($A5D1-$A7C3) ========== */

#define YOUR_PLACE (you()->locations[0])

/* $A5D1 (the warg's script): it howls, if it is where you are. */
void warg_howls(void) {
  if (only_trying()) return;
  if (YOUR_PLACE != mem[0xC452]) return; /* the warg's location */
  say_sentence(0xB085); /* "the vicious warg run around you and howls" */
}

/* $A5E2 (the barrel, every turn): once the barrel is in the forest river
 * ($21), it reaches the bank in two turns (the timer at $CA85). */
void barrel_in_river(void) {
  if (mem[V_OBJECT1] != BARREL) return;
  if (!action_confirmed()) return;
  if (mem[0xC3FE] != 0x21) return; /* the barrel's location */
  mem[0xCA85] = 2;
}

/* $A5FB (timed): the barrel is washed onto the bank of the long lake (you
 * with it, if you are in it), and is left closed and full of the river's
 * water (object $14). */
void barrel_ashore(void) {
  mem[V_TIMED_FIRED] = 0;
  if (you()->container == BARREL) print_message(0xB32C, NULL); /* "you are thrown onto the bank..." */
  mem[0xC3FE] = 0x22;
  object_moved(BARREL, 0x22, 0);
  ObjectRecord *barrel = object_at(0xC3EE);
  barrel->locations[0] = 0x20;
  barrel->attrs &= (uint8_t)~ATTR_OPEN;
  barrel->attrs |= ATTR_FULL;
  mem[V_OUTPUT_ON] = 0;
  empty_object(BARREL);
  mem[V_OUTPUT_ON] = 1;
  ObjectRecord *water = object_at(0xC418);
  water->locations[0] = 0x20;
  water->container = BARREL;
}

/* $A640 (script): "where's the thief ?", when the character is where you
 * are and you are invisible. */
void wheres_the_thief(void) {
  if (mem[V_ACTOR_LOCATION] != mem[V_YOUR_LOCATION]) return;
  if (you()->attrs & ATTR_VISIBLE) return;
  if (only_trying()) return;
  say_aloud(0xB0CB, NULL);
}

/* $A657 (Thorin's script): at random, he waits, sits down and sings about
 * gold, or says "hurry up.". */
void thorin_talks(void) {
  if (only_trying()) return;
  uint8_t n = random_upto(8);
  if (n >= 5) return;
  if (n >= 3)
    say_sentence(0xB0F4); /* "thorin wait." */
  else if (n == 0)
    say_sentence(0xB0E5); /* "thorin sits down and starts singing about gold." */
  else
    say_aloud(0xB05B, NULL); /* "hurry up." ("get us out of this one, thief !", $B0D5, is
                        * for n == 3, which the original never reaches) */
}

/* $A67E (actions $10 and $0C on an object out of reach): only from the
 * cellar ($20); then open ($910E) or close ($9138) it. */
void reach_from_cellar(void) {
  uint16_t actor = word_at(V_ACTOR_RECORD);
  if (object_at(actor)->locations[0] != 0x20) {
    print_message(0xB11B, NULL); /* "you cannot reach" */
    return;
  }
  if (mem[V_ACTION] == 0x0C) close_object(); else open_object();
}

/* $A698 (script): the character ($3C) follows you into locations $27,
 * $2C and $29: "<it> enters." */
void follows_you_in(void) {
  uint8_t place = YOUR_PLACE;
  if (place != 0x27 && place != 0x2C && place != 0x29) return;
  if (only_trying()) return;
  place = YOUR_PLACE;
  if (mem[0xC143] == place) return; /* its location */
  mem[0xC143] = place;
  mem[V_OUTPUT_ON] = 1;
  uint16_t name = 0xC13B; /* its name */
  print_message(MSG_ENTERS, &name);
}

/* $A6C2 (the dragon's script): where you are, it threatens you. */
void dragon_talks(void) {
  if (YOUR_PLACE != mem[V_ACTOR_LOCATION]) return;
  if (only_trying()) return;
  print_message(you()->attrs & ATTR_VISIBLE ? 0xB15D : 0xB17E, NULL);
}

/* $A6DC (the dragon's script): unless it is at its lair ($29), it may burn
 * you where you are, if the place is lit. */
void dragon_burns(void) {
  if (mem[0xC5DD] == 0x29) return; /* the dragon's location */
  if (!(location_at(locate_location(YOUR_PLACE))->attrs & LOC_LIT)) return;
  if (only_trying()) return;
  mem[V_OUTPUT_ON] = 1;
  if (random_upto(0x64) < 0x50) {
    print_message(0xB1A6, NULL);
    return;
  }
  print_message(0xB1BA, NULL);
  you_are_dead();
}

/* $A71E (action $1C, read): if the actor is visible, $AFF4; otherwise
 * $B2AA, and the timer at $CAA8 starts. */
void read_runes(void) {
  if (only_trying()) return;
  if (object_at(word_at(V_ACTOR_RECORD))->attrs & ATTR_VISIBLE) {
    print_message(0xAFF4, NULL);
    return;
  }
  mem[0xCAA8] = mem[0xCAA7];
  print_message(0xB2AA, NULL);
}

/* $A73B (every turn): once the object at $C3B2 is broken, object 2 (at
 * $C1AD) breaks too, and is reset; $B109 if you can see it. */
void breaks_too(void) {
  if (!(object_at(0xC3B2)->attrs & ATTR_BROKEN)) return;
  if (!action_confirmed()) return;
  object_at(0xC1AD)->attrs |= ATTR_BROKEN;
  reset_object(0x02);
  if (!can_see(object_at(PLAYER), 0x02, object_at(0xC1AD))) return;
  print_message(0xB109, NULL);
}

/* $A761 (actions $0C, $10 on an object out of reach): the player cannot
 * reach it ("you cannot reach" unless inside something); others can open
 * ($910E) or close ($9138) it. */
void reach_from_inside(void) {
  if (mem[V_ACTOR] == YOU) {
    if (you()->container == OBJECT_NONE)
      print_message(0xB11B, NULL);
    else
      finish_action();
    return;
  }
  if (mem[V_ACTION] == 0x0C)
    close_object();
  else if (mem[V_ACTION] == 0x10)
    open_object();
}

/* $A784 (actions $1E, $0B, $18 on an object out of reach): the player only
 * from inside something; then go through it ($8F3B), ... ($92ED), look
 * through it ($8EEC). */
void reach_from_inside2(void) {
  if (mem[V_ACTOR] == YOU && you()->container == OBJECT_NONE) {
    print_message(0xB11B, NULL);
    return;
  }
  uint8_t action = mem[V_ACTION];
  if (action == 0x1E)
    go_through();
  else if (action == 0x0B)
    break_object();
  else if (action == 0x18)
    look_through();
}

/* $A7AA (timed): sinking in the bog: $B153, and you die unless the timer
 * $CAA1 is still running (it is stopped if you are still in the bog,
 * $1D). */
void bog_sinks(void) {
  if (YOUR_PLACE == 0x1D) mem[0xCAA1] = 0;
  print_message(0xB153, NULL);
  if (mem[0xCAA1] == 0) you_are_dead();
}

/* $A7C4 (the map, read): for Thorin's map ($41's?), the clue chosen at the
 * start: "<direction> go from <place> to get to <place>"; its three bytes
 * are written into the message the first time (while V_MAP_SHOWN is 0).
 * Others: examine. */
void read_map(void) {
  if (mem[V_ACTOR] != 0x41) {
    examine();
    return;
  }
  if (only_trying()) return;
  uint16_t clue = word_at(V_MAP_RECORD), text = word_at((uint16_t)(clue + 1));
  if (mem[V_MAP_SHOWN] == 0)
    for (int i = 0; i < 3; i++) mem[(uint16_t)(text + i)] = mem[(uint16_t)(clue + 3 + i)];
  uint16_t to = (uint16_t)(locate_location(mem[(uint16_t)(clue + 5)]) + 2);
  uint16_t from = (uint16_t)(locate_location(mem[clue]) + 2);
  uint16_t args[3] = {direction_word(mem[(uint16_t)(clue + 3)]), from, to};
  say_aloud(0xB1D0, args);
}

/* ========== events ($A814-$A9D5) ========== */

/* $A86E: a fifty-fifty chance. */
bool chance_half(void) { return random_upto(0x64) < 0x32; }

/* $A814 (action $2B, shooting at the second object): if it is across the
 * river where the boat is ($C62B), the boat may be hit loose ($C61C =
 * $12); otherwise the first object may land beyond it. */
void shoot_at(void) {
  uint16_t exit;
  bool found = exit_through_door(mem[V_OBJECT2], &exit);
  if (!found) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  print_message(0xAF66, NULL);
  uint8_t beyond = mem[(uint16_t)(exit + 2)];
  uint16_t message;
  if (mem[0xC62B] == beyond) {
    if (chance_half()) {
      mem[0xC61C] = 0x12;
      message = 0xAF8B;
    } else {
      message = chance_half() ? 0xAF76 : 0xAF82;
    }
  } else if (chance_half()) {
    message = 0xAF76;
  } else {
    uint16_t record = word_at(V_OBJECT1_RECORD);
    object_at(record)->locations[0] = beyond;
    object_at(record)->container = OBJECT_NONE;
    object_moved(mem[V_OBJECT1], beyond, 0);
    message = 0xAF6D;
  }
  print_message(message, NULL);
}

/* $A882: the message, then the boat (object $29) goes to the other side
 * ($42 or $43), and is no longer loose. */
static void boat_crosses(uint16_t message) {
  print_message(message, NULL);
  uint8_t side = mem[0xC62B] == 0x42 ? 0x43 : 0x42;
  mem[0xC62B] = side;
  mem[0xC61C] = 0xFF;
  object_moved(0x29, side, 0);
}

/* $A876 (action $31, pulling the boat): if it is loose, it comes over. */
void swing_back(void) {
  if (only_trying()) return;
  if (mem[0xC61C] != 0x12) return;
  boat_crosses(0xAF92);
}

/* $A89E (every turn): for you, the boat comes over ($AFA4). */
void swing_arrives(void) {
  if (!action_confirmed()) return;
  if (mem[V_ACTOR] != YOU) return;
  boat_crosses(0xAFA4);
}

/* $A8AB (Gollum's script, tried then done): when the character has been
 * told something, it takes it as its next op (the op at $C9E2 becomes an
 * action, $42, with what was said and its objects); and stops there. */
void listens(void) {
  if (mem[V_HAS_ORDERS] != 1) return;
  mem[V_HAS_ORDERS] = 0;
  if (!obeys()) return;
  mem[V_DONE] = 0;
  mem[0xC9E3] = mem[V_SAID];
  mem[0xC9E4] = mem[V_OBJECT1];
  mem[0xC9E5] = mem[V_OBJECT2];
  mem[0xC9E2] = 0x42;
}

/* $A8D2 (Gollum's script): where Gollum is ($C4C1) and you are visible,
 * he asks his riddle (the riddle record's message), and waits for the
 * answer. */
void asks_riddle(void) {
  if (mem[0xC4C1] != mem[V_YOUR_LOCATION]) return;
  if (!(you()->attrs & ATTR_VISIBLE)) return;
  if (only_trying()) return;
  say_aloud(word_at((uint16_t)(word_at(V_RIDDLE) + 2)), NULL);
  mem[V_RIDDLE_ASKED] = 1;
}

/* $A8F6 (Gollum's script, tried then done): unless you answered the riddle
 * (its first two bytes, found next to each other in what you said),
 * someone strangles you from behind. The search is the original's CPIR,
 * $18 bytes, that goes on after a match whose next byte is wrong. */
void strangles(void) {
  if (only_trying()) return;
  mem[V_RIDDLE_ASKED] = 0;
  uint16_t said;
  if (obey_orders(false, &said)) {
    uint16_t answer = word_at(V_RIDDLE), count = 0x18;
    for (;;) {
      bool match = false;
      do {
        match = mem[said++] == mem[answer];
        count--;
      } while (count != 0 && !match);
      if (!match) break;
      if (mem[(uint16_t)(answer + 1)] == mem[said]) return;
    }
  }
  if (only_trying()) return;
  mem[V_OUTPUT_ON] = 1;
  print_message(0xB1DB, NULL); /* "someone strangles you from behind" */
  you_are_dead();
}

/* $A926 (script): where Gollum is ($C4C1), half the time the character
 * says $B2D9, otherwise $B2E7 unless the object at $C31A is in object $44. */
void says_something(void) {
  if (mem[0xC4C1] != mem[V_YOUR_LOCATION]) return;
  if (only_trying()) return;
  bool negative = random_spread(8) < 0; /* the carry flag of $9C9F */
  if (!negative || mem[0xC31A + 1] == 0x44)
    say_aloud(0xB2D9, NULL);
  else
    say_aloud(0xB2E7, NULL);
}

/* $A94E (the trolls' script): where you are, they eat you. */
void trolls_eat(void) {
  if (mem[V_ACTOR_LOCATION] != mem[V_YOUR_LOCATION]) return;
  if (only_trying()) return;
  mem[V_ACTION] = ACTION_EAT_YOU;
  mem[V_OBJECT1] = YOU;
  mem[V_OBJECT2] = OBJECT_NONE;
  finish_action(); /* (doing: prints the sentence) */
  eat_food();
  you_are_dead();
}

/* $A971 (the trolls' script): day dawns, and the trolls ($47, $48) turn to
 * stone: they die and cannot be seen, the clearing ($05) gets its new
 * description and picture colour, and is new again. */
void day_dawns(void) {
  if (only_trying()) return;
  kill(0x47);
  kill(0x48);
  mem[0xC646] &= (uint8_t)~ATTR_VISIBLE;
  mem[0xC658] &= (uint8_t)~ATTR_VISIBLE;
  set_word_at(0xBAC4, 0xB262); /* "in a clearing with two stone trolls." */
  mem[0xBABC] &= (uint8_t)~LOC_SCORED;
  empty_object(0x47);
  empty_object(0x48);
  mem[V_OUTPUT_ON] = 1;
  print_message(0xB30B, NULL); /* "day dawns" */
  uint16_t picture = word_at((uint16_t)(find_in_table(PICTURES, 0x05, NULL) + 1));
  mem[picture] = 0x05;
  mem[(uint16_t)(picture + 1)] = 0x28;
}

/* $A9BD (the trolls' script): in the clearing, a troll speaks. */
void trolls_talk(void) {
  if (mem[V_YOUR_LOCATION] != 0x05) return;
  if (only_trying()) return;
  say_aloud(mem[V_ACTOR] == 0x47 ? 0xB238 : 0xB24C, NULL);
}

/* $A9D6 (every turn): when the treasure ($C5CE) is home ($25), the game is
 * won: $B3D9, a key, and a new game. */
void check_game_won(void) {
  if (mem[0xC5CE] != 0x25) return;
  print_message(0xB3D9, NULL);
  wait_and_restart();
}

/* ========== events ($A9E5-$AB52) ========== */

/* $A9E5 (script): where you are, if the object at $C663 is nowhere or
 * with character $41, the character gives it to you (action $1D on
 * object $26). */
void gives_you(void) {
  if (mem[V_ACTOR_LOCATION] != mem[V_YOUR_LOCATION]) return;
  ObjectRecord *gift = object_at(0xC663);
  if (gift->locations[0] != 0 && gift->container != 0x41) return;
  if (only_trying()) return;
  gift->locations[0] = mem[V_ACTOR_LOCATION];
  gift->container = 0x41;
  mem[V_OBJECT1] = 0x26;
  mem[V_OBJECT2] = YOU;
  mem[V_ACTION] = ACTION_GIVE;
  set_word_at(V_OBJECT1_RECORD, 0xC663);
  set_word_at(V_OBJECT2_RECORD, PLAYER);
  finish_action(); /* (doing: prints the sentence) */
  action_give();
}

/* $AA27 (action $38, going into an object): the actor goes into it, and
 * through to where it leads. If its exit there is not a "down" exit ($0A),
 * the original jumps to $B301, which is message text, as code. */
void go_into(void) {
  uint8_t place = object_at(word_at(V_OBJECT1_RECORD))->locations[0];
  uint16_t exit;
  bool found = exit_to(place, &exit);
  if (!found) {
    print_message(0xB301, NULL);
    return;
  }
  if (mem[exit] != DIR_DOWN) {
    /* The same message. (The original's bug jumps to it as code.) */
    if (original_bugs) device_crash(0xB301);
    print_message(0xB301, NULL);
    return;
  }
  if (only_trying()) return;
  object_at(word_at(V_ACTOR_RECORD))->container = mem[V_OBJECT1];
  object_moved(mem[V_OBJECT1], place, 0);
  if (mem[V_ACTOR] != YOU) return;
  describe_location(place, 0);
}

/* $AA5C (timed): the object at $C205 mends: not broken, not open, twice
 * as strong, and its +$0A word becomes $0623. */
void mends(void) {
  ObjectRecord *thing = object_at(0xC205);
  thing->attrs &= (uint8_t)~(ATTR_BROKEN | ATTR_OPEN);
  thing->strength = (uint8_t)(thing->strength << 1);
  set_word_at(0xC20F, 0x0623);
}

#define HOLE 0xC2B5 /* object $0B, the hole by the side door */

/* $AA74 (timed): unless the hole is open, it vanishes (and its timer
 * starts again); "the hole vanishes" if you are there ($2A). */
void hole_vanishes(void) {
  if (object_at(HOLE)->attrs & ATTR_OPEN) return;
  mem[0xCAC4] = mem[0xCAC3];
  object_at(HOLE)->attrs &= (uint8_t)~ATTR_VISIBLE;
  if (YOUR_PLACE != 0x2A) return;
  print_message(0xB2A4, NULL);
}

/* $AA91 (timed): the hole appears. */
void hole_appears(void) {
  object_at(HOLE)->attrs |= ATTR_VISIBLE;
  if (YOUR_PLACE == 0x2A) print_message(0xB277, NULL);
}

/* $AAA2 (the hole, every turn): it closes: locked, invisible, and its
 * timer set to 6. */
void hole_closes(void) {
  if (!action_confirmed()) return;
  mem[0xCAC4] = 0x06;
  object_at(HOLE)->attrs |= ATTR_LOCKED;
  object_at(HOLE)->attrs &= (uint8_t)~ATTR_VISIBLE;
  if (YOUR_PLACE == 0x2A) print_message(0xB2A4, NULL);
}

#define MAGIC_DOOR 0xC381

/* $AAC7: the message, if you are at either side of the magic door ($1E,
 * $1C). */
void say_at_door(uint16_t message) {
  if (YOUR_PLACE == 0x1E || YOUR_PLACE == 0x1C) print_message(message, NULL);
}

/* $AAE0 (timed, and when the door opens): if the object at $C31A is
 * inside something, that becomes the actor, and unless it is visible it
 * ... ($A3BC). */
void door_lets_out(void) {
  uint8_t container = object_at(0xC31A)->container;
  if (container == OBJECT_NONE) return;
  uint16_t record = locate_object(container, NULL);
  set_word_at(V_ACTOR_RECORD, record);
  if (!(object_at(record)->attrs & ATTR_VISIBLE)) take_off_ring();
}

/* $AAB3 (timed): the magic door opens. */
void door_opens(void) {
  object_at(MAGIC_DOOR)->attrs |= ATTR_OPEN;
  say_at_door(0xB037); /* "the magic door opens." */
  say_at_door(0xB113);
  door_lets_out();
}

/* $AAD5 (timed): the magic door closes. */
void door_closes(void) {
  object_at(MAGIC_DOOR)->attrs &= (uint8_t)~ATTR_OPEN;
  say_at_door(0xB03F); /* "the magic door closes." */
}

/* $AAF9 (every turn): for you, set $B700 and start the timer of $AB0B. */
void starts_timer(void) {
  if (mem[V_ACTOR] != YOU) return;
  mem[V_TIMER_RUNNING] = 1;
  mem[0xCAB6] = mem[0xCAB5];
}

/* $AB0B (timed): clear $B700. */
void timer_ends(void) { mem[V_TIMER_RUNNING] = 0; }

/* $AB10 (timed): among the spider threads ($1A), the web smothers you. */
void web_smothers(void) {
  if (YOUR_PLACE != 0x1A) return;
  print_message(0xB0FD, NULL);
  you_are_dead();
}

/* Something drops from above and stings: you are dead. */
static void stung(void) {
  print_message(0xB31F, NULL);
  you_are_dead();
}

/* $AB1F (timed, below the threshold): pale bulbous eyes stare at you;
 * something stings you unless you stay where you came into the forest, or
 * move on to the other forest place (2 or 3). */
void eyes_stare(void) {
  print_message(0xB311, NULL);
  uint8_t here = YOUR_PLACE, came = mem[V_FOREST_ENTRY];
  if (here == came) return;
  if (here == (came == 0x02 ? 0x03 : 0x02)) return;
  stung();
}

/* $AB3A (timed): in the forest (2, 3), the eyes, and the sting. */
void something_stings(void) {
  if (YOUR_PLACE != 0x02 && YOUR_PLACE != 0x03) return;
  print_message(0xB311, NULL);
  stung();
}

/* ========== the location events ($C7A4-$C7FB, in the data area) ==========
 * Run from the table at $C78E when you arrive at the location. */

/* $C7A4 (Beorn's house, $16): unless the butler ($42) is dead, he
 * appears and his script starts. */
void at_beorns_house(void) {
  if (mem[0xC437] & ATTR_BROKEN) return;
  mem[0xCAE7] = 0x42;
  mem[0xC437] |= ATTR_VISIBLE;
}

/* $C7B2 (the spider threads, $1A): start the web's timer. */
void at_spider_threads(void) { mem[0xCA93] = mem[0xCA92]; }

/* $C7B9 (the deep bog, $1D): start the bog's timer. */
void at_deep_bog(void) { mem[0xCAA1] = mem[0xCAA0]; }

/* $C7C0 (the elvenking's cellar, $20): start the side door's timer (3),
 * and the scripts of the dragon ($3C) and Bard ($46) unless they are
 * dead. */
void at_elvenkings_cellar(void) {
  mem[0xCAC4] = 0x03;
  if (!(mem[0xC13A] & ATTR_BROKEN)) mem[0xCB03] = 0x3C;
  if (!(mem[0xC4CA] & ATTR_BROKEN)) mem[0xCAFC] = 0x46;
}

/* $C7DD (the forest, $02 and $03): remember where you came in, and start
 * the eyes' timer. */
void at_forest(void) {
  mem[V_FOREST_ENTRY] = mem[V_ENTERING];
  mem[0xCABD] = mem[0xCABC];
}

/* $C7EA (the forest river, $21): unless you are in the barrel, you are
 * swept against the rocks and die (after the usual arrival, $8E39, for
 * the location you were moved to, which the original has in B). */
void at_forest_river(uint8_t location) {
  if (you()->container == BARREL) return;
  arrive(location, find_entry(LOCATION_EVENTS, location)); /* (IX: its event's entry) */
  print_message(0xB26D, NULL); /* "You are swept forcefully against the..." */
  you_are_dead();
}

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ==================== adapters (for the faithful callers, while they remain) ==================== */

/* Each adapter puts back the registers it was given (all but SP), then
 * sets its results. */
static void put_back(Cpu *c, const Cpu *entry) {
  uint16_t sp = c->sp;
  *c = *entry;
  c->sp = sp;
}
#define RUN(stmt) do { Cpu entry_ = *c; stmt; put_back(c, &entry_); } while (0)

/* Handlers reached through $9B6C (which keeps IX IY DE BC HL around
 * them), or by a JP from one: their callers use no registers. */
#define HANDLER(fn) static void a_##fn(Cpu *c) { RUN(fn()); }
HANDLER(examine) HANDLER(empty_out) HANDLER(river_sweeps_away) HANDLER(lock_object)
HANDLER(unlock_object) HANDLER(send_through_door) HANDLER(kill_first_object) HANDLER(climb_out)
HANDLER(climb_in) HANDLER(look_across) HANDLER(warg_howls) HANDLER(barrel_in_river)
HANDLER(barrel_ashore) HANDLER(wheres_the_thief) HANDLER(thorin_talks) HANDLER(reach_from_cellar)
HANDLER(follows_you_in) HANDLER(dragon_talks) HANDLER(dragon_burns) HANDLER(read_runes)
HANDLER(breaks_too) HANDLER(reach_from_inside) HANDLER(reach_from_inside2) HANDLER(bog_sinks)
HANDLER(read_map) HANDLER(shoot_at) HANDLER(swing_back) HANDLER(swing_arrives) HANDLER(listens)
HANDLER(asks_riddle) HANDLER(strangles) HANDLER(says_something) HANDLER(trolls_eat)
HANDLER(day_dawns) HANDLER(trolls_talk) HANDLER(check_game_won) HANDLER(gives_you)
HANDLER(go_into) HANDLER(mends) HANDLER(hole_vanishes) HANDLER(hole_appears)
HANDLER(hole_closes) HANDLER(door_opens) HANDLER(door_closes) HANDLER(door_lets_out)
HANDLER(starts_timer) HANDLER(timer_ends) HANDLER(web_smothers) HANDLER(eyes_stare)
HANDLER(something_stings) HANDLER(at_beorns_house) HANDLER(at_spider_threads)
HANDLER(at_deep_bog) HANDLER(at_elvenkings_cellar) HANDLER(at_forest)
HANDLER(end_of_turn) HANDLER(characters_act) HANDLER(new_game_characters) HANDLER(do_action)
HANDLER(may_try_action)
#undef HANDLER

/* $C7EA: B the location the player was moved to. */
static void a_at_forest_river(Cpu *c) { RUN(at_forest_river(c->b)); }

/* $95DF: A the object, IX its record, B the action. */
static void a_character_reacts(Cpu *c) { RUN(character_reacts(c->a, c->ix, c->b)); }

/* $95ED: carry set if dark, and then HL = "it is dark" (CALL C,$72DD). */
static void a_player_in_dark(Cpu *c) {
  bool dark;
  RUN(dark = player_in_dark());
  c->cf = dark;
  if (dark) set_hl(c, MSG_DARK);
}

/* $962B, $9630, $964D (HL the message), $965B: A the location. */
static void a_describe_you_see(Cpu *c) { RUN(describe_location_you_see(c->a, c->ix)); }
static void a_describe_location(Cpu *c) { RUN(describe_location(c->a, c->ix)); }
static void a_describe_after(Cpu *c) { RUN(describe_location_after(get_hl(c), c->a, c->ix)); }

/* $9686: Z: the words of the location at IX; NZ: the message at HL. */
static void a_print_location_text(Cpu *c) {
  RUN(if (c->zf) print_location_words(c->ix); else print_message(get_hl(c), NULL));
}
static void a_print_location_words(Cpu *c) { RUN(print_location_words(c->ix)); }

/* $96A8: A the location, B the one whose exits are listed. */
static void a_describe_briefly(Cpu *c) { RUN(describe_location_briefly(c->a, c->b, c->ix)); }

/* $9728: NZ if it said so. */
static void a_say_who_carries(Cpu *c) {
  bool said;
  RUN(said = say_who_carries(c->a));
  c->zf = !said;
}

/* $977F: A the one who dies; keeps IX IY BC. */
static void a_kill(Cpu *c) { RUN(kill(c->a)); }

/* $97F4, $A1E3: HL the message. */
static void a_say_sentence(Cpu *c) { RUN(say_sentence(get_hl(c))); }
static void a_say_aloud(Cpu *c) {
  /* The message's arguments: what the caller left on the stack (the
   * original's message takes them from there). */
  uint16_t args[4];
  for (int i = 0; i < 4; i++) args[i] = word_at((uint16_t)(c->sp + 2 * i));
  RUN(say_aloud(get_hl(c), args));
}

/* $97FF: unless confirmed, return from the caller too: the original pops
 * the caller's return address into BC (A = V_DOING & V_DONE). */
static void a_action_confirmed(Cpu *c) {
  c->a = mem[V_DOING] & mem[V_DONE];
  c->zf = !action_confirmed();
  if (c->zf) set_bc(c, pop16(c));
}

/* $9918: IY the character, IX the op (whose bit 4 is tested), DE its
 * length from HL (the callers have HL = IX; the original adds DE to HL,
 * so the length is taken from there); HL out. */
static void a_script_skip(Cpu *c) {
  uint16_t length = (uint16_t)(get_hl(c) - c->ix + get_de(c));
  set_hl(c, script_skip(c->iy, c->ix, length));
}

/* $99C6: NZ if done; keeps IX. */
static void a_character_tries(Cpu *c) {
  bool done;
  RUN(done = character_tries());
  c->zf = !done;
}

/* $9A59: IY the character, IX the op. $9A68: E the entry. */
static void a_script_random(Cpu *c) { RUN(script_random(c->iy, c->ix)); }
static void a_script_choose(Cpu *c) { script_choose(c->iy, c->e); }

/* $A5CA: Z if the object at IX is not open; A = 5 (the refusal). */
static void a_object_is_open(Cpu *c) {
  c->zf = !object_is_open(c->ix);
  c->a = 0x05;
}

/* $A86E: carry half the time. */
static void a_chance_half(Cpu *c) {
  bool yes;
  RUN(yes = chance_half());
  c->cf = yes;
}

/* $AAC7: HL the message. */
static void a_say_at_door(Cpu *c) { RUN(say_at_door(get_hl(c))); }

#define KEEPS_ALL (OUT_BC | OUT_DE | OUT_HL | OUT_IX | OUT_IY)
const CleanRoutine characters_clean[] = {
    /* actions2 */
    {0x93DA, "examine", a_examine, 0},
    {0x9404, "empty_out", a_empty_out, 0},
    {0x9428, "river_sweeps_away", a_river_sweeps_away, 0},
    {0x946D, "lock_object", a_lock_object, 0},
    {0x948D, "unlock_object", a_unlock_object, 0},
    {0x94A4, "send_through_door", a_send_through_door, 0},
    {0x94D6, "may_try_action", a_may_try_action, OUT_IX | OUT_HL},
    {0x950F, "do_action", a_do_action, OUT_HL | OUT_IX | OUT_BC},
    {0x95DF, "character_reacts", a_character_reacts, 0},
    {0x95ED, "player_in_dark", a_player_in_dark, OUT_CF | KEEPS_ALL},
    {0x962B, "describe_location_you_see", a_describe_you_see, OUT_IX | OUT_IY | OUT_BC},
    {0x9630, "describe_location", a_describe_location, OUT_IX | OUT_IY | OUT_BC},
    {0x964D, "describe_location_after", a_describe_after, OUT_IX | OUT_IY | OUT_BC},
    {0x965B, "describe_location_after", a_describe_after, 0},
    {0x9686, "print_location_text", a_print_location_text, 0},
    {0x9689, "print_location_words", a_print_location_words, OUT_IY},
    {0x96A8, "describe_location_briefly", a_describe_briefly, 0},
    {0x96B3, "end_of_turn", a_end_of_turn, KEEPS_ALL},
    {0x9728, "say_who_carries", a_say_who_carries, OUT_ZF | OUT_IX | OUT_IY | OUT_BC},
    {0x977C, "kill_first_object", a_kill_first_object, 0},
    {0x977F, "kill", a_kill, OUT_IX | OUT_IY | OUT_BC},
    {0x97AD, "new_game_characters", a_new_game_characters, 0},
    {0x97F4, "say_sentence", a_say_sentence, 0},
    {0x97FF, "action_confirmed", a_action_confirmed, OUT_A | OUT_ZF | OUT_BC},
    {0x980E, "characters_act", a_characters_act, 0},
    {0x9918, "script_skip", a_script_skip, OUT_HL | OUT_DE | OUT_IX | OUT_IY},
    {0x99C6, "character_tries", a_character_tries, OUT_ZF | OUT_IX},
    {0x9A59, "script_random", a_script_random, OUT_IX | OUT_IY},
    {0x9A68, "script_choose", a_script_choose, OUT_IX | OUT_IY},
    /* events */
    {0xA1E3, "say_aloud", a_say_aloud, 0},
    {0xA541, "climb_out", a_climb_out, 0},
    {0xA55F, "climb_in", a_climb_in, 0},
    {0xA5CA, "object_is_open", a_object_is_open, OUT_A | OUT_ZF},
    {0xA5D1, "warg_howls", a_warg_howls, 0},
    {0xA5E2, "barrel_in_river", a_barrel_in_river, 0},
    {0xA5FB, "barrel_ashore", a_barrel_ashore, 0},
    {0xA640, "wheres_the_thief", a_wheres_the_thief, 0},
    {0xA657, "thorin_talks", a_thorin_talks, 0},
    {0xA67E, "reach_from_cellar", a_reach_from_cellar, 0},
    {0xA698, "follows_you_in", a_follows_you_in, 0},
    {0xA6C2, "dragon_talks", a_dragon_talks, 0},
    {0xA6DC, "dragon_burns", a_dragon_burns, 0},
    {0xA71E, "read_runes", a_read_runes, 0},
    {0xA73B, "breaks_too", a_breaks_too, 0},
    {0xA761, "reach_from_inside", a_reach_from_inside, 0},
    {0xA784, "reach_from_inside2", a_reach_from_inside2, 0},
    {0xA7AA, "bog_sinks", a_bog_sinks, 0},
    {0xA7C4, "read_map", a_read_map, 0},
    {0xA814, "shoot_at", a_shoot_at, 0},
    {0xA86E, "chance_half", a_chance_half, OUT_CF},
    {0xA876, "swing_back", a_swing_back, 0},
    {0xA89E, "swing_arrives", a_swing_arrives, 0},
    {0xA8AB, "listens", a_listens, 0},
    {0xA8D2, "asks_riddle", a_asks_riddle, 0},
    {0xA8F6, "strangles", a_strangles, 0},
    {0xA926, "says_something", a_says_something, 0},
    {0xA94E, "trolls_eat", a_trolls_eat, 0},
    {0xA971, "day_dawns", a_day_dawns, 0},
    {0xA9BD, "trolls_talk", a_trolls_talk, 0},
    {0xA9D6, "check_game_won", a_check_game_won, 0},
    {0xA9E5, "gives_you", a_gives_you, 0},
    {0xAA27, "go_into", a_go_into, 0},
    {0xAA5C, "mends", a_mends, 0},
    {0xAA74, "hole_vanishes", a_hole_vanishes, 0},
    {0xAA91, "hole_appears", a_hole_appears, 0},
    {0xAAA2, "hole_closes", a_hole_closes, 0},
    {0xAAB3, "door_opens", a_door_opens, 0},
    {0xAAC7, "say_at_door", a_say_at_door, 0},
    {0xAAD5, "door_closes", a_door_closes, 0},
    {0xAAE0, "door_lets_out", a_door_lets_out, 0},
    {0xAAF9, "starts_timer", a_starts_timer, 0},
    {0xAB0B, "timer_ends", a_timer_ends, 0},
    {0xAB10, "web_smothers", a_web_smothers, 0},
    {0xAB1F, "eyes_stare", a_eyes_stare, 0},
    {0xAB3A, "something_stings", a_something_stings, 0},
    {0xC7A4, "at_beorns_house", a_at_beorns_house, 0},
    {0xC7B2, "at_spider_threads", a_at_spider_threads, 0},
    {0xC7B9, "at_deep_bog", a_at_deep_bog, 0},
    {0xC7C0, "at_elvenkings_cellar", a_at_elvenkings_cellar, 0},
    {0xC7DD, "at_forest", a_at_forest, 0},
    {0xC7EA, "at_forest_river", a_at_forest_river, 0},
    {0x8EF8, "look_across", a_look_across, 0},
    {0, NULL, NULL, 0},
};

const CleanScratch characters_scratch[] = {
    {0x980B, 0x980B, "characters_act: the actions a character failed this turn, kept in a local"},
    {0x948B, 0x948B, "lock_or_unlock: the SET/RES opcode of $9488 (only $9488 reads it), done directly"},
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
