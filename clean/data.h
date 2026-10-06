/* The game's data, for the clean edition.
 *
 * The data is the tape's: it lives in the game's 64K memory image, in its
 * own formats, and the clean code reads and writes it there through the
 * types and names below rather than through bare addresses. (Keeping it
 * in place keeps saved games compatible, and lets a run be compared with
 * the faithful port byte for byte.) Records are packed structs laid over
 * the image; words in them are little-endian, as on the host. */
#ifndef HOBBIT_CLEAN_DATA_H
#define HOBBIT_CLEAN_DATA_H

#include <stdbool.h>
#include <stdint.h>

/* The memory image (set up by the host). */
extern uint8_t *mem;

/* Keep the original's bugs (to check the clean edition against the
 * faithful port); otherwise they are fixed (docs/CLEAN.md lists them). */
extern bool original_bugs;

static inline uint16_t word_at(uint16_t addr) { return (uint16_t)(mem[addr] | mem[(uint16_t)(addr + 1)] << 8); }
static inline void set_word_at(uint16_t addr, uint16_t v) {
  mem[addr] = (uint8_t)v;
  mem[(uint16_t)(addr + 1)] = v >> 8;
}

/* ---------- objects (and characters: they are objects too) ---------- */

enum { OBJECT_NONE = 0xFF, YOU = 0x00, FIRST_CHARACTER = 0x3C, GANDALF = 0x3E, THORIN = 0x3F };

/* Object attributes (ObjectRecord.attrs). */
enum {
  ATTR_VISIBLE = 0x80,
  ATTR_ANIMATE = 0x40, /* a character */
  ATTR_OPEN = 0x20,
  ATTR_LIGHT = 0x10,
  ATTR_BROKEN = 0x08, /* broken, or dead */
  ATTR_FULL = 0x04,
  ATTR_LIQUID = 0x02,
  ATTR_LOCKED = 0x01,
};

typedef struct __attribute__((packed)) {
  uint8_t nlocations; /* how many locations it is in (listed in locations[]) */
  uint8_t container;  /* the object it is inside or carried by, OBJECT_NONE if none */
  uint8_t size;
  uint8_t weight;
  uint8_t kind;       /* bits 4-6: attitude / which "contains" message */
  uint8_t strength;
  uint8_t defence;
  uint8_t attrs;
  uint16_t name[3];   /* dictionary words: adjective, adjective, noun (low 12 bits) */
  uint16_t help;      /* its own description, 0 for none */
  uint8_t locations[]; /* then (action, handler word) triples to $FF */
} ObjectRecord;

#define OBJECT_INDEX 0xC063 /* [id][record address] triples, $FF ends */

/* ---------- locations ---------- */

enum { LOC_LIT = 0x80, LOC_SCORED = 0x40 };

typedef struct __attribute__((packed)) {
  uint8_t attrs;
  uint8_t capacity;
  uint16_t name[3];
  uint16_t description;
  uint8_t exits[]; /* [direction][door object or 0][destination] triples, $FF ends */
} LocationRecord;

#define LOCATION_TABLE 0xB9E0 /* a word per location, $00-$4F, pointing at its record */
#define LOCATION_COUNT 0x50

enum { DIR_N = 1, DIR_S, DIR_E, DIR_W, DIR_NE, DIR_NW, DIR_SE, DIR_SW, DIR_UP, DIR_DOWN };

/* ---------- characters, events ---------- */

typedef struct __attribute__((packed)) {
  uint8_t id;
  uint8_t entries;
  uint16_t current;  /* the entry of its script table in use */
  uint16_t table;    /* [key][script address] pairs */
  uint8_t spare;
} CharacterRecord;
#define CHARACTER_TABLE 0xCACB /* $FF ends */

typedef struct __attribute__((packed)) {
  uint8_t active;
  uint8_t countdown;
  uint16_t at_zero;  /* routine run when the countdown reaches zero */
  uint8_t threshold;
  uint16_t below;    /* routine run while below the threshold */
} TimedEvent;
#define TIMED_EVENTS 0xCA84 /* $FF ends */

#define LOCATION_EVENTS 0xC78E /* [location][routine] triples, $FF ends */
#define DEFAULT_ACTIONS 0xC730 /* [action][routine] triples, $FF ends */
#define SPEECH_SLOTS 0xB738    /* 8 slots of $19 bytes: what characters were told */

/* ---------- variables ---------- */

/* Each is a byte (or word) of the image at the original address; the
 * names say what is known about them. */
#define V_ACTION 0xB6E7      /* the action of the command being carried out */
#define V_OBJECT1 0xB6E8
#define V_OBJECT2 0xB6E9
#define V_ACTOR 0xB6EA       /* who is acting: YOU or a character */
#define V_YOUR_LOCATION 0xB6F5
#define V_ACTOR_LOCATION 0xB6F6
#define V_DOING 0xB6FA       /* 1: carry the action out; 0: only try it */
#define V_DONE 0xB6FB        /* set when an action could be done */
#define V_PRINT_TO_INPUT 0xB701
#define V_OUTPUT_ON 0xB702
#define V_GRAPHICS 0xB707    /* 0: text only */
#define V_OBJECT1_RECORD 0xB708
#define V_OBJECT2_RECORD 0xB70A
#define V_ACTOR_RECORD 0xB70C
#define V_RANDOM 0xB70E      /* the last random number (the seed) */
#define V_RANDOM_STATE 0xB712 /* the generator's state, a word, high byte first */
#define V_QUESTION 0xB71A    /* a "which ...?" question is waiting for an answer */
#define V_IN_QUOTES 0xB71B

#endif
