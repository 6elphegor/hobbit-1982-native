/* The clean edition: the routines the game's data names.
 *
 * The data names routines by their address in the original: each
 * object's handlers for actions, the default handler of each action, the
 * timed events', the locations' (on arriving), and those in the
 * characters' scripts. The original jumps to the address through
 * TriggerAction ($9B6C); here it is looked up in the table below, which
 * holds every address the tape's data names (a scan of its tables and
 * scripts), and the other routines reached the same way. See docs/CLEAN.md. */
#include "handlers.h"

#include <stddef.h>

#include "actions.h"
#include "characters.h"
#include "objects.h"
#include "platform.h"

/* The keys of the three locks ($A330, $A334, $A338). */
enum { KEY_SIDE = 0x02, KEY_ROCK = 0x04, KEY_RED = 0x0F };

static void lock_with_side_key(void) { lock_with(KEY_SIDE); }
static void lock_with_red_key(void) { lock_with(KEY_RED); }
static void lock_with_rock_key(void) { lock_with(KEY_ROCK); }

/* $94D6: may the action be tried (V_DONE). */
static void may_try(void) { (void)may_try_action(); }

/* $9B02: where you are, noted (and whether it is dark). */
static void note_location(void) { (void)note_your_location(); }

/* $A448: the original compares your location with the B register its
 * caller left: 0 on every call traced in the faithful port (docs/CLEAN.md). */
static void goblin_returns(void) { goblin(0); }

typedef struct {
  uint16_t addr;
  void (*run)(void);
} Routine;

static const Routine ROUTINES[] = {
    {0x8C4B, action_look}, /* default action */
    {0x8CA6, action_put_down}, /* default action */
    {0x8CE0, take_from}, /* object 1F action 14; object 21 action 14; object 25 action 1 */
    {0x8D33, action_take}, /* default action */
    {0x8D9D, action_go}, /* default action */
    {0x8EEC, look_through}, /* object 01 action 18; object 05 action 18; object 06 action 1 */
    {0x8EF8, look_across}, /* object 09 action 19; object 2A action 19 */
    {0x8F3B, go_through}, /* object 01 action 1E; object 05 action 1E; object 06 action 1 */
    {0x8F69, put_in}, /* object 13 action 23 */
    {0x8FAD, action_run}, /* default action */
    {0x8FCD, action_enter}, /* default action */
    {0x8FD6, action_follow}, /* default action */
    {0x8FF5, action_throw_at}, /* default action */
    {0x9034, action_talk_to}, /* default action */
    {0x9065, open_or_close}, /* object 1D action 39 */
    {0x9076, action_shoot}, /* default action */
    {0x90D2, you_are_dead}, /* object 00 action 00 */
    {0x90EB, action_inventory}, /* default action */
    {0x910E, open_object}, /* object 01 action 10; object 05 action 10; object 08 action 1 */
    {0x9138, close_object}, /* object 01 action 0C; object 05 action 0C; object 06 action 0 */
    {0x9171, action_attack}, /* default action; object 3D action 0F; object 3F action 0F; ob */
    {0x924F, put_into}, /* object 13 action 11; object 1D action 11; object 1F action 1 */
    {0x929C, eat}, /* object 14 action 21; object 15 action 21 */
    {0x92B5, eat_food}, /* object 00 action 1B; object 22 action 1B; object 26 action 1 */
    {0x92ED, break_object}, /* object 01 action 0B; object 05 action 0B; object 07 action 0 */
    {0x939E, action_give}, /* default action */
    {0x93DA, examine}, /* default action */
    {0x9404, empty_out}, /* object 13 action 22 */
    {0x9428, river_sweeps_away}, /* object 09 action 0E; object 09 action 11; object 2A action 0 */
    {0x94A4, send_through_door}, /* object 01 action 2C; object 05 action 2C; object 08 action 2 */
    {0x9B02, note_location}, /* default action */
    {0xA244, do_nothing}, /* object 17 action 21 */
    {0xA248, action_tie}, /* default action; object 12 action 2E */
    {0xA2B4, action_untie}, /* default action */
    {0xA2CD, swim_river}, /* object 2A action 32 */
    {0xA302, action_burn}, /* default action */
    {0xA310, swim_black_river}, /* object 09 action 32 */
    {0xA328, drink_black_water}, /* object 16 action 21; object 18 action 21 */
    {0xA330, lock_with_side_key}, /* object 0B action 26 */
    {0xA334, lock_with_red_key}, /* object 08 action 25; object 08 action 26 */
    {0xA338, lock_with_rock_key}, /* object 01 action 25; object 01 action 26 */
    {0xA358, wrong_key}, /* object 05 action 25; object 05 action 26; object 1E action 2 */
    {0xA35E, side_door}, /* object 0B action 00 */
    {0xA368, open_crack}, /* object 06 action 10 */
    {0xA377, web}, /* object 07 action 00 */
    {0xA390, wear_ring}, /* object 10 action 28 */
    {0xA3BC, take_off_ring}, /* object 10 action 16 */
    {0xA3E6, action_capture}, /* default action */
    {0xA448, goblin_returns}, /* object 3D action 00; object 45 action 00; object 49 action 0 */
    {0xA4C0, open_goblin_door}, /* object 11 action 10 */
    {0xA4D9, close_goblin_door}, /* timed (zero) */
    {0xA4DF, say_a4df}, /* script of 3E */
    {0xA4F5, say_a4f5}, /* script of 3E */
    {0xA4FE, say_a4fe}, /* script of 3E */
    {0xA51C, say_a51c}, /* script of 3F */
    {0xA525, say_a525}, /* script of 41 */
    {0xA541, climb_out}, /* default action */
    {0xA55F, climb_in}, /* object 13 action 36; object 25 action 36; object 29 action 3 */
    {0xA5D1, warg_howls}, /* script of 43 */
    {0xA5E2, barrel_in_river}, /* object 0C action 00 */
    {0xA5FB, barrel_ashore}, /* timed (zero) */
    {0xA640, wheres_the_thief}, /* script of 3F */
    {0xA657, thorin_talks}, /* script of 3F */
    {0xA67E, reach_from_cellar}, /* object 0C action 0C; object 0C action 10 */
    {0xA698, follows_you_in}, /* script of 00 */
    {0xA6C2, dragon_talks}, /* script of 00 */
    {0xA6DC, dragon_burns}, /* script of 00 */
    {0xA71E, read_runes}, /* object 0D action 1C */
    {0xA73B, breaks_too}, /* object 3F action 00 */
    {0xA761, reach_from_inside}, /* object 1B action 0C; object 1B action 10 */
    {0xA784, reach_from_inside2}, /* object 1B action 0B; object 1B action 18; object 1B action 1 */
    {0xA7AA, bog_sinks}, /* timed (below); timed (zero) */
    {0xA7C4, read_map}, /* object 03 action 1C */
    {0xA814, shoot_at}, /* object 12 action 2B */
    {0xA876, swing_back}, /* object 12 action 31 */
    {0xA89E, swing_arrives}, /* object 29 action 00 */
    {0xA8AB, listens}, /* script of 00 */
    {0xA8D2, asks_riddle}, /* script of 44 */
    {0xA8F6, strangles}, /* script of 44 */
    {0xA926, says_something}, /* script of 44 */
    {0xA94E, trolls_eat}, /* script of 47 */
    {0xA9E5, gives_you}, /* script of 41 */
    {0xAA27, go_into}, /* object 13 action 38 */
    {0xAA5C, mends}, /* timed (zero) */
    {0xAA74, hole_vanishes}, /* timed (zero) */
    {0xAA91, hole_appears}, /* timed (below) */
    {0xAAA2, hole_closes}, /* object 0B action 00 */
    {0xAAB3, door_opens}, /* timed (below) */
    {0xAAD5, door_closes}, /* timed (zero) */
    {0xAAE0, door_lets_out}, /* timed (zero) */
    {0xAAF9, starts_timer}, /* object 14 action 00 */
    {0xAB0B, timer_ends}, /* timed (zero) */
    {0xAB10, web_smothers}, /* timed (zero) */
    {0xAB1F, eyes_stare}, /* timed (below) */
    {0xAB3A, something_stings}, /* timed (zero) */
    {0xC7A4, at_beorns_house}, /* location event */
    {0xC7B2, at_spider_threads}, /* location event */
    {0xC7B9, at_deep_bog}, /* location event */
    {0xC7C0, at_elvenkings_cellar}, /* location event */
    {0xC7DD, at_forest}, /* location event */
    /* Not found by the scan (reached through other characters' scripts,
     * or by the routines' own callers), but reached the same way. */
    {0x8DAB, move_actor},
    {0x946D, lock_object},
    {0x948D, unlock_object},
    {0x94D6, may_try},
    {0x950F, do_action},
    {0x96B3, end_of_turn},
    {0x977C, kill_first_object},
    {0x97AD, new_game_characters},
    {0x980E, characters_act},
    {0xA971, day_dawns},
    {0xA9BD, trolls_talk},
    {0xA9D6, check_game_won},
};

void run_location_event(uint16_t addr, uint8_t location) {
  if (addr == 0xC7EA) /* the only one that uses the location: it passes it on to arrive */
    at_forest_river(location);
  else
    run_routine(addr);
}

void run_routine(uint16_t addr) {
  if (addr == 0) return; /* none (TriggerAction tests for it) */
  for (size_t i = 0; i < sizeof ROUTINES / sizeof ROUTINES[0]; i++)
    if (ROUTINES[i].addr == addr) {
      ROUTINES[i].run();
      return;
    }
  device_crash(addr); /* the original would run whatever is there */
}
