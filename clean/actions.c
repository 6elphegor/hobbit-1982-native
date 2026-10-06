/* The clean edition: actions ($8C4B-$93D9, $A138-$A540). See
 * docs/CLEAN.md and actions.h.
 *
 * Every handler is run twice for a command: first only to try it
 * (V_DOING = 0), then to do it (V_DOING = 1). only_trying() is the
 * original's $9D44: called at the point where a handler would start
 * changing things, it notes that the action could be done and tells the
 * handler to stop there. Checks that refuse an action print why and
 * return false, and the handler stops (the original popped its caller's
 * return address). */
#include <stddef.h>

#include "actions.h"
#include "characters.h"
#include "clean.h"
#include "executor.h"
#include "game.h"
#include "handlers.h"
#include "objects.h"
#include "platform.h"
#include "rng.h"
#include "screen.h"
#include "text.h"

/* ---------- the game's data used here ---------- */

#define REC(addr) ((ObjectRecord *)&mem[addr])
#define LOCREC(addr) ((LocationRecord *)&mem[addr])

#define V_ALWAYS_ANSWER 0xB6F9 /* nonzero: characters always do as they are told */
#define V_SCORE 0xB6F7         /* the percentage, a word */
#define V_WEAPON_NAME 0xB6FC   /* the weapon's name for the attack messages */

/* The location being entered and what is moving there (kept in the code
 * area, read across the entries of the movement code). */
#define V_ENTERING_RECORD 0x8D99 /* its record */
#define V_ENTERING 0x8D9B        /* its id */
#define V_MOVER_SIZE 0x8D9C      /* the mover's size with what it carries */
#define LOCATION_SCORES 0x8D6E   /* [location][percentage word], $FF ends */
#define DAMAGE_MESSAGES 0x9226   /* attack messages, by how close the fight was */
#define ESCAPES 0xA49C           /* per goblin: id, where its id goes, location, adjective word */

enum {
  OBJ_KEY_SIDE = 0x02, OBJ_KEY_ROCK = 0x04, OBJ_KEY_RED = 0x0F, OBJ_RING = 0x10,
  OBJ_ROPE = 0x12, OBJ_WATER_A = 0x15, OBJ_WATER_B = 0x16, OBJ_POOL_A = 0x17, OBJ_POOL_B = 0x18,
  OBJ_BOW = 0x19, OBJ_ARROW = 0x1A, DRAGON = 0x3C, WOOD_ELF = 0x40, BUTLER = 0x42, BARD = 0x46,
};
#define PLAYER_RECORD 0xC11B
#define RING_RECORD 0xC31A
#define ROPE_RECORD 0xC366
#define WATER_A_RECORD 0xC454
#define WATER_B_RECORD 0xC469
#define ARROW_RECORD 0xC4EA
#define GOBLIN_DOOR_ATTRS 0xC34B
#define SPIDER_PERIOD 0xCA8B   /* the spiders' timed event: its period, */
#define SPIDER_COUNT 0xCA8C    /* and its countdown */
#define GOBLIN_DOOR_PERIOD 0xCA99 /* the goblins' door event (it closes the door) */
#define GOBLIN_DOOR_COUNT 0xCA9A
#define RING_TIMER 0xCAAF      /* turns left wearing the ring */

/* Messages. */
enum {
  MSG_YOU_ARE_IN = 0xAFFC, MSG_YOU_SEE = 0xB003, MSG_NOT_CARRYING = 0xADF1, MSG_EVAPORATES = 0xB142,
  MSG_NOT_IN_IT = 0xAFB5, MSG_TOO_HEAVY = 0xAE04, MSG_TOO_MUCH = 0xAE0C, MSG_ALREADY = 0xAE11,
  MSG_HIT_HEAD = 0xAD7D, MSG_SMASH_SKULL = 0xAD8A, MSG_TOO_SMALL = 0xAE23, MSG_TOO_FULL_ENTER = 0xAE2E,
  MSG_DARK = 0xAE1F, MSG_CANNOT_FOLLOW = 0xAFE9, MSG_SAYS_NO = 0xB1E3, MSG_NO_BOW = 0xB121,
  MSG_ARROW_MISSES = 0xB127, MSG_ARROW_HITS = 0xB136, MSG_DEAD = 0xAFF1, MSG_CARRYING = 0xADF6,
  MSG_NOTHING = 0xB33B, MSG_CANNOT_KILL = 0xAF5F, MSG_WASTED = 0xAF50, MSG_CLEAVE = 0xAE3A,
  MSG_TOO_FULL = 0xAE1A, MSG_GLUTTONY = 0xB146, MSG_IS = 0xAFBB, MSG_EXITS = 0xB020,
  MSG_ALREADY_TIED = 0xB07D, MSG_NOT_TIED = 0xB092, MSG_SWEPT_AWAY = 0xB0B4, MSG_YOU_DROWN = 0xADA9,
  MSG_POISONED = 0xB0F8, MSG_WRONG_KEY = 0xB02D, MSG_WEB_BROKEN = 0xB099, MSG_NOT_WEARING = 0xB074,
  MSG_GOBLIN_BACK = 0xB068, MSG_GOBLIN_HERE = 0xB018, MSG_SAY_A0A6 = 0xB0A6, MSG_SAY_B047 = 0xB047,
  MSG_SAY_B04B = 0xB04B, MSG_SAY_B052 = 0xB052, MSG_SAY_B05B = 0xB05B, MSG_SAY_B05F = 0xB05F,
  MSG_SAY_B061 = 0xB061,
};
#define WORDS_IS 0xA224     /* words for say_object_is, by word & $7F */
#define WORDS_IS_ALT 0xA234 /* ... when bit 7 of word is set */
#define EXIT_ACTIONS 0xA20B /* the 5 actions action_in_list looks for */
#define WORD_BARE_HANDS 0x026B
#define WORD_BROKEN 0x00CD
#define WORD_DEAD 0x0192

/* ---------- other modules ---------- */

/* At the bottom. */
static void wait_key_and_restart(void);
static void swap_objects_and_run(uint16_t code);

/* Is there an exit in direction dir from the actor's location? */
static bool has_exit_in_direction(uint8_t dir) {
  uint16_t exit;
  return find_exit_direction(dir, &exit);
}

/* A location's record; 0 for a location out of range (where the original
 * leaves its IX as it was, which here is 0). */
static uint16_t location_record_addr(uint8_t location) {
  LocationRecord *l = location_record(location);
  return l ? addr_of(l) : 0;
}

/* The actor's exit in a direction / through a door / to a location; where
 * there is none, the record is the list's end. The original answers with
 * what it looked for, or $FF for none: so looking for $FF never finds. */
static uint16_t exit_in_direction(uint8_t dir, bool *found) {
  uint16_t e;
  bool f = find_exit_direction(dir, &e);
  if (found) *found = f && dir != 0xFF;
  return e;
}
static uint16_t exit_through_door(uint8_t door, bool *found) {
  uint16_t e;
  bool f = find_exit_door(door, &e);
  if (found) *found = f && door != 0xFF;
  return e;
}
static uint16_t exit_to_location(uint8_t location, bool *found) {
  uint16_t e;
  bool f = find_exit_to(location, &e);
  if (found) *found = f && location != 0xFF;
  return e;
}

/* ---------- helpers ---------- */

/* $9D44: only trying the action? Then note that it could be done, and
 * the handler stops. */
static bool only_trying(void) {
  if (mem[V_DOING] == 1) return false;
  mem[V_DONE] = (uint8_t)(mem[V_DOING] + 1);
  return true;
}

/* $97FF: was the action done (doing it, and it could be)? */
static bool was_done(void) { return (mem[V_DOING] & mem[V_DONE]) != 0; }

static uint16_t object1_record(void) { return word_at(V_OBJECT1_RECORD); }
static uint16_t object2_record(void) { return word_at(V_OBJECT2_RECORD); }
static uint16_t actor_record(void) { return word_at(V_ACTOR_RECORD); }

/* The word (12-bit dictionary offset) of a record's name. */
static uint16_t name_word(uint16_t rec) { return REC(rec)->name[0] & 0x0FFF; }
static uint16_t name_address(uint16_t rec) { return (uint16_t)(rec + 8); }
static uint8_t location_of(uint16_t rec) { return REC(rec)->locations[0]; }

static uint8_t rotate_left(uint8_t v) { return (uint8_t)(v << 1 | v >> 7); }
static uint8_t rotate_right(uint8_t v) { return (uint8_t)(v >> 1 | v << 7); }
static uint8_t add_clamped(uint8_t a, uint8_t b) { return a + b > 0xFF ? 0xFF : (uint8_t)(a + b); }
static uint8_t halve_signed(uint8_t v) { return (uint8_t)(v >> 1 | (v & 0x80)); } /* SRA */

/* ---------- looking, taking and dropping ---------- */

/* $8C4B: describe where the actor is: the location, or what it is in or
 * on, and what is there. */
void action_look(void) {
  if (only_trying()) return;
  uint16_t actor = actor_record();
  uint8_t in = REC(actor)->container;
  if (in == OBJECT_NONE) {
    describe_location(location_of(actor), actor); /* (IX: the actor's record) */
    return;
  }
  set_word_at(MSG_YOU_ARE_IN + 1, 0x0080); /* "You are" with no preposition of its own */
  print_message(MSG_YOU_ARE_IN, NULL);
  uint16_t holder = object_record_addr(in);
  print_message(contains_phrase(object_at(holder)), NULL);
  print_token(name_word(holder));
  print_char('.');
  print_newline();
  print_message(MSG_YOU_SEE, NULL);
  list_contents(in, location_of(actor));
}

/* $8C9B */
bool check_carrying(void) {
  if (object_carried(mem[V_OBJECT1])) return true;
  print_message(MSG_NOT_CARRYING, NULL);
  return false;
}

/* $8CA6: put the first object down where the actor is (in what the actor
 * is in). Something tied to the rope puts the rope down instead; a liquid
 * evaporates. */
void action_put_down(void) {
  if (!check_carrying()) return;
  if (only_trying()) return;
  uint16_t rec = object1_record();
  if (REC(rec)->container == OBJ_ROPE) rec = ROPE_RECORD;
  REC(rec)->container = REC(actor_record())->container;
  if (!(REC(rec)->attrs & ATTR_LIQUID)) return;
  REC(rec)->locations[0] = 0;
  uint16_t name = name_address(rec);
  print_message(MSG_EVAPORATES, &name);
}

/* $8CF1. The weight of the object with what it holds is compared with the
 * actor's strength; then what the actor already carries is taken from
 * what is left, and only the sign of the result is tested (so strong
 * characters can be told they carry too much). */
static uint16_t lift_refusal(void) { /* the message, or 0 */
  uint8_t load = add_clamped(contents_weight(mem[V_OBJECT1]), REC(object1_record())->weight);
  uint8_t strength = REC(actor_record())->weight;
  if (strength < load) return MSG_TOO_HEAVY;
  uint8_t carried = contents_weight(mem[V_ACTOR]);
  /* (The original's bug tests the sign of what is left, so a difference
   * of 128 or more counts as too much: strong characters are refused.) */
  if (original_bugs) return (uint8_t)(strength - load - carried) & 0x80 ? MSG_TOO_MUCH : 0;
  return (uint8_t)(strength - load) < carried ? MSG_TOO_MUCH : 0;
}

bool check_lift(void) {
  uint16_t refusal = lift_refusal();
  if (refusal) {
    print_message(refusal, NULL);
    return false;
  }
  return check_single();
}

/* $9246 */
bool is_single(void) { return REC(object1_record())->nlocations == 1; }

static bool single_and_solid(void) { return is_single() && !(REC(object1_record())->attrs & ATTR_LIQUID); }

/* $8D25 */
bool check_single(void) {
  if (single_and_solid()) return true;
  finish_action();
  return false;
}

/* $8D33 and $8D3C: take the first object, unless it is too heavy or the
 * actor is in or on it. Something tied to the rope takes the rope. */
static void take(bool unless_carried) {
  if (unless_carried && object_carried(mem[V_OBJECT1])) {
    print_message(MSG_ALREADY, NULL);
    return;
  }
  if (!check_lift()) return;
  for (uint8_t o = mem[V_ACTOR];;) {
    uint8_t in = REC(object_record_addr(o))->container;
    if (in == mem[V_OBJECT1]) {
      finish_action();
      return;
    }
    if (in == OBJECT_NONE) break;
    o = in;
  }
  uint16_t rec = object1_record();
  if (only_trying()) return;
  if (REC(rec)->container == OBJ_ROPE) rec = ROPE_RECORD;
  REC(rec)->container = mem[V_ACTOR];
}

void action_take(void) { take(true); }

/* $8CE0: take the first object out of the second, if it is in it. (The
 * original's bug tests the wrong flag after asking, so it takes anything
 * you carry out of anything.) */
void take_from(void) {
  uint8_t outermost;
  bool inside = object_inside(mem[V_OBJECT1], mem[V_OBJECT2], &outermost);
  if (original_bugs ? !inside && outermost != YOU : !inside) {
    print_message(MSG_NOT_IN_IT, NULL);
    return;
  }
  take(false);
}

/* ---------- moving ---------- */

/* $8E85 (reads the location being entered, V_ENTERING, and the mover's
 * size, V_MOVER_SIZE, and notes the location's record). A door that is
 * neither open nor broken is shut; so is one with bit 7 of its kind, for
 * the player. exit: the exit's record. (For a location out of range,
 * which bad data can give, the original's lookup leaves its pointer as it
 * was: the door's record, or with no door the exit's; the record noted,
 * and the capacity read, are there.) */
uint8_t can_enter(uint8_t door, uint16_t exit) {
  uint16_t fallback = exit;
  if (door != 0) {
    uint16_t d = object_record_addr(door);
    fallback = d;
    if (!(REC(d)->attrs & (ATTR_OPEN | ATTR_BROKEN))) return 1;
    if (mem[V_ACTOR] == YOU && (REC(d)->kind & 0x80)) return 1;
    if (mem[V_MOVER_SIZE] >= REC(d)->size) return 2;
  }
  uint8_t to = mem[V_ENTERING];
  LocationRecord *l = location_record(to);
  uint16_t rec = l ? addr_of(l) : fallback;
  set_word_at(V_ENTERING_RECORD, rec);
  if (LOCREC(rec)->capacity == 0xFF) return 0;
  return mem[V_MOVER_SIZE] < location_room(to, rec) ? 0 : 3;
}

/* $8DD9: there is no way: in the dark the player falls (losing half his
 * strength) and may die; otherwise the action fails. */
static void no_way(void) {
  if (!player_in_dark()) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  uint8_t strength = REC(PLAYER_RECORD)->strength >> 1;
  REC(PLAYER_RECORD)->strength = strength;
  if (strength != 0) {
    print_message(MSG_HIT_HEAD, NULL);
    return;
  }
  print_message(MSG_SMASH_SKULL, NULL);
  you_are_dead();
}

/* $8D9D: go in the direction of the action (a random one in the dark). */
void action_go(void) {
  if (player_in_dark()) mem[V_ACTION] = (uint8_t)(random_upto(9) + 1);
  move_actor();
}

/* $8DAB: move the actor in the direction of the action. An actor carried
 * by a character gets down first; one inside something else cannot go. */
void move_actor(void) {
  uint16_t actor = actor_record();
  uint8_t in = REC(actor)->container;
  if (in != OBJECT_NONE) {
    if (!(REC(object_record_addr(in))->attrs & ATTR_ANIMATE)) {
      no_way();
      return;
    }
    REC(actor)->container = OBJECT_NONE;
  }
  mem[V_MOVER_SIZE] = (uint8_t)(contents_size(mem[V_ACTOR]) + REC(actor)->size);
  bool found;
  uint16_t exit = exit_in_direction(mem[V_ACTION], &found);
  if (!found || mem[exit + 2] == 0) {
    no_way();
    return;
  }
  mem[V_ENTERING] = mem[exit + 2];
  uint8_t door = mem[exit + 1];
  switch (can_enter(door, exit)) {
  case 0:
    move_to(actor, mem[V_ENTERING]);
    break;
  case 1:
    no_way();
    break;
  case 2: { /* the message is given the word at +2 of the door's record */
    uint16_t word = word_at((uint16_t)(object_record_addr(door) + 2));
    print_message(MSG_TOO_SMALL, &word);
    break;
  }
  default: {
    uint16_t name = LOCREC(word_at(V_ENTERING_RECORD))->name[0];
    print_message(MSG_TOO_FULL_ENTER, &name);
    break;
  }
  }
}

/* $8E12 */
void move_to(uint16_t mover, uint8_t location) {
  if (only_trying()) return;
  REC(mover)->locations[0] = location;
  object_moved(mem[V_ACTOR], location, mover);
  if (mem[V_ACTOR] != YOU) return;
  /* (The search leaves its entry, or the table's end, in the original's
   * IX, which arrive then has.) */
  uint16_t event = find_entry(LOCATION_EVENTS, mem[V_ENTERING]);
  if (entry_key(event) != 0xFF) run_location_event(entry_word(event), location);
  arrive(location, event);
}

/* $8E39: the first visit to some places adds to the percentage.
 * fallback: as for describe_location (characters.h): what the caller had
 * in IX; on a first visit, the original's search of the scores leaves its
 * own there. */
void arrive(uint8_t exits_of, uint16_t fallback) {
  if (player_in_dark()) return;
  uint8_t location = mem[V_ENTERING];
  if (mem[V_ACTOR] != YOU) {
    describe_location(location, fallback);
    return;
  }
  uint16_t rec = word_at(V_ENTERING_RECORD);
  if (mem[rec] & LOC_SCORED) {
    describe_location_briefly(location, exits_of, fallback);
    return;
  }
  mem[rec] |= LOC_SCORED;
  uint16_t score = find_entry(LOCATION_SCORES, location);
  if (entry_key(score) != 0xFF) set_word_at(V_SCORE, (uint16_t)(word_at(V_SCORE) + entry_word(score)));
  describe_location(location, score);
}

/* $8ED2 */
bool inside_closed(uint8_t obj) {
  uint16_t rec = object_record_addr(obj);
  while (REC(rec)->container != OBJECT_NONE) {
    rec = object_record_addr(REC(rec)->container);
    if (!(REC(rec)->attrs & ATTR_OPEN)) return true;
  }
  return false;
}

/* $8EEC: look through the first object (a window or door) into the
 * location beyond, if it is lit. */
void look_through(void) {
  uint16_t rec = object_record_addr(mem[V_OBJECT1]);
  if (!(REC(rec)->attrs & ATTR_OPEN)) {
    say_object_is(rec, 5);
    return;
  }
  if (inside_closed(mem[V_ACTOR])) return;
  bool found;
  uint16_t exit = exit_through_door(mem[V_OBJECT1], &found);
  if (!found || mem[exit + 2] == 0) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  uint8_t beyond = mem[exit + 2];
  /* The original asks whether location 1 is lit (the A that $9D44 left),
   * not the one beyond. */
  if (!(LOCREC(location_record_addr(1))->attrs & LOC_LIT)) {
    print_message(MSG_DARK, NULL);
    return;
  }
  uint16_t actor = actor_record();
  uint8_t here = REC(actor)->locations[0];
  REC(actor)->locations[0] = beyond;
  describe_location_you_see(beyond, 0);
  REC(actor)->locations[0] = here;
}

/* $8F3E: go through an exit (0: there is none). The door is checked with
 * whatever location and mover size were last noted. */
void go_exit(uint16_t exit) {
  if (exit == 0 || mem[exit + 2] == 0 || can_enter(mem[exit + 1], exit) != 0) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  mem[V_ACTION] = mem[exit];
  mem[V_OBJECT1] = OBJECT_NONE;
  move_actor();
}

/* $8F3B: go through the exit whose door is the first object. */
void go_through(void) {
  bool found;
  uint16_t exit = exit_through_door(mem[V_OBJECT1], &found);
  go_exit(found ? exit : 0);
}

/* $8F69: put the first object into the second (the pools stand for their
 * water), if that holds liquid and the first is not full. */
void put_in(void) {
  uint16_t rec = object1_record();
  uint8_t into = mem[V_OBJECT2];
  if (into == OBJ_POOL_A || into == OBJ_POOL_B) {
    uint16_t water = into == OBJ_POOL_A ? WATER_A_RECORD : WATER_B_RECORD;
    mem[V_OBJECT2] = into == OBJ_POOL_A ? OBJ_WATER_A : OBJ_WATER_B;
    set_word_at(V_OBJECT2_RECORD, water);
    REC(water)->container = OBJECT_NONE;
  }
  if (!(REC(object2_record())->attrs & ATTR_LIQUID)) {
    finish_action();
    return;
  }
  if (REC(rec)->attrs & ATTR_FULL) {
    say_object_is(rec, 0x82);
    return;
  }
  swap_objects_and_run(0x924F); /* put_into, the other way round */
}

/* $8FAD: run in a random direction that has an exit. */
void action_run(void) {
  uint8_t dir;
  do dir = random_upto(10);
  while (dir == 0);
  /* The directions from there round (1-9), until one has an exit. With
   * none, the action fails. (The original's bug tries for ever.) */
  for (int tries = 0; !has_exit_in_direction(dir); tries++) {
    if (tries == 9) {
      if (original_bugs) device_hang();
      finish_action();
      return;
    }
    dir = dir + 1 < 10 ? (uint8_t)(dir + 1) : 1;
  }
  mem[V_ACTION] = dir;
  action_go();
}

/* $8FCD: go through the exit to the first object (as a location). */
void action_enter(void) {
  bool found;
  uint16_t exit = exit_to_location(mem[V_OBJECT1], &found);
  go_exit(found ? exit : 0);
}

/* $8FD6: go where the first object is, if there is a way from here. */
void action_follow(void) {
  uint8_t there = location_of(object1_record());
  if (there != location_of(actor_record())) {
    bool found;
    uint16_t exit = exit_to_location(there, &found);
    if (found) {
      go_exit(exit);
      return;
    }
  }
  print_message(MSG_CANNOT_FOLLOW, NULL);
}

/* ---------- throwing, talking, opening ---------- */

/* $8FF5: throw the first object at the second: an attack on a character,
 * a blow on anything else; then it lands where it is. */
void action_throw_at(void) {
  if (!check_lift()) return;
  bool at_character = REC(object2_record())->attrs & ATTR_ANIMATE;
  mem[V_ACTION] = at_character ? 0x0F : 0x0B;
  swap_objects_and_run(at_character ? 0x9171 : 0x92ED); /* action_attack, break_object */
  mem[V_ACTION] = 0x2A;
  if (mem[V_DOING] != 1) return;
  uint16_t rec = object1_record();
  REC(rec)->container = OBJECT_NONE;
  mem[V_ACTION] = 0x0F;
  character_reacts(mem[V_OBJECT1], rec, mem[V_OBJECT2]);
}

/* $9034: say what was said in quotes to the first object: a character
 * does (at most) a random number of the commands, up to its own limit
 * (none: all of them), unless characters always answer. */
void action_talk_to(void) {
  if (only_trying()) return;
  uint16_t character = find_character(mem[V_OBJECT1]);
  uint8_t obey = 0;
  if (mem[character] != 0xFF) {
    if (mem[V_ALWAYS_ANSWER] != 0) {
      obey = mem[V_ALWAYS_ANSWER];
    } else if (mem[character + 6] != 0) {
      obey = random_upto(mem[character + 6]);
      if (obey == 0) print_message(MSG_SAYS_NO, NULL);
    }
  }
  give_orders(obey);
}

/* $9065: open the first object if it is closed, close it if open. */
void open_or_close(void) {
  if (only_trying()) return;
  uint16_t rec = object1_record();
  if (REC(rec)->attrs & ATTR_OPEN)
    close_it(rec);
  else
    open_it(rec);
}

/* $9117: open it; for a container (the first object), show what is in it. */
void open_it(uint16_t rec) {
  REC(rec)->attrs |= ATTR_OPEN;
  if (REC(rec)->nlocations != 1) return;
  if (object_count(mem[V_OBJECT1]) == 0) return;
  if (!list_inside(mem[V_OBJECT1])) return;
  list_contents(mem[V_OBJECT1], REC(rec)->locations[0]);
}

/* $910E: open the first object, unless it is locked or open already. */
void open_object(void) {
  uint8_t word;
  if (locked_word(&word)) {
    say_object_is(object1_record(), word);
    return;
  }
  if (only_trying()) return;
  open_it(object1_record());
}

/* $9145 */
void close_it(uint16_t rec) { REC(rec)->attrs &= (uint8_t)~ATTR_OPEN; }

/* $9138: close the first object, if it is open, and the actor is not in
 * it. (The original's bug lets you close it on yourself: then it is dark,
 * you cannot see it to open it, nor climb out: you are stuck for good.) */
void close_object(void) {
  uint16_t rec = object1_record();
  if (!(REC(rec)->attrs & ATTR_OPEN)) {
    say_object_is(rec, 5);
    return;
  }
  if (!original_bugs && object_inside(mem[V_ACTOR], mem[V_OBJECT1], NULL)) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  close_it(rec);
}

/* ---------- fighting ---------- */

/* $914A: (bits 4-6 of the kind say whom it fights.) */
bool check_may_attack(void) {
  uint16_t rec = object1_record();
  if (mem[V_ACTOR] == YOU) REC(rec)->kind &= (uint8_t)~0x10;
  if ((REC(rec)->kind & 0x70 & REC(actor_record())->kind) == 0) return true;
  mem[V_DONE] = 0;
  return false;
}

/* $9076: shoot the first object with the bow. Bard never misses; the
 * dragon is never hit by anyone else; others miss 3 times in 9. */
void action_shoot(void) {
  if (!object_carried(OBJ_BOW)) {
    print_message(MSG_NO_BOW, NULL);
    return;
  }
  if (!check_may_attack()) return;
  if (only_trying()) return;
  mem[V_ACTION] = 0x0F;
  if (mem[V_ACTOR] != BARD && (mem[V_OBJECT1] == DRAGON || random_upto(8) < 3)) {
    print_message(MSG_ARROW_MISSES, NULL);
    return;
  }
  if (mem[V_OBJECT1] != OBJ_ARROW) REC(ARROW_RECORD)->container = OBJECT_NONE;
  print_message(MSG_ARROW_HITS, NULL);
  uint16_t rec = object1_record();
  if (!is_alive(rec)) {
    break_object();
    return;
  }
  kill(mem[V_OBJECT1]);
  say_object_is(rec, 6);
}

/* $90D2: "You are dead", the score, and a new game after a key. */
void you_are_dead(void) {
  mem[V_ACTOR] = YOU;
  print_message(MSG_DEAD, NULL);
  show_score();
  wait_key_and_restart();
}

/* $90EB: list what the actor carries. */
void action_inventory(void) {
  if (only_trying()) return;
  print_message(MSG_CARRYING, NULL);
  if (object_count(mem[V_ACTOR]) == 0) {
    print_message(MSG_NOTHING, NULL);
    return;
  }
  list_contents(mem[V_ACTOR], location_of(actor_record()));
}

/* $9213: v, give or take up to 10, within 0-255. (The original's bug
 * takes the carry of adding the roll as a byte for an overflow: a
 * negative roll on a value over 10 gives 0, and on a smaller one wraps
 * round to a big one; fights mostly end in one blow.) */
uint8_t randomise(uint8_t v) {
  int8_t roll = random_spread(10);
  if (original_bugs) {
    unsigned sum = v + (uint8_t)roll;
    if (sum <= 0xFF) return (uint8_t)sum;
    return roll < 0 ? 0 : 0xFF;
  }
  int sum = v + roll;
  return (uint8_t)(sum < 0 ? 0 : sum > 0xFF ? 0xFF : sum);
}

/* $9171: the actor attacks the first object, with the second (or bare
 * hands). Strength (with the weapon's) and the defence are randomised; a
 * clear win kills, a narrow one wounds, by how narrow. */
void action_attack(void) {
  if (!check_may_attack()) return;
  bool armed = mem[V_OBJECT2] != OBJECT_NONE;
  set_word_at(V_WEAPON_NAME, armed ? REC(object2_record())->name[0] : WORD_BARE_HANDS);
  uint8_t power = REC(actor_record())->strength;
  if (armed) {
    uint16_t weapon = object2_record();
    if (REC(weapon)->nlocations != 1) {
      print_message(MSG_CANNOT_KILL, NULL);
      return;
    }
    power = add_clamped(REC(weapon)->strength, power);
  }
  power = randomise(power);
  if (only_trying()) return;
  uint16_t victim = object1_record();
  uint8_t defence = randomise(REC(victim)->defence);
  if (defence >= power) {
    print_message(MSG_WASTED, NULL);
    return;
  }
  if (add_clamped(defence, 0x10) < power) {
    print_message(MSG_CLEAVE, NULL);
    REC(victim)->attrs |= ATTR_BROKEN;
    kill(mem[V_OBJECT1]);
    say_object_is(victim, 6);
    return;
  }
  uint8_t margin = (uint8_t)(power - defence);
  uint16_t msg = word_at((uint16_t)(DAMAGE_MESSAGES + rotate_left(margin)));
  uint8_t loss = rotate_right(margin);
  if (REC(victim)->strength > loss) REC(victim)->strength -= (uint8_t)(loss + 1);
  loss = rotate_right(loss);
  if (REC(victim)->defence > loss) REC(victim)->defence -= (uint8_t)(loss + 1);
  print_message(msg, NULL);
}

/* $924F (run with the objects swapped): put the first object into the
 * second, if it is open (or the action is $12) and has room. */
void put_into(void) {
  if (!check_single()) return;
  uint16_t rec = object1_record();
  if (mem[V_OBJECT2] == REC(rec)->container) {
    print_cannot_do_that();
    return;
  }
  uint16_t into = object2_record();
  if (mem[V_ACTION] != 0x12 && !(REC(into)->attrs & ATTR_OPEN)) {
    say_object_is(into, 5);
    return;
  }
  if (REC(into)->size < REC(rec)->size ||
      (uint8_t)(REC(into)->size - REC(rec)->size) <= contents_size(mem[V_OBJECT2])) {
    print_message(MSG_TOO_FULL, NULL);
    return;
  }
  if (only_trying()) return;
  REC(rec)->locations[0] = REC(into)->locations[0];
  REC(rec)->container = mem[V_OBJECT2];
}

/* $92BA: the actor eats the first object, gaining strength; too much
 * ($80 or more) kills. The object is gone. */
static void eat_gaining(uint8_t gain) {
  uint16_t actor = actor_record();
  uint8_t strength = (uint8_t)(REC(actor)->strength + gain);
  if (strength >= 0x80) {
    print_message(MSG_GLUTTONY, NULL);
    kill(mem[V_ACTOR]);
    return;
  }
  REC(actor)->strength = strength;
  uint16_t rec = object1_record();
  REC(rec)->container = OBJECT_NONE;
  uint8_t n = REC(rec)->nlocations, i = 0;
  do REC(rec)->locations[i++] = 0; /* (256 of them if it is in none) */
  while (--n);
}

/* $929C: eat the first object: out of something (which is then not full),
 * 1 strength; otherwise as eat_food. */
void eat(void) {
  if (only_trying()) return;
  uint8_t in = REC(object1_record())->container;
  if (in == OBJECT_NONE) {
    eat_food();
    return;
  }
  REC(object_record_addr(in))->attrs &= (uint8_t)~ATTR_FULL;
  eat_gaining(1);
}

/* $92B5: eat the first object: $0A strength. */
void eat_food(void) {
  if (only_trying()) return;
  eat_gaining(0x0A);
}

/* $92ED: break (or wound) the first object, with the second if any (it
 * must have strength and a handler for breaking): a random blow with the
 * tool's and the actor's strength against its defence. The tool may
 * break on it too; then what the first object holds is dropped (the
 * original empties the first object, not the tool). */
void break_object(void) {
  uint16_t target = object1_record();
  if (REC(target)->attrs & ATTR_LIQUID) {
    finish_action();
    return;
  }
  if (REC(target)->attrs & ATTR_BROKEN) {
    say_object_is(target, 0x83);
    return;
  }
  if (REC(target)->defence == 0) {
    finish_action();
    return;
  }
  uint8_t force = 0;
  if (mem[V_OBJECT2] != OBJECT_NONE) {
    uint16_t tool = object2_record();
    if (REC(tool)->strength == 0 || entry_key(object_action_entry(object_at(tool), 0x0B)) == 0xFF) {
      finish_action();
      return;
    }
    force = REC(tool)->strength;
  }
  if (only_trying()) return;
  uint8_t blow = add_clamped((uint8_t)(random_spread(0x15) + force), REC(actor_record())->strength);
  if (blow >= REC(target)->defence) {
    REC(target)->attrs |= ATTR_BROKEN;
    reset_object(mem[V_OBJECT1]);
    REC(target)->strength = halve_signed(REC(target)->strength);
    if (REC(target)->kind < 2) empty_object(mem[V_OBJECT1]);
    say_object_is(target, 0x83);
  }
  if (mem[V_OBJECT2] == OBJECT_NONE) return;
  uint16_t tool = object2_record();
  if (REC(tool)->attrs & ATTR_BROKEN) return;
  uint8_t back = add_clamped((uint8_t)random_spread(0x15), REC(tool)->defence);
  if (back < REC(target)->defence) return;
  REC(tool)->attrs |= ATTR_BROKEN;
  reset_object(mem[V_OBJECT2]);
  REC(tool)->strength = halve_signed(REC(tool)->strength);
  empty_object(mem[V_OBJECT1]);
  say_object_is(tool, 0x83);
}

/* $939E: give the first object to the second, if it can carry it. */
void action_give(void) {
  uint16_t to = object2_record();
  if (!object_carried(mem[V_OBJECT1])) {
    print_message(MSG_NOT_CARRYING, NULL);
    return;
  }
  uint8_t load = (uint8_t)(contents_weight(mem[V_OBJECT2]) + REC(object1_record())->weight);
  if (REC(to)->weight < load) {
    print_message(MSG_TOO_MUCH, NULL);
    return;
  }
  if (only_trying()) return;
  uint16_t rec = object1_record();
  REC(rec)->container = mem[V_OBJECT2];
  REC(rec)->locations[0] = REC(to)->locations[0];
  object_moved(mem[V_OBJECT1], REC(to)->locations[0], to);
}

/* ---------- exits, words about objects ---------- */

/* $A138: "visible exits are: ..." for the location, if it has any (the
 * exits with a direction and no door; $FF ends the list). */
void list_exits(uint8_t location, uint16_t fallback) {
  LocationRecord *l = location_record(location);
  uint16_t e = next_open_exit((uint16_t)((l ? addr_of(l) : fallback) + 7));
  if (mem[e] == 0xFF) return;
  print_message(MSG_EXITS, NULL);
  do {
    print_token(direction_word(mem[e]));
    e = next_open_exit(e);
  } while (mem[e] != 0xFF);
  print_newline();
}

/* $A164, $A16C: "<object> is <word>." (the word from one of two tables,
 * by bit 7). */
void say_object_is(uint16_t rec, uint8_t word) {
  uint16_t w = table_word(word & 0x80 ? WORDS_IS_ALT : WORDS_IS, word);
  uint16_t args[2] = {REC(rec)->name[0], w}; /* the object's name, the word */
  print_message(MSG_IS, args);
}

/* $A18C: an object broken or killed: its adjectives become "dead" (a
 * character) or "broken". */
void reset_object(uint8_t obj) {
  uint16_t rec = object_record_addr(obj);
  REC(rec)->name[2] = 0;
  REC(rec)->name[1] = REC(rec)->attrs & ATTR_ANIMATE ? WORD_DEAD : WORD_BROKEN;
}

/* $A1C8: a character, and not dead? */
bool is_alive(uint16_t rec) { return (REC(rec)->attrs & (ATTR_ANIMATE | ATTR_BROKEN)) == ATTR_ANIMATE; }

/* $A1D0: is the action one of the five at EXIT_ACTIONS? */
bool action_in_list(void) {
  for (int i = 0; i < 5; i++)
    if (mem[EXIT_ACTIONS + i] == mem[V_ACTION]) return true;
  return false;
}

/* $A204 */
bool open_word(uint16_t rec, uint8_t *word) {
  *word = 0x85;
  return REC(rec)->attrs & ATTR_OPEN;
}

/* $A1F9 */
bool locked_word(uint8_t *word) {
  uint16_t rec = object1_record();
  if (REC(rec)->attrs & ATTR_LOCKED) {
    *word = 0x80;
    return true;
  }
  return open_word(rec, word);
}

/* ---------- tying, swimming, burning ---------- */

/* $A244: an action that does nothing. */
void do_nothing(void) { (void)only_trying(); }

/* $A248: tie the first object to the rope (the second; tying the rope to
 * something is done the other way round). Nothing else may be tied to
 * the rope already, and a live character cannot be tied. If the actor
 * does not carry the object, the rope goes with it when the actor could
 * take it (else it is left nowhere). */
void action_tie(void) {
  if (mem[V_OBJECT1] == OBJ_ROPE) {
    swap_objects_and_run(0xA248);
    return;
  }
  if (mem[V_OBJECT2] != OBJ_ROPE) {
    finish_action();
    return;
  }
  uint16_t rec = object1_record();
  if (REC(rec)->attrs & ATTR_LIQUID) {
    finish_action();
    return;
  }
  if (object_count(OBJ_ROPE) != 0) {
    print_message(MSG_ALREADY_TIED, NULL);
    return;
  }
  if ((REC(rec)->attrs & (ATTR_BROKEN | ATTR_ANIMATE)) == ATTR_ANIMATE) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  uint16_t rope = object2_record();
  bool carried = object_carried(mem[V_OBJECT1]);
  REC(rec)->container = mem[V_OBJECT2];
  if (!carried) {
    mem[V_DOING] = 0;
    action_take();
    bool could = mem[V_DONE] != 0;
    mem[V_DOING] = mem[V_DONE] = 1;
    /* (The original's bug: the take leaves the actor's record where the
     * rope's was kept, so the actor, not the rope, is put in the actor or
     * in nothing: inside itself, the next walk through containers loops
     * for ever.) */
    if (original_bugs) rope = actor_record();
    if (!could) {
      REC(rope)->container = OBJECT_NONE;
      return;
    }
  }
  REC(rope)->container = mem[V_ACTOR];
}

/* $A2B4: untie the first object: it goes where the rope is. */
void action_untie(void) {
  uint16_t rec = object1_record();
  if (REC(rec)->container != OBJ_ROPE) {
    print_message(MSG_NOT_TIED, NULL);
    return;
  }
  if (only_trying()) return;
  REC(rec)->container = REC(ROPE_RECORD)->container;
}

/* $A2CD: swim the river (the first object): go where the exit through it
 * leads, with it open for the move. (The original does not check that
 * there is such an exit; it reads past the end of the list.) */
void swim_river(void) {
  uint16_t exit = exit_through_door(mem[V_OBJECT1], NULL);
  if (mem[exit + 2] == 0) {
    (void)only_trying();
    return;
  }
  uint8_t action = mem[V_ACTION], obj1 = mem[V_OBJECT1];
  mem[V_ACTION] = mem[exit];
  uint16_t rec = object1_record();
  REC(rec)->attrs |= ATTR_OPEN;
  mem[V_OBJECT1] = OBJECT_NONE;
  move_actor();
  REC(rec)->attrs &= (uint8_t)~ATTR_OPEN;
  mem[V_ACTION] = action;
  mem[V_OBJECT1] = obj1;
}

/* $A302: only the dragon can burn: the first object is destroyed. */
void action_burn(void) {
  if (mem[V_ACTOR] != DRAGON) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  kill(mem[V_OBJECT1]);
}

/* $A316: the message, and (for you) "you drown"; the actor dies. */
static void drown(uint16_t msg) {
  print_message(msg, NULL);
  if (mem[V_ACTOR] == YOU) print_message(MSG_YOU_DROWN, NULL);
  kill(mem[V_ACTOR]);
}

/* $A310 */
void swim_black_river(void) {
  if (only_trying()) return;
  drown(MSG_SWEPT_AWAY);
}

/* $A328 */
void drink_black_water(void) {
  if (only_trying()) return;
  drown(MSG_POISONED);
}

/* ---------- locks, doors, the web, the ring ---------- */

/* $A33A: lock or unlock the first object with the second: the right key
 * does it; another key is the wrong one; anything else cannot. */
void lock_with(uint8_t key) {
  uint8_t with = mem[V_OBJECT2];
  if (with == key) {
    if (mem[V_ACTION] == 0x25)
      lock_object();
    else
      unlock_object();
    return;
  }
  if (with == OBJ_KEY_SIDE || with == OBJ_KEY_ROCK || with == OBJ_KEY_RED)
    wrong_key();
  else
    print_cannot_do_that();
}

/* $A358 */
void wrong_key(void) { print_message(MSG_WRONG_KEY, NULL); }

/* $A35E: the side door opens once the action is done. */
void side_door(void) {
  if (!was_done()) return;
  open_it(object1_record());
}

/* $A368: the crack opens only from location $0F. */
void open_crack(void) {
  if (location_of(actor_record()) != 0x0F) {
    finish_action();
    return;
  }
  open_object();
}

/* $A377 (every turn): once the web is broken it is open, and the
 * spiders' countdown starts again. */
void web(void) {
  uint16_t rec = object1_record();
  if (!(REC(rec)->attrs & ATTR_BROKEN)) return;
  REC(rec)->attrs |= ATTR_OPEN;
  mem[SPIDER_COUNT] = mem[SPIDER_PERIOD];
  print_message(MSG_WEB_BROKEN, NULL);
}

/* $A390: wear the ring: the actor is invisible and a quarter as strong
 * for 2-10 turns. */
void wear_ring(void) {
  if (only_trying()) return;
  uint16_t actor = actor_record();
  REC(actor)->attrs &= (uint8_t)~ATTR_VISIBLE;
  REC(actor)->strength >>= 2;
  REC(RING_RECORD)->attrs &= (uint8_t)~ATTR_VISIBLE;
  REC(RING_RECORD)->container = mem[V_ACTOR];
  mem[RING_TIMER] = (uint8_t)(random_upto(8) + 2);
}

/* $A3BC: take the ring off (also when its time is up). */
void take_off_ring(void) {
  uint16_t actor = actor_record();
  if (REC(RING_RECORD)->attrs & ATTR_VISIBLE) {
    print_message(MSG_NOT_WEARING, NULL);
    return;
  }
  if (only_trying()) return;
  REC(RING_RECORD)->attrs |= ATTR_VISIBLE;
  REC(actor)->attrs |= ATTR_VISIBLE;
  REC(actor)->strength = (uint8_t)(REC(actor)->strength << 2);
  mem[RING_TIMER] = 0;
}

/* ---------- capture, goblins, doors that close, sayings ---------- */

/* $A3E6: the actor captures the first object (a character it does not
 * fight): into the elvenking's dungeon ($1F) for the wood elf and the
 * butler, the goblins' ($0D) for anyone else; not if it is there already.
 * You are then told where you are. */
void action_capture(void) {
  uint16_t rec = object1_record();
  if ((REC(rec)->kind & 0x70 & REC(actor_record())->kind) != 0 || !(REC(rec)->attrs & ATTR_ANIMATE)) {
    finish_action();
    return;
  }
  uint8_t dungeon = mem[V_ACTOR] == WOOD_ELF || mem[V_ACTOR] == BUTLER ? 0x1F : 0x0D;
  if (mem[V_ACTOR_LOCATION] == dungeon) {
    finish_action();
    return;
  }
  if (only_trying()) return;
  REC(rec)->locations[0] = dungeon;
  REC(rec)->container = OBJECT_NONE;
  /* (The original hands its caller's IY on as the mover: see move_contents.) */
  object_moved(mem[V_OBJECT1], dungeon, rec);
  if (mem[V_OBJECT1] != YOU) return;
  mem[V_ACTOR] = YOU;
  set_word_at(V_ACTOR_RECORD, PLAYER_RECORD);
  if (player_in_dark()) return;
  describe_location_after(MSG_YOU_ARE_IN, dungeon, 0);
}

/* $A448 (every turn): a dead goblin comes back: its entry in ESCAPES says
 * where to note its id, where it goes and its new adjective; if that is
 * where you are, you are told. (The original's bug compares your location
 * with a register its caller left, here, instead: 0, so it never tells.) */
void goblin(uint8_t here) {
  uint16_t rec = object1_record();
  if (!(REC(rec)->attrs & ATTR_BROKEN)) return;
  REC(rec)->attrs &= (uint8_t)~ATTR_BROKEN;
  uint16_t e = ESCAPES;
  while (mem[e] != mem[V_OBJECT1]) e += 6;
  mem[word_at((uint16_t)(e + 1))] = mem[V_OBJECT1];
  REC(rec)->locations[0] = mem[e + 3];
  REC(rec)->name[1] = word_at((uint16_t)(e + 4));
  print_message(MSG_GOBLIN_BACK, NULL);
  if (!original_bugs) here = REC(rec)->locations[0];
  if (REC(PLAYER_RECORD)->locations[0] != here) return;
  print_token(0x005E);
  print_token(0x02E2);
  print_message(MSG_GOBLIN_HERE, NULL);
}

/* $A4C0: the goblins' door opens only from location $10, and closes
 * again after a while. */
void open_goblin_door(void) {
  if (location_of(actor_record()) != 0x10) {
    finish_action();
    return;
  }
  open_object();
  if (!was_done()) return;
  mem[GOBLIN_DOOR_COUNT] = mem[GOBLIN_DOOR_PERIOD];
}

/* $A4D9 */
void close_goblin_door(void) { mem[GOBLIN_DOOR_ATTRS] &= (uint8_t)~ATTR_OPEN; }

/* $A4DF, $A4F5, $A4FE, $A51C, $A525: what characters say. */
void say_a4df(void) {
  if (only_trying()) return;
  say_aloud(random_upto(10) >= 8 ? MSG_SAY_A0A6 : MSG_SAY_B047, NULL);
}

void say_a4f5(void) {
  if (only_trying()) return;
  say_aloud(MSG_SAY_B04B, NULL);
}

void say_a4fe(void) {
  if (only_trying()) return;
  uint8_t r = random_upto(2);
  say_aloud(r == 0 ? MSG_SAY_B052 : r == 1 ? MSG_SAY_B05B : MSG_SAY_B05F, NULL);
}

void say_a51c(void) {
  if (only_trying()) return;
  say_aloud(MSG_SAY_B061, NULL);
}

/* (Only where you are.) */
void say_a525(void) {
  if (mem[V_ACTOR_LOCATION] != mem[V_YOUR_LOCATION]) return;
  if (only_trying()) return;
  say_aloud(MSG_SAY_B05F, NULL);
}

/* ---------- the routines the data names, and starting again ---------- */

/* $9F4A SwapAction: run the routine at code with the two objects (and
 * their records) the other way round, then put them back. */
static void swap_objects_and_run(uint16_t code) {
  uint16_t rec1 = word_at(V_OBJECT1_RECORD), rec2 = word_at(V_OBJECT2_RECORD);
  uint8_t obj1 = mem[V_OBJECT1], obj2 = mem[V_OBJECT2];
  set_word_at(V_OBJECT1_RECORD, rec2);
  set_word_at(V_OBJECT2_RECORD, rec1);
  mem[V_OBJECT1] = obj2;
  mem[V_OBJECT2] = obj1;
  run_routine(code);
  mem[V_OBJECT1] = obj1;
  mem[V_OBJECT2] = obj2;
  set_word_at(V_OBJECT1_RECORD, rec1);
  set_word_at(V_OBJECT2_RECORD, rec2);
}

/* $90DF DeadWaitKey: wait for a key and start again. Never returns. */
static void wait_key_and_restart(void) {
  wait_key_after_death();
  device_restart();
}

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ---------- adapters ----------
 * Most of these routines are action handlers, reached through
 * TriggerAction ($9B6C, which keeps every register but AF) or by a JP
 * from another handler: their callers use no register. Those with
 * callers that do are noted. A check that refuses abandons its caller in
 * the original (it pops the caller's return address and goes on to the
 * message): the adapter does the same, so that the stack is as the
 * original leaves it. */

#define HANDLER(name, fn) \
  static void name(Cpu *c) { \
    (void)c; \
    fn(); \
  }

HANDLER(a_look, action_look)
HANDLER(a_put_down, action_put_down)
HANDLER(a_take_from, take_from)
HANDLER(a_take, action_take)
HANDLER(a_go, action_go)
HANDLER(a_move_actor, move_actor)
HANDLER(a_look_through, look_through)
HANDLER(a_go_through, go_through)
HANDLER(a_put_in, put_in)
HANDLER(a_run, action_run)
HANDLER(a_enter, action_enter)
HANDLER(a_follow, action_follow)
HANDLER(a_throw_at, action_throw_at)
HANDLER(a_talk_to, action_talk_to)
HANDLER(a_open_or_close, open_or_close)
HANDLER(a_shoot, action_shoot)
HANDLER(a_dead, you_are_dead)
HANDLER(a_inventory, action_inventory)
HANDLER(a_open_object, open_object)
HANDLER(a_close_object, close_object)
HANDLER(a_attack, action_attack)
HANDLER(a_put_into, put_into)
HANDLER(a_eat, eat)
HANDLER(a_eat_food, eat_food)
HANDLER(a_break, break_object)
HANDLER(a_give, action_give)
HANDLER(a_do_nothing, do_nothing)
HANDLER(a_tie, action_tie)
HANDLER(a_untie, action_untie)
HANDLER(a_swim_river, swim_river)
HANDLER(a_burn, action_burn)
HANDLER(a_swim_black_river, swim_black_river)
HANDLER(a_drink_black_water, drink_black_water)
HANDLER(a_wrong_key, wrong_key)
HANDLER(a_side_door, side_door)
HANDLER(a_open_crack, open_crack)
HANDLER(a_web, web)
HANDLER(a_wear_ring, wear_ring)
HANDLER(a_take_off_ring, take_off_ring)
HANDLER(a_capture, action_capture)
HANDLER(a_open_goblin_door, open_goblin_door)
HANDLER(a_close_goblin_door, close_goblin_door)
HANDLER(a_say_a4df, say_a4df)
HANDLER(a_say_a4f5, say_a4f5)
HANDLER(a_say_a4fe, say_a4fe)
HANDLER(a_say_a51c, say_a51c)
HANDLER(a_say_a525, say_a525)

/* Abandon the caller (POP its return address) and go on at addr (where
 * the original goes on after that POP) with HL. */
static void abandon_caller(Cpu *c, uint16_t addr, uint16_t hl) {
  c->sp += 2;
  set_hl(c, hl);
  cpu_tail(c, addr);
}

/* $8C9B (from $8CA6 and $94A4): C if carried. */
static void a_check_carrying(Cpu *c) {
  if (object_carried(mem[V_OBJECT1])) {
    c->cf = 1;
    return;
  }
  abandon_caller(c, 0x8CA0, MSG_NOT_CARRYING);
}

/* $8CF1 (from $8D3C and $8FF5). */
static void a_check_lift(Cpu *c) {
  uint16_t refusal = lift_refusal();
  if (refusal)
    abandon_caller(c, 0x8D22, refusal);
  else if (single_and_solid())
    c->ix = object1_record();
  else
    abandon_caller(c, 0x8D30, 0);
}

/* $8D25 (from $924F, $9428): IX the first object's record (used by $924F). */
static void a_check_single(Cpu *c) {
  if (single_and_solid()) {
    c->ix = object1_record();
    c->zf = 1;
  } else {
    abandon_caller(c, 0x8D30, 0);
  }
}

/* $914A (from $9076, $9171). */
static void a_check_may_attack(Cpu *c) {
  if (!check_may_attack()) abandon_caller(c, 0x916C, get_hl(c));
}

/* $9246 (from $8D25): Z if single, IX the first object's record. */
static void a_is_single(Cpu *c) {
  c->ix = object1_record();
  c->a = (uint8_t)(REC(c->ix)->nlocations - 1);
  c->zf = c->a == 0;
}

/* $8E85 (from $8E06, $8F50): A (and NZ) the answer; B the location
 * entered; IX the door's record (the "too small" message reads it), or
 * the location's. */
static void a_can_enter(Cpu *c) {
  uint8_t door = c->a;
  uint8_t answer = can_enter(door, c->ix);
  if (answer == 1 || answer == 2) {
    c->ix = object_record_addr(door);
  } else {
    c->ix = word_at(V_ENTERING_RECORD);
    c->b = mem[V_ENTERING];
  }
  c->a = answer;
  c->zf = answer == 0;
}

/* $8ED2 (from $8EFB): NZ if inside something closed; IX kept. */
static void a_inside_closed(Cpu *c) {
  uint16_t ix = c->ix;
  c->zf = !inside_closed(c->a);
  c->ix = ix;
}

/* $8E12 (from $9C05, MoveContents): the mover in IY, the location in B. */
static void a_move_to(Cpu *c) {
  move_to(c->iy, c->b);
}

/* $8E39 (from $C7F0): the location whose exits are listed in B. */
static void a_arrive(Cpu *c) {
  arrive(c->b, c->ix);
}

/* $8F3E: A $FF for no exit, else IX the exit's record. */
static void a_go_exit(Cpu *c) {
  go_exit(c->a == 0xFF ? 0 : c->ix);
}

/* $9117, $9145: the object's record in IX. */
static void a_open_it(Cpu *c) {
  open_it(c->ix);
}
static void a_close_it(Cpu *c) { close_it(c->ix); }

/* $9213 (from $91B0, $91BE): A in and out. */
static void a_randomise(Cpu *c) { c->a = randomise(c->a); }

/* $8CA0, $8D22, $8D30, $916C: where the checks above go on after
 * abandoning their caller. */
static void a_not_carrying(Cpu *c) { (void)c;
  print_message(MSG_NOT_CARRYING, NULL);
}
static void a_lift_message(Cpu *c) {
  print_message(get_hl(c), NULL);
}
static void a_single_fail(Cpu *c) { (void)c;
  finish_action();
}
static void a_no_fight(Cpu *c) {
  (void)c;
  mem[V_DONE] = 0;
}

/* $A138 (from $9680): the location in A; IX IY DE BC kept. */
static void a_list_exits(Cpu *c) {
  uint16_t ix = c->ix, iy = c->iy, de = get_de(c), bc = get_bc(c);
  list_exits(c->a, c->ix);
  c->ix = ix, c->iy = iy;
  set_de(c, de);
  set_bc(c, bc);
}

/* $A164 (the object at IY), $A16C (at IX): the word in A. */
static void a_say_iy_is(Cpu *c) {
  say_object_is(c->iy, c->a);
}
static void a_say_ix_is(Cpu *c) {
  say_object_is(c->ix, c->a);
}

/* $A18C (from $934C, $9387, $97A0, $A74E): the object in A; IX BC HL
 * kept, DE its new adjective. */
static void a_reset_object(Cpu *c) {
  uint16_t ix = c->ix, bc = get_bc(c), hl = get_hl(c);
  reset_object(c->a);
  set_de(c, REC(c->ix)->name[1]);
  c->ix = ix;
  set_bc(c, bc);
  set_hl(c, hl);
}

/* $A1C8 (from $90C1): Z if alive. */
static void a_is_alive(Cpu *c) {
  c->a = c->mem[(uint16_t)(c->ix + 7)] & (ATTR_ANIMATE | ATTR_BROKEN);
  c->zf = is_alive(c->ix);
}

/* $A1D0 (from $94F0, $9575): Z if in the list; A the action. */
static void a_action_in_list(Cpu *c) {
  c->zf = action_in_list();
  c->a = mem[V_ACTION];
}

/* $A1F9 (from $910E, $946D): IX the first object's record, A the word,
 * NZ if locked or open. */
static void a_locked_word(Cpu *c) {
  uint8_t word;
  c->zf = !locked_word(&word);
  c->a = word;
  c->ix = object1_record();
}

/* $A204 (from $949A): the object at IX; A the word, NZ if open. */
static void a_open_word(Cpu *c) {
  uint8_t word;
  c->zf = !open_word(c->ix, &word);
  c->a = word;
}

/* $A330, $A334, $A338: the key for each door. */
static void a_lock_with_side(Cpu *c) { (void)c;
  lock_with(OBJ_KEY_SIDE);
}
static void a_lock_with_red(Cpu *c) { (void)c;
  lock_with(OBJ_KEY_RED);
}
static void a_lock_with_rock(Cpu *c) { (void)c;
  lock_with(OBJ_KEY_ROCK);
}

/* $A448: B is compared with your location. */
static void a_goblin(Cpu *c) {
  goblin(c->b);
}

#define NONE 0

const CleanRoutine actions_clean[] = {
    {0x8C4B, "action_look", a_look, NONE},
    {0x8C9B, "check_carrying", a_check_carrying, NONE},
    {0x8CA6, "action_put_down", a_put_down, NONE},
    {0x8CE0, "take_from", a_take_from, NONE},
    {0x8CF1, "check_lift", a_check_lift, OUT_IX},
    {0x8D25, "check_single", a_check_single, OUT_IX},
    {0x8D33, "action_take", a_take, NONE},
    {0x8D9D, "action_go", a_go, NONE},
    {0x8DAB, "move_actor", a_move_actor, NONE},
    {0x8E12, "move_to", a_move_to, NONE},
    {0x8E39, "arrive", a_arrive, NONE},
    {0x8E85, "can_enter", a_can_enter, OUT_A | OUT_B | OUT_IX | OUT_ZF},
    {0x8ED2, "inside_closed", a_inside_closed, OUT_IX | OUT_ZF},
    {0x8EEC, "look_through", a_look_through, NONE},
    {0x8F3B, "go_through", a_go_through, NONE},
    {0x8F3E, "go_exit", a_go_exit, NONE},
    {0x8F69, "put_in", a_put_in, NONE},
    {0x8FAD, "action_run", a_run, NONE},
    {0x8FCD, "action_enter", a_enter, NONE},
    {0x8FD6, "action_follow", a_follow, NONE},
    {0x8FF5, "action_throw_at", a_throw_at, NONE},
    {0x9034, "action_talk_to", a_talk_to, NONE},
    {0x9065, "open_or_close", a_open_or_close, NONE},
    {0x9076, "action_shoot", a_shoot, NONE},
    {0x90D2, "you_are_dead", a_dead, NONE},
    {0x90EB, "action_inventory", a_inventory, NONE},
    {0x910E, "open_object", a_open_object, NONE},
    {0x9117, "open_it", a_open_it, NONE},
    {0x9138, "close_object", a_close_object, NONE},
    {0x9145, "close_it", a_close_it, NONE},
    {0x914A, "check_may_attack", a_check_may_attack, NONE},
    {0x9171, "action_attack", a_attack, NONE},
    {0x9213, "randomise", a_randomise, OUT_A},
    {0x9246, "is_single", a_is_single, OUT_A | OUT_IX | OUT_ZF},
    {0x924F, "put_into", a_put_into, NONE},
    {0x929C, "eat", a_eat, NONE},
    {0x92B5, "eat_food", a_eat_food, NONE},
    {0x92ED, "break_object", a_break, NONE},
    {0x939E, "action_give", a_give, NONE},
    {0x8CA0, "not_carrying", a_not_carrying, NONE},
    {0x8D22, "lift_message", a_lift_message, NONE},
    {0x8D30, "single_fail", a_single_fail, NONE},
    {0x916C, "no_fight", a_no_fight, NONE},
    {0xA138, "list_exits", a_list_exits, OUT_IX | OUT_IY | OUT_DE | OUT_BC},
    {0xA164, "say_object_is_iy", a_say_iy_is, NONE},
    {0xA16C, "say_object_is_ix", a_say_ix_is, NONE},
    {0xA18C, "reset_object", a_reset_object, OUT_IX | OUT_BC | OUT_HL | OUT_DE},
    {0xA1C8, "is_alive", a_is_alive, OUT_ZF},
    {0xA1D0, "action_in_list", a_action_in_list, OUT_ZF | OUT_HL | OUT_BC},
    {0xA1F9, "locked_word", a_locked_word, OUT_A | OUT_IX | OUT_ZF},
    {0xA204, "open_word", a_open_word, OUT_A | OUT_ZF},
    {0xA244, "do_nothing", a_do_nothing, NONE},
    {0xA248, "action_tie", a_tie, NONE},
    {0xA2B4, "action_untie", a_untie, NONE},
    {0xA2CD, "swim_river", a_swim_river, NONE},
    {0xA302, "action_burn", a_burn, NONE},
    {0xA310, "swim_black_river", a_swim_black_river, NONE},
    {0xA328, "drink_black_water", a_drink_black_water, NONE},
    {0xA330, "lock_with_side_key", a_lock_with_side, NONE},
    {0xA334, "lock_with_red_key", a_lock_with_red, NONE},
    {0xA338, "lock_with_rock_key", a_lock_with_rock, NONE},
    {0xA358, "wrong_key", a_wrong_key, NONE},
    {0xA35E, "side_door", a_side_door, NONE},
    {0xA368, "open_crack", a_open_crack, NONE},
    {0xA377, "web", a_web, NONE},
    {0xA390, "wear_ring", a_wear_ring, NONE},
    {0xA3BC, "take_off_ring", a_take_off_ring, NONE},
    {0xA3E6, "action_capture", a_capture, NONE},
    {0xA448, "goblin", a_goblin, NONE},
    {0xA4C0, "open_goblin_door", a_open_goblin_door, NONE},
    {0xA4D9, "close_goblin_door", a_close_goblin_door, NONE},
    {0xA4DF, "say_a4df", a_say_a4df, NONE},
    {0xA4F5, "say_a4f5", a_say_a4f5, NONE},
    {0xA4FE, "say_a4fe", a_say_a4fe, NONE},
    {0xA51C, "say_a51c", a_say_a51c, NONE},
    {0xA525, "say_a525", a_say_a525, NONE},
    {0, NULL, NULL, 0},
};

const CleanScratch actions_scratch[] = {
    {0x70DC, 0x70E1, "PrintMsg's copy of A, DE and IX (registers it restores; the clean callers pass none)"},
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
