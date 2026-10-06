/* The clean edition: running the routines the game's data names by
 * their address in the original (see handlers.c). */
#ifndef HOBBIT_CLEAN_HANDLERS_H
#define HOBBIT_CLEAN_HANDLERS_H

#include <stdint.h>

/* Run the routine at addr in the original (0: none). */
void run_routine(uint16_t addr);
/* Run a location's event (at addr in the original), for the player moved
 * to location: the original keeps it in B for the routine. */
void run_location_event(uint16_t addr, uint8_t location);

#endif
