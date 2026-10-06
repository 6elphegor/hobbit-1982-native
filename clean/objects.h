/* Objects, locations and characters (clean edition): lookups,
 * containment, sizes and weights, what can be seen, exits, and the lists
 * of objects printed by "You see". */
#ifndef HOBBIT_CLEAN_OBJECTS_H
#define HOBBIT_CLEAN_OBJECTS_H

#include "clean.h"

/* ---------- variables this module uses (not in data.h) ---------- */

#define V_DARK 0x980C          /* 1 when your location is dark (2: ?) */
#define V_MOVED 0x9BDC         /* a byte in the code: set while contents are moved, cleared if you were among them */
#define V_LOOK_LOCATION 0x8D9B /* a byte in the code: the location the look at $8E12 describes */
#define V_MATCH_KIND 0xB710    /* what match_object looks for: 0 objects, 1 characters, 2 either */
#define V_MATCH_UNSEEN 0xB70F  /* nonzero: match_object matches objects the actor cannot see */
#define V_OBJECT1_PLURAL 0xB6FE /* unknown: the first object was given as a pronoun or plural? */
#define V_OBJECT2_PLURAL 0xB6FF
#define V_NOUN_ONLY 0xB703     /* print names without their adjectives (and articles) */
#define V_CAPITAL 0xB704       /* the next word starts with a capital */
#define V_INDENT 0x869F        /* the text window's indentation */

#define DIRECTION_WORDS 0xA20E /* a dictionary word per direction, from 0 */
#define CONTAINS_PHRASES 0xAFCA /* 4-byte messages: "in it", "on it", ... */
#define CHARACTER_END 0xFF

/* The address in the memory image of a record. */
static inline uint16_t addr_of(const void *p) { return (uint16_t)((const uint8_t *)p - mem); }
static inline ObjectRecord *object_at(uint16_t addr) { return (ObjectRecord *)&mem[addr]; }
static inline LocationRecord *location_at(uint16_t addr) { return (LocationRecord *)&mem[addr]; }
/* Location i of an object (i may run past nlocations, as the original's
 * loops do; the address wraps at 64K as on the Spectrum). */
static inline uint8_t object_location(const ObjectRecord *o, unsigned i) {
  return mem[(uint16_t)(addr_of(o) + 0x10 + i)];
}

/* ---------- tables of 3-byte entries: [key][word] ... $FF ---------- */

/* $9DBD: the entry of the table with this key, or the $FF that ends it. */
uint16_t find_entry(uint16_t table, uint8_t key);
/* $9B93: the entry after this one. */
static inline uint16_t next_entry(uint16_t entry) { return (uint16_t)(entry + 3); }
static inline uint8_t entry_key(uint16_t entry) { return mem[entry]; }
static inline uint16_t entry_word(uint16_t entry) { return word_at((uint16_t)(entry + 1)); }

/* ---------- objects, locations, characters ---------- */

/* $9BCA: the record of object id. Not in the index: the word after the
 * index's end ($FF01, a bogus record), as in the original. */
uint16_t object_record_addr(uint8_t id);
static inline ObjectRecord *object_record(uint8_t id) { return object_at(object_record_addr(id)); }
/* $9BB1: the record of location loc; NULL if loc >= $50 (the original then
 * leaves its pointer as it was: callers pass what that was). */
LocationRecord *location_record(uint8_t loc);
/* $9D37: the record of the actor's location (the actor's own record if
 * that location is out of range). */
uint16_t actor_location_addr(void);
/* $9F82: the location of object id if it is in exactly one, else $FF. */
uint8_t object_first_location(uint8_t id);
/* $9B81: the entry for action in the object's (action, handler) table,
 * or its $FF end. */
uint16_t object_action_entry(const ObjectRecord *o, uint8_t action);
/* $9A85: the character table record of character id (or its $FF end). */
uint16_t find_character(uint8_t id);
/* $9AA0: make the entry with this key the character's current one. */
void character_set_entry(uint8_t id, uint8_t key);

/* ---------- containment, sizes, weights ---------- */

/* $9C7E: is obj inside container (at any depth)? obj $FF: yes. Not
 * inside: *outermost (if not NULL) is the outermost container found. Loops
 * for ever if containers form a cycle. */
bool object_inside(uint8_t obj, uint8_t container, uint8_t *outermost);
/* $9C7B: is obj carried (at any depth) by the actor? */
bool object_carried(uint8_t obj);
/* $9C17: everything inside obj (at any depth) is now at location loc. */
void set_contents_location(uint8_t obj, uint8_t loc);
/* $9BDD: obj has moved to loc: move what it holds; if that took you, look
 * around. look_record is the record whose location the look sets again
 * (the original's IY, left by its caller). */
void object_moved(uint8_t obj, uint8_t loc, uint16_t look_record);
/* $9C41: the room left at location loc (capacity less the sizes of what
 * is there), 0 if over. Out of range: the capacity of fallback. */
uint8_t location_room(uint8_t loc, uint16_t fallback);
/* $9D00: total + the sizes (sizes) or weights (counting what they hold)
 * of what obj holds; $FF on signed overflow. */
uint8_t sum_contents(uint8_t obj, bool sizes, uint8_t total);
/* $9CE8, $9CED */
uint8_t contents_size(uint8_t obj);
uint8_t contents_weight(uint8_t obj);
/* $9D53: obj has gone: what it held goes to its container; liquids
 * evaporate. Returns obj's container. */
uint8_t empty_object(uint8_t obj);
/* $9D97: how many visible objects obj holds (directly). */
uint8_t object_count(uint8_t obj);

/* ---------- seeing ---------- */

/* $9E7A: the innermost container of the object that cannot be seen into,
 * or $FF. Loops for ever if open containers form a cycle. */
uint8_t opaque_container(const ObjectRecord *o);
/* $9E40: can viewer see object obj (record o)? */
bool can_see(const ObjectRecord *viewer, uint8_t obj, const ObjectRecord *o);
/* $9E34: can the actor see it? */
bool actor_can_see(uint8_t obj, const ObjectRecord *o);
/* $9DD9: from index entry *entry on, the next object whose name matches the
 * words at words, of the kind V_MATCH_KIND asks for, that the actor can see
 * (unless V_MATCH_UNSEEN). Returns its id, $FF at the end; *entry is at it. */
uint8_t match_object(uint16_t *entry, uint16_t words);

/* ---------- exits ---------- */

/* $9E95: the actor's location record + 7: next_entry() gives its first exit. */
uint16_t actor_exits(void);
/* $A0AE: the same for location loc (fallback if out of range). */
uint16_t location_exits(uint8_t loc, uint16_t fallback);
/* $9F08: the actor's exit in direction dir that leads somewhere; its entry
 * (or the $FF end) in *exit. */
bool find_exit_direction(uint8_t dir, uint16_t *exit);
/* $9F28 / $9F2D: the actor's exit through door / to location dest. */
bool find_exit_door(uint8_t door, uint16_t *exit);
bool find_exit_to(uint8_t dest, uint16_t *exit);
/* $9EA0: from exit *exit on, the next whose destination's name matches the
 * words at words. Returns the destination, $FF at the end. */
uint8_t match_exit(uint16_t *exit, uint16_t words);
/* $A124: the next exit after this one with no door (and a direction); the
 * $FF end if none. */
uint16_t next_open_exit(uint16_t exit);
/* $A0BA: the dictionary word for direction dir. */
uint16_t direction_word(uint8_t dir);
/* $A0BD: word (n & $7F) of the table at table. */
uint16_t table_word(uint16_t table, uint8_t n);

/* ---------- printing ---------- */

/* $9ED6: print a name (three words at addr: noun, adjective, adjective). */
void print_name(uint16_t addr);
/* $9EC7: print an object's name. */
void print_object_name(const ObjectRecord *o);
/* $A09D: the "contains" phrase of an object ("in it", ...), a message. */
uint16_t contains_phrase(const ObjectRecord *o);
/* $9ACD: when obj is mentioned in a turn in which things moved, and it is
 * now at your location (not at old_location), print msg with its name. */
void mention_object(uint8_t obj, uint8_t old_location, uint16_t msg);
/* $9B02: note your location and whether it is dark. Returns dark. */
bool note_your_location(void);
/* $9B44: is the actor one of the command's objects? */
bool actor_is_object(void);
/* $9D44: true if the action is being carried out; else notes that it
 * could be ($B6FB) and the caller gives up. */
bool output_check(void);
/* $9F76: the action is done: describe it ($712B) if carrying it out,
 * else note that it could not be. */
void finish_action(void);
/* $9FC7: list the objects in container (or $FF: lying at location loc)
 * that the actor can see, indented by indent, with what they hold.
 * Returns how many were listed (nested ones too). */
uint8_t list_objects(uint8_t container, uint8_t loc, uint8_t indent);
/* $9FAF: the same at the left margin, or "nothing". Returns the count. */
uint8_t list_contents(uint8_t container, uint8_t loc);
/* $9F94: "You see :" and what is at the actor's location. */
void list_you_see(void);
/* $A050: if obj can be seen into and holds visible things, say what it
 * holds and return true; else a new line and false. */
bool list_inside(uint8_t obj);
/* $A0C8: for each door of location loc that can be seen, "to the <dir>
 * there is <door>". fallback: as for location_exits. */
void list_doors(uint8_t loc, uint16_t fallback);

#endif
