/* The clean edition: actions (the handlers for the verbs and for the
 * objects' own actions, $8C4B-$93D9 and $A138-$A540).
 *
 * The try-then-do protocol: every handler runs twice for a command, first
 * with V_DOING = 0 (only try: it stops at the point where it would start
 * changing things, setting V_DONE) and then with V_DOING = 1 (do it). The
 * checks that can refuse an action (check_carrying, check_lift, ...) print
 * why and return false; the handler then stops. */
#ifndef HOBBIT_CLEAN_ACTIONS_H
#define HOBBIT_CLEAN_ACTIONS_H

#include <stdbool.h>
#include <stdint.h>

/* ---------- checks ($8C9B-$9246) ---------- */

/* $8C9B: is the first object carried by the actor? If not, "You are not
 * carrying it", false. */
bool check_carrying(void);
/* $8CF1: can the actor lift the first object? If not, why, false. Goes
 * on to check_single. */
bool check_lift(void);
/* $8D25: is the first object a single object that does not vanish when
 * put down? If not, the action fails, false. */
bool check_single(void);
/* $914A: may the actor fight the first object (their attitudes)? If not,
 * V_DONE = 0, false. The player attacking makes it hostile first. */
bool check_may_attack(void);
/* $9246: is the first object in a single location? */
bool is_single(void);
/* $8E85: can the actor go through door (0 for none) into the location
 * being entered? 0 yes, 1 the door is shut, 2 too small, 3 too full. */
uint8_t can_enter(uint8_t door, uint16_t exit);
/* $8ED2: is object obj inside something closed? */
bool inside_closed(uint8_t obj);
/* $9213: v plus a random -10..10, clamped to 0..255 (by the sign of the
 * random part: so a negative roll can give 0 where it should not). */
uint8_t randomise(uint8_t v);

/* ---------- the handlers, actions1 ($8C4B-$93D9) ---------- */

void action_look(void);       /* $8C4B */
void action_put_down(void);   /* $8CA6 */
void take_from(void);         /* $8CE0 */
void action_take(void);       /* $8D33 */
void action_go(void);         /* $8D9D */
void move_actor(void);        /* $8DAB */
/* $8E12: put the mover (record, normally the actor's) at location, and if
 * the actor is the player, run the place's event and arrive there. */
void move_to(uint16_t mover, uint8_t location);
/* $8E39: the player arrives at the location being entered: score it once,
 * describe it (its exits those of exits_of). */
void arrive(uint8_t exits_of, uint16_t fallback);
void look_through(void);      /* $8EEC */
void go_through(void);        /* $8F3B */
void go_exit(uint16_t exit);  /* $8F3E: through the exit record (0: none) */
void put_in(void);            /* $8F69 */
void action_run(void);        /* $8FAD */
void action_enter(void);      /* $8FCD */
void action_follow(void);     /* $8FD6 */
void action_throw_at(void);   /* $8FF5 */
void action_talk_to(void);    /* $9034 */
void open_or_close(void);     /* $9065 */
void action_shoot(void);      /* $9076 */
void you_are_dead(void);      /* $90D2: never returns */
void action_inventory(void);  /* $90EB */
void open_object(void);       /* $910E */
void open_it(uint16_t rec);   /* $9117 */
void close_object(void);      /* $9138 */
void close_it(uint16_t rec);  /* $9145 */
void action_attack(void);     /* $9171 */
void put_into(void);          /* $924F */
void eat(void);               /* $929C */
void eat_food(void);          /* $92B5 */
void break_object(void);      /* $92ED */
void action_give(void);       /* $939E */

/* ---------- actions3 ($A138-$A540) ---------- */

void list_exits(uint8_t location, uint16_t fallback); /* $A138 (fallback: see characters.h) */
void say_object_is(uint16_t rec, uint8_t word); /* $A164, $A16C */
void reset_object(uint8_t obj);                 /* $A18C */
bool is_alive(uint16_t rec);                    /* $A1C8 */
bool action_in_list(void);                      /* $A1D0 */
/* $A204: true if open; *word is then the word to say about it. */
bool open_word(uint16_t rec, uint8_t *word);
/* $A1F9: the first object: true if locked (word $80) or open. */
bool locked_word(uint8_t *word);
void do_nothing(void);        /* $A244 */
void action_tie(void);        /* $A248 */
void action_untie(void);      /* $A2B4 */
void swim_river(void);        /* $A2CD */
void action_burn(void);       /* $A302 */
void swim_black_river(void);  /* $A310 */
void drink_black_water(void); /* $A328 */
void lock_with(uint8_t key);  /* $A330, $A334, $A338 */
void wrong_key(void);         /* $A358 */
void side_door(void);         /* $A35E */
void open_crack(void);        /* $A368 */
void web(void);               /* $A377 */
void wear_ring(void);         /* $A390 */
void take_off_ring(void);     /* $A3BC */
void action_capture(void);    /* $A3E6 */
void goblin(uint8_t here);        /* $A448: here is a register its caller left */
void open_goblin_door(void);  /* $A4C0 */
void close_goblin_door(void); /* $A4D9 */
void say_a4df(void), say_a4f5(void), say_a4fe(void), say_a51c(void), say_a525(void);

#endif
