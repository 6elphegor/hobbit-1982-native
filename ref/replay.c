/* hobbit-replay: rerun one routine call from a state saved by
 * hobbit-ref --dump-mismatch, and print the original's path: every
 * instruction that is not the one following the last (a jump, call or
 * return), until it leaves the routine's frame.
 *
 *   hobbit-replay STATE [MAX_STEPS] */
#include <stdio.h>
#include <stdlib.h>

#include "spectrum.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: hobbit-replay STATE [MAX_STEPS]\n");
    return 2;
  }
  static Spectrum s;
  spec_setup(&s);
  FILE *f = fopen(argv[1], "rb");
  uint8_t r[15];
  if (!f || fread(s.mem, 1, 65536, f) != 65536 || fread(r, 1, 15, f) != 15) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  fclose(f);
  unsigned long max = argc > 2 ? strtoul(argv[2], NULL, 0) : 2000000;
  z80 *z = &s.cpu;
  z->a = r[0], z->b = r[1], z->c = r[2], z->d = r[3], z->e = r[4], z->h = r[5], z->l = r[6];
  z->ix = r[7] | r[8] << 8, z->iy = r[9] | r[10] << 8;
  z->sp = r[11] | r[12] << 8, z->pc = r[13] | r[14] << 8;
  uint16_t entry_sp = z->sp, prev = 0;
  printf("$%04X\n", z->pc);
  for (unsigned long n = 0; n < max && z->sp <= entry_sp; n++) {
    prev = z->pc;
    spec_step(&s);
    if (z->pc < prev || z->pc > prev + 4)
      printf("$%04X  (from $%04X)  A=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X IX=%04X IY=%04X SP=%04X\n", z->pc,
             prev, z->a, z->b, z->c, z->d, z->e, z->h, z->l, z->ix, z->iy, z->sp);
  }
  printf("end: PC=$%04X SP=$%04X\n", z->pc, z->sp);
  return 0;
}
