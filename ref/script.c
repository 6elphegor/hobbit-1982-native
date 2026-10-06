/* Scripted play, for the test machines (hobbit-ref, hobbit-clean): a
 * script of commands typed into the game, and a transcript of what it
 * prints.
 *
 * The script has one command per line, typed followed by ENTER. Blank
 * lines and lines starting with '#' are skipped. Directives:
 *   @wait N   let N keyboard scans pass with no typing (time passes in game)
 *   @key TEXT type TEXT without ENTER (e.g. to answer a press-any-key)
 * Digits press the raw keys (0 deletes, 5-8 are the cursor keys), and
 * {DEL} {CLEAR} {UP} {DOWN} {LEFT} {RIGHT} {ENTER} name special keys.
 * Picture pauses (the game waits for a key after drawing a location) are
 * answered automatically. */
#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The transcript: text window output as printed, and each input line
 * (prompt, echo of the command as the game saw it, parser errors) on a line
 * of its own. */
static char input_line[256];
static size_t input_len;
static bool at_line_start = true;

static void emit(uint8_t ch) {
  if (ch == 0x0D || ch == '\n')
    putchar('\n');
  else if (ch >= 0x20 && ch < 0x7F)
    putchar(ch);
  else
    printf("\\x%02X", ch);
  at_line_start = ch == 0x0D || ch == '\n';
}

static void flush_input_line(void) {
  if (!input_len) return;
  if (!at_line_start) emit('\n');
  for (size_t i = 0; i < input_len; i++) emit(input_line[i]);
  emit('\n');
  input_len = 0;
}

static void put_char(void *ud, int stream, uint8_t ch) {
  static int last_stream;
  (void)ud;
  if (stream == 2) { /* annotations start on a line of their own */
    flush_input_line();
    if (!at_line_start && last_stream != 2) emit('\n');
    emit(ch);
  } else if (stream == 0) {
    flush_input_line();
    emit(ch);
  } else if (ch == 0x0D) {
    flush_input_line();
  } else if (ch == 0x08) {
    if (input_len) input_len--;
  } else if (input_len < sizeof input_line) {
    input_line[input_len++] = ch;
  }
  last_stream = stream;
}

/* ---------- input provider ---------- */

enum { ACT_COMMAND, ACT_KEY, ACT_WAIT };
typedef struct {
  int kind;
  char text[1024];
  unsigned long n;
} Action;

static Action *actions;
static int nactions;
static bool show_pictures, text_only;

static void annotate(Spectrum *s, const char *text) {
  while (*text) s->on_char(s->ud, 2, (uint8_t)*text++);
}

/* Called by the machine when the game wants a key. The script position is
 * kept in the machine (in_pos, in_wait) so check-mode replays see the
 * same input. Picture pauses are answered here. */
static void need_input(Spectrum *s) {
  if (s->ipc == ADDR_PICTURE_WAIT && s->in_pos >= 0) {
    if (show_pictures) {
      char buf[32];
      snprintf(buf, sizeof buf, "[picture $%02X]\n", s->mem[ADDR_LOCATION_PICTURE]);
      annotate(s, buf);
    }
    spec_type(s, " ");
    return;
  }
  if (s->in_pos < 0) { /* title screen: any key, N held for text only */
    s->in_pos = 0;
    spec_type(s, text_only ? "N" : " ");
    return;
  }
  while (s->in_pos < nactions) {
    const Action *a = &actions[s->in_pos];
    if (a->kind == ACT_WAIT) {
      /* Waiting is counted in GetKey scans; at a press-any-key prompt
       * there are none, so skip it. */
      if (!s->in_wait && !spec_in_getkey(s)) {
        s->in_pos++;
        continue;
      }
      if (!s->in_wait) s->in_wait = s->getkey_calls + a->n;
      if (s->getkey_calls < s->in_wait && spec_in_getkey(s)) return;
      s->in_wait = 0;
      s->in_pos++;
      continue;
    }
    s->in_pos++;
    spec_type(s, a->text);
    if (a->kind == ACT_COMMAND) spec_type(s, "\n");
    return;
  }
  spec_stop(s);
}

/* Expand key escapes: {DEL} delete, {CLEAR} clear the line, {UP} {DOWN}
 * {LEFT} {RIGHT} the cursor keys (as the first key of a line they type
 * N, S, W, E and ENTER). */
static int expand_keys(const char *in, char *out, size_t size) {
  static const struct {
    const char *name;
    char key;
  } keys[] = {{"{DEL}", KEY_DELETE}, {"{CLEAR}", KEY_CLEAR}, {"{UP}", '7'},
              {"{DOWN}", '6'},       {"{LEFT}", '5'},        {"{RIGHT}", '8'},
              {"{ENTER}", '\n'}};
  size_t n = 0;
  while (*in && n + 1 < size) {
    size_t k = 0, nk = sizeof keys / sizeof keys[0];
    while (k < nk && strncmp(in, keys[k].name, strlen(keys[k].name))) k++;
    if (k < nk) {
      out[n++] = keys[k].key;
      in += strlen(keys[k].name);
    } else if (*in == '{') {
      return -1;
    } else {
      out[n++] = *in++;
    }
  }
  out[n] = 0;
  return 0;
}

/* Read the script: one command per line, '#' comments, @wait and @key. */
int script_read(FILE *in, const char *name) {
  char line[1024];
  int lineno = 0;
  while (fgets(line, sizeof line, in)) {
    lineno++;
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] || line[0] == '#') continue;
    Action a = {ACT_COMMAND, "", 0};
    if (!strncmp(line, "@wait ", 6)) {
      a.kind = ACT_WAIT;
      a.n = strtoul(line + 6, NULL, 10);
    } else if (line[0] == '@' && strncmp(line, "@key ", 5) != 0) {
      fprintf(stderr, "%s:%d: unknown directive\n", name, lineno);
      return -1;
    } else {
      bool key = line[0] == '@';
      a.kind = key ? ACT_KEY : ACT_COMMAND;
      if (expand_keys(key ? line + 5 : line, a.text, sizeof a.text) != 0) {
        fprintf(stderr, "%s:%d: unknown {KEY}\n", name, lineno);
        return -1;
      }
    }
    if (!(nactions & 63)) actions = realloc(actions, (nactions + 64) * sizeof *actions);
    actions[nactions++] = a;
  }
  return 0;
}


void script_attach(Spectrum *s, bool pictures, bool text) {
  show_pictures = pictures;
  text_only = text;
  s->on_char = put_char;
  s->need_input = need_input;
  s->in_pos = -1;
}

void script_annotate(Spectrum *s, const char *text) { annotate(s, text); }

int script_length(void) { return nactions; }

void script_end(void) {
  flush_input_line();
  if (!at_line_start) emit('\n');
  fflush(stdout);
}
