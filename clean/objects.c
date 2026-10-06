/* The clean edition: objects, locations and characters ($9A85-$A137).
 * See docs/CLEAN.md and objects.h.
 *
 * Records stay in the memory image (clean/data.h). The object index
 * (OBJECT_INDEX) and most tables are runs of 3-byte entries [key][word]
 * ending with a $FF key. Loops over every object start at the index's
 * first entry (object 0, "you"), except sum_contents, which skips it.
 *
 * The original's quirks are kept, since they decide what the game does:
 * an unknown object id gives the word after the index as its record; a
 * location id >= $50 leaves the caller's pointer as it was; containers
 * that form a cycle make the containment walks loop for ever; a door
 * list runs once before testing for the end of the exits. */
#include <stddef.h>

#include "objects.h"

#include "actions.h"
#include "characters.h"
#include "screen.h"
#include "text.h"

/* ---------- other modules ---------- */

/* $71F3: do the two words at name match the phrase at words? */
static bool words_match(uint16_t name, uint16_t words) {
  bool swapped;
  return phrase_matches(words, name, &swapped);
}

/* ---------- tables ---------- */

uint16_t find_entry(uint16_t table, uint8_t key) {
  uint16_t e = table;
  while (mem[e] != key && mem[e] != 0xFF) e = next_entry(e);
  return e;
}

/* ---------- objects, locations, characters ---------- */

uint16_t object_record_addr(uint8_t id) { return entry_word(find_entry(OBJECT_INDEX, id)); }

LocationRecord *location_record(uint8_t loc) {
  if (loc >= LOCATION_COUNT) return NULL;
  return location_at(word_at((uint16_t)(LOCATION_TABLE + 2 * loc)));
}

uint16_t actor_location_addr(void) {
  uint16_t actor = word_at(V_ACTOR_RECORD);
  LocationRecord *l = location_record(object_location(object_at(actor), 0));
  return l ? addr_of(l) : actor;
}

uint8_t object_first_location(uint8_t id) {
  if (id == OBJECT_NONE) return OBJECT_NONE;
  ObjectRecord *o = object_record(id);
  return o->nlocations == 1 ? object_location(o, 0) : OBJECT_NONE;
}

uint16_t object_action_entry(const ObjectRecord *o, uint8_t action) {
  /* The table follows the locations (an 8-bit offset, as in the original). */
  uint8_t skip = (uint8_t)(o->nlocations + 0x10);
  return find_entry((uint16_t)(addr_of(o) + skip), action);
}

uint16_t find_character(uint8_t id) {
  uint16_t r = CHARACTER_TABLE;
  while (mem[r] != id && mem[r] != CHARACTER_END) r = (uint16_t)(r + sizeof(CharacterRecord));
  return r;
}

void character_set_entry(uint8_t id, uint8_t key) {
  uint16_t r = find_character(id);
  if (mem[r] == CHARACTER_END) return;
  CharacterRecord *ch = (CharacterRecord *)&mem[r];
  uint16_t e = find_entry(ch->table, key);
  if (entry_key(e) == 0xFF) return;
  ch->current = entry_word(e);
}

/* ---------- containment, sizes, weights ---------- */

/* $9C8A: walk out from obj through its containers. */
static bool inside_walk(uint8_t obj, uint8_t container, uint8_t *outermost) {
  for (;;) {
    uint16_t e = find_entry(OBJECT_INDEX, obj);
    uint8_t found = entry_key(e); /* obj, or $FF if it is not in the index */
    uint8_t in = object_at(entry_word(e))->container;
    if (in == OBJECT_NONE) {
      if (outermost) *outermost = found;
      return false;
    }
    if (in == container) {
      if (outermost) *outermost = in;
      return true;
    }
    obj = in;
  }
}

bool object_inside(uint8_t obj, uint8_t container, uint8_t *outermost) {
  if (obj == OBJECT_NONE) {
    if (outermost) *outermost = OBJECT_NONE;
    return true;
  }
  return inside_walk(obj, container, outermost);
}

bool object_carried(uint8_t obj) { return object_inside(obj, mem[V_ACTOR], NULL); }

void set_contents_location(uint8_t obj, uint8_t loc) {
  for (uint16_t e = OBJECT_INDEX; entry_key(e) != 0xFF; e = next_entry(e)) {
    ObjectRecord *o = object_at(entry_word(e));
    if (o->container != obj) continue;
    o->locations[0] = loc;
    if (entry_key(e) == YOU) mem[V_MOVED] = 0;
    set_contents_location(entry_key(e), loc);
  }
}

/* $8E12: the actor (record r, which the look sets to loc again) is now at
 * loc: describe it. */
static void look_at(uint8_t loc, uint16_t r) { move_to(r, loc); }

void object_moved(uint8_t obj, uint8_t loc, uint16_t look_record) {
  mem[V_MOVED] = 1;
  set_contents_location(obj, loc);
  if (mem[V_MOVED]) return; /* you were not inside it */
  if (object_location(object_record(YOU), 0) != 0) {
    uint8_t actor = mem[V_ACTOR];
    uint16_t actor_record = word_at(V_ACTOR_RECORD);
    set_word_at(V_ACTOR_RECORD, object_record_addr(YOU));
    mem[V_ACTOR] = YOU;
    mem[V_LOOK_LOCATION] = loc;
    look_at(loc, look_record);
    note_your_location();
    set_word_at(V_ACTOR_RECORD, actor_record);
    mem[V_ACTOR] = actor;
  }
  mem[V_MOVED] = 0;
}

uint8_t location_room(uint8_t loc, uint16_t fallback) {
  LocationRecord *l = location_record(loc);
  uint8_t room = l ? l->capacity : mem[(uint16_t)(fallback + 1)];
  for (uint16_t e = OBJECT_INDEX; entry_key(e) != 0xFF; e = next_entry(e)) {
    ObjectRecord *o = object_at(entry_word(e));
    if (o->nlocations != 1 || o->locations[0] != loc) continue;
    if (o->size > room) return 0;
    room = (uint8_t)(room - o->size);
  }
  return room;
}

/* a + b, or $FF if that overflows as a signed byte. */
static bool add_overflows(uint8_t a, uint8_t b) {
  uint8_t s = (uint8_t)(a + b);
  return ((a ^ s) & (b ^ s) & 0x80) != 0;
}

uint8_t sum_contents(uint8_t obj, bool sizes, uint8_t total) {
  /* From the entry after object 0's. */
  for (uint16_t e = next_entry(OBJECT_INDEX); entry_key(e) != 0xFF; e = next_entry(e)) {
    ObjectRecord *o = object_at(entry_word(e));
    if (o->container != obj) continue;
    uint8_t v = sizes ? o->size : o->weight;
    if (add_overflows(total, v)) return 0xFF;
    total = (uint8_t)(total + v);
    if (!sizes) total = sum_contents(entry_key(e), false, total);
  }
  return total;
}

uint8_t contents_size(uint8_t obj) { return sum_contents(obj, true, 0); }
uint8_t contents_weight(uint8_t obj) { return sum_contents(obj, false, 0); }

uint8_t empty_object(uint8_t obj) {
  /* The original then compares the containers with what its lookup
   * leaves: the id, or for one that is not an object, $FF, so that
   * everything in nothing is emptied (into what the end of the index
   * names as the container). */
  uint16_t found = find_entry(OBJECT_INDEX, obj);
  uint8_t key = entry_key(found);
  uint8_t to = object_at(entry_word(found))->container;
  for (uint16_t e = OBJECT_INDEX; entry_key(e) != 0xFF; e = next_entry(e)) {
    ObjectRecord *o = object_at(entry_word(e));
    if (o->container != key) continue;
    if (!(o->attrs & ATTR_LIQUID)) {
      o->container = to;
      continue;
    }
    o->locations[0] = 0;
    o->container = OBJECT_NONE;
    o->attrs &= (uint8_t)~ATTR_VISIBLE;
    print_object_name(o);
    print_message(0xB143, NULL); /* "evaporates" */
  }
  return to;
}

uint8_t object_count(uint8_t obj) {
  uint8_t n = 0;
  for (uint16_t e = OBJECT_INDEX; entry_key(e) != 0xFF; e = next_entry(e)) {
    ObjectRecord *o = object_at(entry_word(e));
    if (o->container == obj && (o->attrs & ATTR_VISIBLE)) n++;
  }
  return n;
}

/* ---------- seeing ---------- */

/* Attributes that let you see inside: open, or broken. */
#define SEE_INSIDE (ATTR_OPEN | ATTR_BROKEN)

uint8_t opaque_container(const ObjectRecord *o) {
  for (;;) {
    uint8_t in = o->container;
    if (in == OBJECT_NONE) return OBJECT_NONE;
    o = object_record(in);
    if (!(o->attrs & SEE_INSIDE)) return in;
  }
}

bool can_see(const ObjectRecord *viewer, uint8_t obj, const ObjectRecord *o) {
  if (!(o->attrs & ATTR_VISIBLE)) return false;
  uint8_t around_viewer = opaque_container(viewer);
  if (around_viewer == obj) return true; /* the viewer is inside it */
  uint8_t around_obj = opaque_container(o);
  if (around_obj != around_viewer) return false;
  if (around_obj != OBJECT_NONE) return true; /* in the same closed container */
  /* Both in the open: is it at the viewer's location? (A count of 0 is
   * taken as 256, as by the original's DJNZ.) */
  uint8_t here = object_location(viewer, 0);
  unsigned n = o->nlocations ? o->nlocations : 256;
  for (unsigned i = 0; i < n; i++)
    if (object_location(o, i) == here) return true;
  return false;
}

bool actor_can_see(uint8_t obj, const ObjectRecord *o) {
  return can_see(object_at(word_at(V_ACTOR_RECORD)), obj, o);
}

uint8_t match_object(uint16_t *entry, uint16_t words) {
  uint8_t kind = mem[V_MATCH_KIND];
  uint16_t e = *entry;
  /* A search that has ended stays ended. (The original's bug steps past
   * the end of the index and reads the records after it as entries:
   * objects that do not exist, $FF among them, which lead to crashes.)
   * Searches start before the first entry, never on an $FF. */
  if (entry_key(e) == 0xFF && !original_bugs) return 0xFF;
  for (;;) {
    e = next_entry(e);
    if (entry_key(e) == 0xFF) break;
    ObjectRecord *o = object_at(entry_word(e));
    if (kind != 2) {
      bool character = (o->attrs & (ATTR_ANIMATE | ATTR_BROKEN)) == ATTR_ANIMATE;
      if (character != kind) continue;
    }
    if (!words_match((uint16_t)(addr_of(o) + 8), words)) continue;
    if (mem[V_MATCH_UNSEEN] || actor_can_see(entry_key(e), o)) break;
  }
  *entry = e;
  return entry_key(e);
}

/* ---------- exits ---------- */

/* Exits are entries [direction][door][destination] from +$0A of the
 * location record; these return the record + 7, so that next_entry gives
 * the first. */
uint16_t actor_exits(void) { return (uint16_t)(actor_location_addr() + 7); }

uint16_t location_exits(uint8_t loc, uint16_t fallback) {
  LocationRecord *l = location_record(loc);
  return (uint16_t)((l ? addr_of(l) : fallback) + 7);
}

bool find_exit_direction(uint8_t dir, uint16_t *exit) {
  uint16_t e = actor_exits();
  for (;;) {
    e = next_entry(e);
    if (mem[e] == 0xFF) break;
    if (mem[(uint16_t)(e + 2)] != 0 && mem[e] == dir) break;
  }
  *exit = e;
  return mem[e] != 0xFF;
}

/* The actor's first exit whose byte at offset is v. */
static bool find_exit_by(unsigned offset, uint8_t v, uint16_t *exit) {
  uint16_t e = actor_exits();
  for (;;) {
    e = next_entry(e);
    if (mem[e] == 0xFF) break;
    if (mem[(uint16_t)(e + offset)] == v) break;
  }
  *exit = e;
  return mem[e] != 0xFF;
}

bool find_exit_door(uint8_t door, uint16_t *exit) { return find_exit_by(1, door, exit); }
bool find_exit_to(uint8_t dest, uint16_t *exit) { return find_exit_by(2, dest, exit); }

uint8_t match_exit(uint16_t *exit, uint16_t words) {
  uint16_t e = *exit;
  uint8_t dest = 0xFF;
  if (mem[e] == 0xFF && !original_bugs) return 0xFF; /* ended: as match_object */
  for (;;) {
    e = next_entry(e);
    if (mem[e] == 0xFF) break;
    LocationRecord *l = location_record(mem[(uint16_t)(e + 2)]);
    /* A destination out of range: the original looks at the exit itself. */
    uint16_t name = (uint16_t)((l ? addr_of(l) : e) + 2);
    if (words_match(name, words)) {
      dest = mem[(uint16_t)(e + 2)];
      break;
    }
  }
  *exit = e;
  return dest;
}

uint16_t next_open_exit(uint16_t exit) {
  for (;;) {
    exit = next_entry(exit);
    if (mem[exit] == 0xFF) return exit;
    if (mem[(uint16_t)(exit + 1)] == 0 && mem[exit] != 0) return exit;
  }
}

uint16_t table_word(uint16_t table, uint8_t n) { return word_at((uint16_t)(table + 2 * (n & 0x7F))); }

uint16_t direction_word(uint8_t dir) { return table_word(DIRECTION_WORDS, dir); }

/* ---------- printing ---------- */


void print_name(uint16_t addr) {
  uint16_t noun = word_at(addr);
  if (mem[V_NOUN_ONLY] == 0) {
    print_article(noun);
    print_token(word_at((uint16_t)(addr + 2)));
    print_token(word_at((uint16_t)(addr + 4)));
  }
  if (noun != 0) print_noun_token(noun);
}

void print_object_name(const ObjectRecord *o) { print_name((uint16_t)(addr_of(o) + 8)); }

uint16_t contains_phrase(const ObjectRecord *o) {
  /* Bits 4-7 of kind, times 4 (RLCA twice, AND $3C). */
  uint8_t k = (uint8_t)(o->kind << 2 | o->kind >> 6) & 0x3C;
  return (uint16_t)(CONTAINS_PHRASES + k);
}

void mention_object(uint8_t obj, uint8_t old_location, uint16_t msg) {
  if (obj == OBJECT_NONE || obj == YOU) return;
  if (mem[V_DARK] == 2) return;
  uint8_t loc = object_first_location(obj);
  if (loc == old_location || loc != mem[V_YOUR_LOCATION]) return;
  if (mem[V_MOVED] == 0) return;
  mem[V_OUTPUT_ON] = 1;
  uint16_t name = (uint16_t)(object_record_addr(obj) + 8);
  print_message(msg, &name);
}

bool note_your_location(void) {
  mem[V_YOUR_LOCATION] = object_first_location(YOU);
  bool dark = player_in_dark();
  mem[V_DARK] = dark;
  return dark;
}

bool actor_is_object(void) {
  uint8_t actor = mem[V_ACTOR], obj1 = mem[V_OBJECT1], obj2 = mem[V_OBJECT2];
  if (obj1 == OBJECT_NONE) return false;
  if (mem[V_OBJECT1_PLURAL] == 0) {
    if (actor == obj1) return true;
    if (obj2 == obj1) return true; /* sic: the original compares the second object with the first */
  }
  if (mem[V_OBJECT2_PLURAL] != 0) return false;
  return obj2 == actor;
}

bool output_check(void) {
  uint8_t doing = mem[V_DOING];
  if (doing == 1) return true;
  mem[V_DONE] = (uint8_t)(doing + 1);
  return false;
}

void finish_action(void) {
  if (mem[V_DOING] == 1) {
    describe_action();
  } else {
    mem[V_DONE] = 0;
  }
}

/* ---------- lists of objects ---------- */

#define TOP_INDENT 4

bool list_inside(uint8_t obj) {
  ObjectRecord *o = object_record(obj);
  uint8_t n;
  if (!(o->attrs & SEE_INSIDE) || (n = object_count(obj)) == 0) {
    print_newline();
    return false;
  }
  if (o->attrs & ATTR_ANIMATE) {
    uint16_t who = (uint16_t)(obj << 8); /* the character, for "is"/"are" */
    print_message(0xADF9, &who);         /* "<A> is carrying" */
  } else {
    print_message(contains_phrase(o), NULL);
    uint16_t args[2] = {o->name[0] & 0x0FFF, n == 1 ? 0x039B : 0x0065}; /* its noun; "is" / "are" */
    print_message(0xAFE0, args); /* "<...> there <...>" */
  }
  return true;
}

uint8_t list_objects(uint8_t container, uint8_t loc, uint8_t indent) {
  uint8_t count = 0;
  uint8_t old_indent = mem[V_INDENT];
  mem[V_INDENT] = indent;
  for (uint16_t e = OBJECT_INDEX; entry_key(e) != 0xFF; e = next_entry(e)) {
    ObjectRecord *o = object_at(entry_word(e));
    uint8_t id = entry_key(e);
    if (o->container != container) continue;
    /* Lying loose: only things in one location. Then at loc (as its
     * first location, or its second if it is in more than one; a count
     * of 0 counts as more). */
    if (container == OBJECT_NONE && o->nlocations != 1) continue;
    if (o->locations[0] != loc && (o->nlocations == 1 || o->locations[1] != loc)) continue;
    bool actor = id == mem[V_ACTOR];
    if (actor && indent == TOP_INDENT) continue;
    if (!actor_can_see(id, o)) continue;
    count++;
    mem[V_CAPITAL] = 0;
    mem[V_NOUN_ONLY] = 0;
    print_object_name(o);
    if (actor) {
      print_newline();
      continue;
    }
    print_char('.');
    if (!list_inside(id)) continue;
    count = (uint8_t)(count + list_objects(id, loc, (uint8_t)(indent + 2)));
  }
  mem[V_INDENT] = old_indent;
  return count;
}

uint8_t list_contents(uint8_t container, uint8_t loc) {
  uint8_t n = list_objects(container, loc, TOP_INDENT);
  if (n == 0) print_message(0xB33B, NULL); /* "nothing" */
  return n;
}

void list_you_see(void) {
  print_message(0xB003, NULL); /* "You see :" */
  list_contents(OBJECT_NONE, object_location(object_at(word_at(V_ACTOR_RECORD)), 0));
}

void list_doors(uint8_t loc, uint16_t fallback) {
  /* The first exit is looked at before the test for the end. */
  uint16_t e = next_entry(location_exits(loc, fallback));
  do {
    uint8_t door = mem[(uint16_t)(e + 1)];
    if (door != 0) {
      ObjectRecord *d = object_record(door);
      if (d->attrs & ATTR_VISIBLE) {
        uint16_t name = (uint16_t)(addr_of(d) + 8); /* the door's name, for the last message */
        uint8_t dir = mem[e];
        uint16_t word = direction_word(dir);
        if (dir < DIR_UP) print_message(0xAE17, NULL); /* "to the" */
        else if (dir == DIR_UP) word = 0x07B5;
        else word = 0x082B;
        print_token(word);
        print_message(0xB013, &name); /* "there is <door>" */
      }
    }
    e = next_entry(e);
  } while (mem[e] != 0xFF);
}

#ifdef CLEAN_ADAPTERS /* the faithful port's callers: not in the clean edition alone */

/* ======================================================================
 * Adapters, for the faithful callers while they remain. Each takes its
 * routine's inputs from the registers, calls the clean code, and puts the
 * results where the callers look (the outputs mask: what the callers use,
 * from tools/refs.py). Everything else is left as it was on entry.
 * ====================================================================== */

static void restore_regs(Cpu *c, const Cpu *s) {
  c->a = s->a, c->b = s->b, c->c = s->c, c->d = s->d, c->e = s->e, c->h = s->h, c->l = s->l;
  c->b_ = s->b_, c->c_ = s->c_, c->d_ = s->d_, c->e_ = s->e_, c->h_ = s->h_, c->l_ = s->l_;
  c->ix = s->ix, c->iy = s->iy;
  c->sf = s->sf, c->zf = s->zf, c->hf = s->hf, c->pf = s->pf, c->nf = s->nf, c->cf = s->cf;
  c->yf = s->yf, c->xf = s->xf;
}

/* Run body (clean code) with the registers kept, then set the outputs. */
#define CLEAN(c, body) \
  do { \
    Cpu keep_ = *(c); \
    body; \
    restore_regs((c), &keep_); \
  } while (0)

/* Flags of CP $FF with A = v (the end-of-table test). */
static void cp_ff(Cpu *c, uint8_t v) { c->zf = v == 0xFF, c->cf = v != 0xFF; }

/* The refresh register: IndexIdTable's opcode fetches, as the faithful
 * port counts them (the game takes its seed from R). */
static void count_r(Cpu *c, uint16_t table, uint16_t entry) {
  unsigned k = (uint16_t)(entry - table) / 3;
  unsigned n = 7 + 7 * k + (mem[entry] == c->a ? 3 : 5) + 6;
  c->r = (uint8_t)((c->r & 0x80) | ((c->r + n) & 0x7F));
}

/* $9DBD IndexIdTable: IX table, A key -> IX entry, A its key, Z at the end. */
static void a_find_entry(Cpu *c) {
  uint16_t e = find_entry(c->ix, c->a);
  count_r(c, c->ix, e);
  c->ix = e, c->a = entry_key(e);
  cp_ff(c, c->a);
}

/* $9B93 Step3ByteTable: IX on to the next entry, IY its word, A its key. */
static void a_next_entry(Cpu *c) {
  c->ix = next_entry(c->ix);
  c->iy = entry_word(c->ix);
  c->a = entry_key(c->ix);
  cp_ff(c, c->a);
}

/* $9BA9: the same, A kept. */
static void a_next_entry_keep(Cpu *c) {
  c->ix = next_entry(c->ix);
  c->iy = entry_word(c->ix);
  cp_ff(c, entry_key(c->ix));
}

/* $9BCA LocateObject: A id -> IX record, A id ($FF unknown). */
static void a_object_record(Cpu *c) {
  uint16_t e = find_entry(OBJECT_INDEX, c->a);
  count_r(c, OBJECT_INDEX, e);
  c->ix = entry_word(e), c->a = entry_key(e);
  cp_ff(c, c->a);
}

/* $9BB1 LocateLocation: A -> IX; out of range: A=0, Z, IX kept. */
static void a_location_record(Cpu *c) {
  LocationRecord *l = location_record(c->a);
  if (l) {
    c->ix = addr_of(l);
    c->zf = false, c->cf = false; /* NZ from CP $50, NC from the ADD HL that follows */
  } else {
    c->a = 0;
    c->zf = true, c->cf = false;
  }
}

/* $9B81: IX object record, A action -> IX entry, A key; C if found. */
static void a_object_action_entry(Cpu *c) {
  uint16_t table = (uint16_t)(c->ix + (uint8_t)(mem[c->ix] + 0x10));
  uint16_t e = object_action_entry(object_at(c->ix), c->a);
  count_r(c, table, e);
  c->ix = e, c->a = entry_key(e);
  cp_ff(c, c->a);
}

/* $9A85: A character -> IY and HL its record, A its first byte, Z. */
static void a_find_character(Cpu *c) {
  uint16_t r = find_character(c->a);
  c->iy = r, c->h = r >> 8, c->l = (uint8_t)r;
  c->a = mem[r];
  c->zf = true, c->cf = false;
}

/* $9AA0: A character, B key. */
static void a_character_set_entry(Cpu *c) { CLEAN(c, character_set_entry(c->a, c->b)); }

/* $9F82: A object -> A its location or $FF; IX its record. */
static void a_object_first_location(Cpu *c) {
  uint8_t id = c->a;
  c->a = object_first_location(id);
  if (id == OBJECT_NONE) {
    c->zf = true;
    return;
  }
  c->ix = object_record_addr(id);
  c->zf = object_at(c->ix)->nlocations == 1;
}

/* $9D37: IX = the actor's location record. */
static void a_actor_location(Cpu *c) { c->ix = actor_location_addr(); }

/* $9C7E/$9C8A results: C if inside (A the container, Z); $FF: C, Z;
 * otherwise NC, A the outermost container, Z if that is you. */
static void inside_result(Cpu *c, bool inside, uint8_t outer) {
  c->a = outer;
  c->cf = inside;
  c->zf = inside || outer == 0;
}

/* $9C8A: A object, HL -> container. */
static void a_inside_walk(Cpu *c) {
  uint8_t outer;
  bool in = inside_walk(c->a, mem[get_hl(c)], &outer);
  inside_result(c, in, outer);
}

/* $9C7E: A object, HL -> container. */
static void a_object_inside(Cpu *c) {
  uint8_t outer;
  bool in = object_inside(c->a, mem[get_hl(c)], &outer);
  inside_result(c, in, outer);
}

/* $9C7B: A object, carried by the actor? HL left at the actor's id. */
static void a_object_carried(Cpu *c) {
  set_hl(c, V_ACTOR);
  a_object_inside(c);
}

/* $9C78: the first object. */
static void a_object1_carried(Cpu *c) {
  c->a = mem[V_OBJECT1];
  a_object_carried(c);
}

/* $9C17: A object, B location. */
static void a_set_contents_location(Cpu *c) { CLEAN(c, set_contents_location(c->a, c->b)); }

/* $9BDD: A object, B location, IY the record the look sets (see
 * object_moved). Leaves A = the move flag, HL at it. */
static void a_object_moved(Cpu *c) {
  CLEAN(c, object_moved(c->a, c->b, c->iy));
  c->a = mem[V_MOVED];
  set_hl(c, V_MOVED);
  c->zf = c->a == 0, c->cf = false;
}

/* $9C41: A location -> A room; IX: the record if A is out of range. */
static void a_location_room(Cpu *c) { c->a = location_room(c->a, c->ix); }

/* $9CE8, $9CED: A object -> A total. */
static void a_contents_size(Cpu *c) { c->a = contents_size(c->a); }
static void a_contents_weight(Cpu *c) { c->a = contents_weight(c->a); }

/* $9D00: A object, B 1 sizes / 0 weights, C total so far -> C. */
static void a_sum_contents(Cpu *c) { c->c = sum_contents(c->a, c->b != 0, c->c); }

/* $9D53: A object -> B its container. */
static void a_empty_object(Cpu *c) {
  uint8_t to;
  CLEAN(c, to = empty_object(c->a));
  c->b = to;
}

/* $9D50: the first object. */
static void a_empty_object1(Cpu *c) {
  c->a = mem[V_OBJECT1];
  a_empty_object(c);
}

/* $9D97: A object -> A count. */
static void a_object_count(Cpu *c) { c->a = object_count(c->a); }

/* $9D44: if not carrying the action out, abandon the caller as the
 * original does (POP BC; RET). */
static void a_output_check(Cpu *c) {
  uint8_t doing = mem[V_DOING];
  if (output_check()) {
    c->a = 1;
    c->zf = true, c->cf = false;
    return;
  }
  c->a = (uint8_t)(doing + 1);
  c->zf = c->a == 0, c->cf = doing == 0;
  set_bc(c, pop16(c));
  cpu_tail(c, 0x9D4F);
}

/* $9F76: A=0, Z, NC after. */
static void a_finish_action(Cpu *c) {
  CLEAN(c, finish_action());
  c->a = 0;
  c->zf = true, c->cf = false;
}

/* $9B02: A dark, IX your record. */
static void a_note_your_location(Cpu *c) {
  bool dark;
  CLEAN(c, dark = note_your_location());
  c->a = dark;
  c->ix = object_record_addr(YOU);
}

/* $9B44: Z if the actor is one of the objects. */
static void a_actor_is_object(Cpu *c) { c->zf = actor_is_object(); }

/* $9ACD: A object, HL its old location, DE the message. */
static void a_mention_object(Cpu *c) { CLEAN(c, mention_object(c->a, mem[get_hl(c)], get_de(c))); }

/* $9E7A: IX record -> A the innermost container that cannot be seen
 * into, $FF (Z) if none. */
static void a_opaque_container(Cpu *c) {
  c->a = opaque_container(object_at(c->ix));
  c->zf = c->a == 0xFF;
}

/* $9E40: IX viewer, A object, IY its record -> NZ if seen. */
static void a_can_see(Cpu *c) { c->zf = !can_see(object_at(c->ix), c->a, object_at(c->iy)); }

/* $9E25: the same with IX and IY the other way round. */
static void a_can_see_swapped(Cpu *c) { c->zf = !can_see(object_at(c->iy), c->a, object_at(c->ix)); }

/* $9E34: the actor; A object, IY its record. */
static void a_actor_can_see(Cpu *c) { c->zf = !actor_can_see(c->a, object_at(c->iy)); }

/* $9DD9: IX index entry, HL the words -> A id ($FF end), IX at it. IX is
 * always in the object index: the callers ($7A82, $7E12) start it at $C060
 * and go on from where the last call stopped; the check does not mutate it. */
static void a_match_object(Cpu *c) {
  uint16_t e = c->ix;
  uint8_t id;
  CLEAN(c, id = match_object(&e, get_hl(c)));
  c->a = id, c->ix = e;
}

/* $9E95: IX = the actor's exits. */
static void a_actor_exits(Cpu *c) { c->ix = actor_exits(); }

/* $A0AE: A location -> IX its exits, BC 3 (the step); out of range: A=0,
 * from IX as it was. */
static void a_location_exits(Cpu *c) {
  uint8_t loc = c->a;
  c->ix = location_exits(loc, c->ix);
  if (loc >= LOCATION_COUNT) c->a = 0;
  set_bc(c, 3);
}

/* $9EA0: IX exit, HL the words -> A destination ($FF end), IX at it. */
static void a_match_exit(Cpu *c) {
  uint16_t e = c->ix;
  uint8_t dest;
  CLEAN(c, dest = match_exit(&e, get_hl(c)));
  c->a = dest, c->ix = e;
}

/* Exit searches: A what was looked for, or $FF; Z; IX at the entry. */
static void exit_result(Cpu *c, bool found, uint16_t e) {
  c->a = found ? c->a : 0xFF;
  c->ix = e;
  c->zf = true;
}

static void a_find_exit_direction(Cpu *c) {
  uint16_t e;
  bool f = find_exit_direction(c->a, &e);
  exit_result(c, f, e);
}

static void a_find_exit_door(Cpu *c) {
  uint16_t e;
  bool f = find_exit_door(c->a, &e);
  exit_result(c, f, e);
}

static void a_find_exit_object1(Cpu *c) {
  c->a = mem[V_OBJECT1];
  a_find_exit_door(c);
}

static void a_find_exit_to(Cpu *c) {
  uint16_t e;
  bool f = find_exit_to(c->a, &e);
  exit_result(c, f, e);
}

/* $A124: IX exit, BC 3 -> IX the next with no door; Z (A=$FF) at the end,
 * else A=0, NZ, C. */
static void a_next_open_exit(Cpu *c) {
  c->ix = next_open_exit(c->ix);
  bool end = mem[c->ix] == 0xFF;
  c->a = end ? 0xFF : 0;
  c->zf = end, c->cf = !end;
}

/* $A0BA: A direction -> DE its word, HL after its low byte. */
static void a_direction_word(Cpu *c) {
  set_de(c, direction_word(c->a));
  set_hl(c, (uint16_t)(DIRECTION_WORDS + 2 * (c->a & 0x7F) + 1));
}

/* $A0BD: HL table, A n -> DE word n, HL after its low byte. */
static void a_table_word(Cpu *c) {
  uint16_t t = get_hl(c);
  set_de(c, table_word(t, c->a));
  set_hl(c, (uint16_t)(t + 2 * (c->a & 0x7F) + 1));
}

/* $9ED6: IY the name. $9EC7: IY the object's record. */
static void a_print_name(Cpu *c) { CLEAN(c, print_name(c->iy)); }
static void a_print_object_name(Cpu *c) { CLEAN(c, print_object_name(object_at(c->iy))); }

/* $A09D: IX the record: goes on to PrintMsg with HL the phrase (and DE
 * its offset), as the original jumps there (the message may take
 * arguments its caller pushed). */
static void a_contains_phrase(Cpu *c) {
  uint16_t m = contains_phrase(object_at(c->ix));
  set_de(c, (uint16_t)(m - CONTAINS_PHRASES));
  set_hl(c, m);
  cpu_tail(c, 0x72DD);
}

/* $9FC7: A container, B location, D indent; C counts. */
static void a_list_objects(Cpu *c) {
  uint8_t n;
  CLEAN(c, n = list_objects(c->a, c->b, c->d));
  c->c = (uint8_t)(c->c + n);
}

/* $9FAF: A container, B location. A=0, HL at "nothing", Z if none. */
static void a_list_contents(Cpu *c) {
  uint8_t n;
  CLEAN(c, n = list_contents(c->a, c->b));
  c->a = 0;
  set_hl(c, 0xB33B);
  c->zf = n == 0, c->cf = n != 0;
}

/* $9F94: HL left at "nothing". */
static void a_list_you_see(Cpu *c) {
  CLEAN(c, list_you_see());
  set_hl(c, 0xB33B);
}

/* $A050: A object -> NC if what it holds was listed. */
static void a_list_inside(Cpu *c) {
  bool listed;
  CLEAN(c, listed = list_inside(c->a));
  c->cf = !listed;
}

/* $A0C8: A location (IX if out of range). A=$FF, Z after. */
static void a_list_doors(Cpu *c) {
  CLEAN(c, list_doors(c->a, c->ix));
  c->a = 0xFF;
  c->zf = true;
}

#define F_AZC (OUT_A | OUT_ZF | OUT_CF)

const CleanRoutine objects_clean[] = {
    /* callers: CP $FF, then IY (and HL) */
    {0x9A85, "find_character", a_find_character, OUT_A | OUT_HL | OUT_IY | OUT_ZF},
    /* callers return or jump at once */
    {0x9AA0, "character_set_entry", a_character_set_entry, 0},
    {0x9ACD, "mention_object", a_mention_object, 0},
    /* the character loop and $9BDD go on from IX */
    {0x9B02, "note_your_location", a_note_your_location, OUT_A | OUT_IX},
    {0x9B44, "actor_is_object", a_actor_is_object, OUT_ZF},
    /* callers: JR C / INC A, then IX */
    {0x9B81, "object_action_entry", a_object_action_entry, F_AZC | OUT_IX},
    {0x9B93, "next_entry", a_next_entry, F_AZC | OUT_IX | OUT_IY},
    {0x9BA9, "next_entry_keep", a_next_entry_keep, OUT_ZF | OUT_CF | OUT_IX | OUT_IY},
    {0x9BB1, "location_record", a_location_record, F_AZC | OUT_IX},
    /* callers use IX; $9C8A and $8EEF go on with A */
    {0x9BCA, "object_record", a_object_record, F_AZC | OUT_IX},
    {0x9BDD, "object_moved", a_object_moved, OUT_A | OUT_HL | OUT_ZF},
    {0x9C17, "set_contents_location", a_set_contents_location, 0},
    {0x9C41, "location_room", a_location_room, OUT_A},
    /* callers: JR C / JP NZ; $974B goes on with A, the outermost */
    {0x9C78, "object1_carried", a_object1_carried, F_AZC | OUT_HL},
    {0x9C7B, "object_carried", a_object_carried, F_AZC | OUT_HL},
    {0x9C7E, "object_inside", a_object_inside, F_AZC},
    {0x9C8A, "inside_walk", a_inside_walk, F_AZC},
    {0x9CE8, "contents_size", a_contents_size, OUT_A},
    {0x9CED, "contents_weight", a_contents_weight, OUT_A},
    {0x9D00, "sum_contents", a_sum_contents, OUT_C},
    {0x9D37, "actor_location", a_actor_location, OUT_IX},
    {0x9D44, "output_check", a_output_check, F_AZC},
    {0x9D50, "empty_object1", a_empty_object1, OUT_A | OUT_B},
    {0x9D53, "empty_object", a_empty_object, OUT_B},
    {0x9D97, "object_count", a_object_count, OUT_A},
    /* callers: JR Z / CP $FF / CALL NZ, then IX */
    {0x9DBD, "find_entry", a_find_entry, F_AZC | OUT_IX},
    {0x9DD9, "match_object", a_match_object, OUT_A | OUT_IX, OUT_IX},
    {0x9E25, "can_see_swapped", a_can_see_swapped, OUT_ZF},
    {0x9E34, "actor_can_see", a_actor_can_see, OUT_ZF},
    {0x9E40, "can_see", a_can_see, OUT_ZF},
    {0x9E7A, "opaque_container", a_opaque_container, OUT_A | OUT_ZF},
    {0x9E95, "actor_exits", a_actor_exits, OUT_IX},
    {0x9EA0, "match_exit", a_match_exit, OUT_A | OUT_IX},
    {0x9EC7, "print_object_name", a_print_object_name, 0},
    {0x9ED6, "print_name", a_print_name, 0},
    {0x9F08, "find_exit_direction", a_find_exit_direction, OUT_A | OUT_IX | OUT_ZF},
    {0x9F25, "find_exit_object1", a_find_exit_object1, OUT_A | OUT_IX | OUT_ZF},
    {0x9F28, "find_exit_door", a_find_exit_door, OUT_A | OUT_IX | OUT_ZF},
    {0x9F2D, "find_exit_to", a_find_exit_to, OUT_A | OUT_IX | OUT_ZF},
    {0x9F76, "finish_action", a_finish_action, F_AZC},
    /* $9829 keeps IX as the actor's record */
    {0x9F82, "object_first_location", a_object_first_location, OUT_A | OUT_IX | OUT_ZF},
    {0x9F94, "list_you_see", a_list_you_see, OUT_HL},
    /* reached by JP from action handlers, whose flags no one reads (and
     * "nothing" leaves PrintMsg's flags) */
    {0x9FAF, "list_contents", a_list_contents, OUT_A | OUT_HL},
    {0x9FC7, "list_objects", a_list_objects, OUT_C},
    {0xA050, "list_inside", a_list_inside, OUT_CF},
    {0xA09D, "contains_phrase", a_contains_phrase, OUT_IX},
    {0xA0AE, "location_exits", a_location_exits, OUT_A | OUT_IX | OUT_BC},
    {0xA0BA, "direction_word", a_direction_word, OUT_DE | OUT_HL},
    {0xA0BD, "table_word", a_table_word, OUT_DE | OUT_HL},
    {0xA0C8, "list_doors", a_list_doors, OUT_A | OUT_ZF},
    {0xA124, "next_open_exit", a_next_open_exit, F_AZC | OUT_IX},
    {0, NULL, NULL, 0},
};

const CleanScratch objects_scratch[] = {
    /* The displacement of LD A,(IX+d) at $9F40, set by $9F28/$9F2D for
     * $9F30 alone: find_exit_by takes it as an argument. */
    {0x9F42, 0x9F42, "exit search offset (self-modified code)"},
    /* PrintMsg ($72DD) keeps A, DE and IX here while it prints, and reads
     * them back only to restore them: the clean code calls it with only
     * the registers it uses, so what it keeps differs. */
    {0x70DC, 0x70DE, "PrintMsg's saved A and DE"},
    {0x70E0, 0x70E1, "PrintMsg's saved IX"},
    {0, 0, NULL},
};

#endif /* CLEAN_ADAPTERS */
