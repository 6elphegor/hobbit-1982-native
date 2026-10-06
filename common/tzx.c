#include "tzx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_CRC32 0x7bd5495aU /* the 40000-byte "h" block of v1.2 */

uint32_t crc32_buf(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFU;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320U & -(c & 1));
  }
  return ~c;
}

static uint32_t le(const uint8_t *p, int n) {
  uint32_t v = 0;
  for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

/* Collect the flag-$FF data blocks on the tape (checksum byte stripped). */
static int data_blocks(const uint8_t *d, size_t len, const uint8_t **blk, size_t *blen, int max) {
  int count = 0;
  size_t i = 10;
  while (i < len) {
    uint8_t id = d[i++];
    const uint8_t *b = NULL;
    size_t n = 0;
    switch (id) {
    case 0x10: n = le(d + i + 2, 2); b = d + i + 4; i += 4 + n; break;
    case 0x11: n = le(d + i + 15, 3); b = d + i + 18; i += 18 + n; break;
    case 0x14: n = le(d + i + 7, 3); b = d + i + 10; i += 10 + n; break;
    case 0x20: i += 2; break;
    case 0x21: case 0x30: i += 1 + d[i]; break;
    case 0x22: break;
    case 0x32: i += 2 + le(d + i, 2); break;
    default: return -1;
    }
    if (i > len) return -1;
    if (b && n >= 2 && b[0] == 0xFF && count < max) {
      blk[count] = b + 1;
      blen[count] = n - 2;
      count++;
    }
  }
  return count;
}

int hobbit_load_tzx(const char *path, uint8_t mem[0x10000], char *err, size_t errlen) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    snprintf(err, errlen, "cannot open %s", path);
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *d = malloc(len);
  if (!d || fread(d, 1, len, f) != (size_t)len) {
    fclose(f);
    free(d);
    snprintf(err, errlen, "cannot read %s", path);
    return -1;
  }
  fclose(f);

  const uint8_t *blk[8];
  size_t blen[8];
  int n = (len > 10 && memcmp(d, "ZXTape!\x1a", 8) == 0) ? data_blocks(d, len, blk, blen, 8) : -1;
  if (n < 0) {
    snprintf(err, errlen, "%s: not a TZX file, or unsupported block type", path);
    free(d);
    return -1;
  }
  if (n < 3 || blen[1] != 6912 || blen[2] != 40000 || crc32_buf(blk[2], 40000) != GAME_CRC32) {
    snprintf(err, errlen, "%s: not The Hobbit v1.2 (1982, Melbourne House)", path);
    free(d);
    return -1;
  }
  memcpy(mem + 0x4000, blk[1], 6912);
  memcpy(mem + 0x6000, blk[2], 40000);
  free(d);

  static const uint16_t moves[4][3] = {
      {0xC11B, 0x0615, 0xF400}, {0xBA8A, 0x05D9, 0xFA15},
      {0xB6EB, 0x001D, 0x5F00}, {0xCA84, 0x00BF, 0x5F1D}};
  for (int k = 0; k < 4; k++) memmove(mem + moves[k][2], mem + moves[k][0], moves[k][1]);
  return 0;
}
