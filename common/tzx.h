#ifndef HOBBIT_TZX_H
#define HOBBIT_TZX_H

#include <stddef.h>
#include <stdint.h>

/* Entry point of the game code once loaded. */
#define HOBBIT_ENTRY 0x6C00

/* Load The Hobbit v1.2 from a TZX tape image into a 64K memory image.
 * The loading screen goes to $4000 and the game to $6000, then the four
 * block copies the game makes at startup (at $6C00) are applied, matching
 * pobtastic/hobbit's hobbit.t2s. Memory below $4000 is left untouched.
 * Returns 0 on success, or -1 with a message in err. */
int hobbit_load_tzx(const char *path, uint8_t mem[0x10000], char *err, size_t errlen);

uint32_t crc32_buf(const uint8_t *p, size_t n);

#endif
