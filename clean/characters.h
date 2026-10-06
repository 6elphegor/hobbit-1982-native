/* The clean edition: characters and events (the faithful port's
 * port/actions2.c and port/events.c). See docs/CLEAN.md.
 *
 * Action handlers (the routines named in the action tables and in the
 * objects' action lists, and run by the characters' scripts) take no
 * arguments: they work on the command in V_ACTION, V_OBJECT1, V_OBJECT2,
 * V_ACTOR and the records at V_OBJECT1_RECORD... Each is run twice: first
 * to try it (V_DOING = 0: it stops at only_trying, setting V_DONE), then
 * to do it. */
#ifndef HOBBIT_CLEAN_CHARACTERS_H
#define HOBBIT_CLEAN_CHARACTERS_H

#include "clean.h"

/* ---------- the turn, and the characters' scripts ---------- */

void end_of_turn(void);                 /* $96B3 */
void characters_act(void);              /* $980E */
bool character_tries(void);             /* $99C6: true if the actor did V_ACTION */
void new_game_characters(void);         /* $97AD */
uint16_t script_skip(uint16_t character, uint16_t op, uint16_t length); /* $9918 */
void script_random(uint16_t character, uint16_t op); /* $9A59 */
void script_choose(uint16_t character, uint8_t entry); /* $9A68 */

/* ---------- carrying out actions ---------- */

bool may_try_action(void);              /* $94D6: sets V_DONE, and returns it */
void do_action(void);                   /* $950F */
bool action_confirmed(void);            /* $97FF: V_DOING and V_DONE both set */
bool player_in_dark(void);              /* $95ED */
void character_reacts(uint8_t id, uint16_t record, uint8_t action); /* $95DF */
bool say_who_carries(uint8_t object);   /* $9728 */
void kill(uint8_t id);                  /* $977F */
void say_sentence(uint16_t message);    /* $97F4: message, ".", newline */
void say_aloud(uint16_t message, const uint16_t *args); /* $A1E3: You say "message". */

/* ---------- describing a location ---------- */

/* fallback, in these: the record the original's location lookup leaves
 * for a location that does not exist (what its caller had in IX), where
 * the description, the name, the doors and the exits are then read. */
void describe_location(uint8_t location, uint16_t fallback);          /* $9630: "You are in ..." */
void describe_location_you_see(uint8_t location, uint16_t fallback);  /* $962B: "You see ..." */
void describe_location_after(uint16_t message, uint8_t location, uint16_t fallback); /* $964D, $965B */
void print_location_text(uint16_t record);         /* $9686 */
void print_location_words(uint16_t record);        /* $9689 */
void describe_location_briefly(uint8_t location, uint8_t exits_of, uint16_t fallback); /* $96A8 */

/* ---------- action handlers ---------- */

void examine(void);              /* $93DA */
void empty_out(void);         /* $9404 */
void river_sweeps_away(void);    /* $9428 */
void lock_object(void);          /* $946D */
void unlock_object(void);        /* $948D */
void send_through_door(void);    /* $94A4 */
void kill_first_object(void);    /* $977C */
void climb_out(void);            /* $A541 */
void climb_in(void);             /* $A55F */
bool object_is_open(uint16_t record); /* $A5CA */
void look_across(void);          /* $8EF8 */

/* ---------- events ---------- */

void warg_howls(void);           /* $A5D1 */
void barrel_in_river(void);      /* $A5E2 */
void barrel_ashore(void);        /* $A5FB */
void wheres_the_thief(void);     /* $A640 */
void thorin_talks(void);         /* $A657 */
void reach_from_cellar(void);    /* $A67E */
void follows_you_in(void);       /* $A698 */
void dragon_talks(void);         /* $A6C2 */
void dragon_burns(void);         /* $A6DC */
void read_runes(void);           /* $A71E */
void breaks_too(void);           /* $A73B */
void reach_from_inside(void);    /* $A761 */
void reach_from_inside2(void);   /* $A784 */
void bog_sinks(void);            /* $A7AA */
void read_map(void);             /* $A7C4 */
void shoot_at(void);             /* $A814 */
bool chance_half(void);          /* $A86E */
void swing_back(void);           /* $A876 */
void swing_arrives(void);        /* $A89E */
void listens(void);              /* $A8AB */
void asks_riddle(void);          /* $A8D2 */
void strangles(void);            /* $A8F6 */
void says_something(void);       /* $A926 */
void trolls_eat(void);           /* $A94E */
void day_dawns(void);            /* $A971 */
void trolls_talk(void);          /* $A9BD */
void check_game_won(void);       /* $A9D6 */
void gives_you(void);            /* $A9E5 */
void go_into(void);           /* $AA27 */
void mends(void);                /* $AA5C */
void hole_vanishes(void);        /* $AA74 */
void hole_appears(void);         /* $AA91 */
void hole_closes(void);          /* $AAA2 */
void door_opens(void);           /* $AAB3 */
void say_at_door(uint16_t message); /* $AAC7 */
void door_closes(void);          /* $AAD5 */
void door_lets_out(void);        /* $AAE0 */
void starts_timer(void);         /* $AAF9 */
void timer_ends(void);           /* $AB0B */
void web_smothers(void);         /* $AB10 */
void eyes_stare(void);           /* $AB1F */
void something_stings(void);     /* $AB3A */
void at_beorns_house(void);      /* $C7A4 */
void at_spider_threads(void);    /* $C7B2 */
void at_deep_bog(void);          /* $C7B9 */
void at_elvenkings_cellar(void); /* $C7C0 */
void at_forest(void);            /* $C7DD */
void at_forest_river(uint8_t location); /* $C7EA: location, where you were moved to */

#endif
