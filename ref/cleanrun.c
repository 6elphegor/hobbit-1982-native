/* hobbit-clean: the clean edition alone (clean/), with no Z80 and no
 * faithful port, run headless from a script of commands as hobbit-ref
 * runs the original, with the same transcript.
 *
 *   hobbit-clean [options] TAPE.tzx [SCRIPT]
 *
 * The tape gives the game's data (the memory image). The machine is the
 * test machine's (ref/spectrum.c) for its keyboard, typist, tape file and
 * screen, but nothing runs on its Z80 (see cleanhost.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../clean/data.h"
#include "cleanhost.h"
#include "script.h"
#include "spectrum.h"

static void usage(void) {
  fprintf(stderr,
          "usage: hobbit-clean [options] TAPE.tzx [SCRIPT]\n"
          "  --rom FILE        Spectrum 48K ROM (its CHARSET prints the input line)\n"
          "  --seed N          force the random seed ($B70E) to N (0-255)\n"
          "  --text-only       start with pictures off (as if N was held at the title)\n"
          "  --show-pictures   mark picture pauses in the output as [picture $NN]\n"
          "  --screen FILE     write the final screen to FILE as a PPM image\n"
          "  --dump FILE       write the final 64K memory image to FILE\n"
          "  --save-tape FILE  tape file for SAVE/LOAD (default hobbit-save.tap)\n"
          "  --trace-keys      report each key the typist presses\n"
          "  --original-bugs   keep the original's bugs (as the faithful port has them)\n"
          "  --video FILE      write the screen to FILE as raw RGB frames, 25 a second of\n"
          "                    game time (see cleanrun.c)\n"
          "  --video-start N   only from the script's Nth command on (1: the first)\n"
          "  --max-idle S      a hang is S seconds of work without reading the keyboard\n"
          "                    (default 1)\n");
  exit(2);
}

static Spectrum s;

/* ---------- video ----------
 * --video FILE: the screen as raw RGB frames (SPEC_SCREEN_W x
 * SPEC_SCREEN_H, 3 bytes a pixel) at 25 a second of the game's own time,
 * kept as the window keeps it (native/window.c): a GetKey scan (with its
 * pause) about 8.6 ms, another keyboard read 12 us; and a picture's
 * command DRAW_US, so that it is seen being drawn. After a picture, where
 * the game waits for a key, the picture is held a moment. For example:
 *   hobbit-clean --video /tmp/v.rgb TAPE script &&
 *   ffmpeg -f rawvideo -pix_fmt rgb24 -s 320x256 -r 25 -i /tmp/v.rgb out.mp4 */

#define FRAME_US 40000.0
#define SCAN_US 8600.0
#define READ_US 12.0
#define DRAW_US 9000.0
#define PICTURE_HOLD_US 900000.0

#define TITLE_HOLD_US 1200000.0

static FILE *video;
static int video_start; /* --video-start: no frames before this script command */
static double video_us, next_frame_us;
static unsigned long video_scans;
static unsigned video_frames;
static bool held;

static void video_frame(const Spectrum *m) {
  if (video_start && m->in_pos < video_start) return;
  static uint32_t px[SPEC_SCREEN_W * SPEC_SCREEN_H];
  static uint8_t rgb[SPEC_SCREEN_W * SPEC_SCREEN_H * 3];
  spec_render(m, px, (video_frames++ / 16) & 1);
  for (int i = 0; i < SPEC_SCREEN_W * SPEC_SCREEN_H; i++)
    rgb[3 * i] = (uint8_t)(px[i] >> 16), rgb[3 * i + 1] = (uint8_t)(px[i] >> 8), rgb[3 * i + 2] = (uint8_t)px[i];
  fwrite(rgb, 1, sizeof rgb, video);
}

/* Game time passes: the frames that fall in it. */
static void video_advance(const Spectrum *m, double us) {
  for (video_us += us; video_us >= next_frame_us; next_frame_us += FRAME_US) video_frame(m);
}

static void video_on_read(Spectrum *m) {
  if (m->getkey_calls != video_scans) {
    video_advance(m, (double)(m->getkey_calls - video_scans) * SCAN_US);
    video_scans = m->getkey_calls;
  } else {
    video_advance(m, READ_US);
  }
  /* Held a moment: the title screen, and each picture once drawn. */
  bool title = m->ipc == 0x6C6E; /* the "press any key" before a game */
  bool wait = m->ipc == ADDR_PICTURE_WAIT || title;
  if (wait && !held) video_advance(m, title ? TITLE_HOLD_US : PICTURE_HOLD_US);
  held = wait;
}

static void video_watch(Spectrum *m, uint16_t addr) {
  if (addr == 0x7FBC) video_advance(m, DRAW_US); /* a picture's next command */
}

/* ---------- running ---------- */

static void write_ppm(const char *path) {
  static uint32_t px[SPEC_SCREEN_W * SPEC_SCREEN_H];
  spec_render(&s, px, false);
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return;
  }
  fprintf(f, "P6\n%d %d\n255\n", SPEC_SCREEN_W, SPEC_SCREEN_H);
  for (int i = 0; i < SPEC_SCREEN_W * SPEC_SCREEN_H; i++) {
    uint8_t rgb[3] = {px[i] >> 16, px[i] >> 8, px[i]};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

int main(int argc, char **argv) {
  const char *tape = NULL, *script = NULL, *rom = NULL, *screen = NULL, *dump = NULL, *savetape = NULL;
  int seed = -1;
  bool show_pictures = false, text_only = false, trace_keys = false;
  double idle_seconds = 1.0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--rom") && i + 1 < argc) rom = argv[++i];
    else if (!strcmp(a, "--seed") && i + 1 < argc) seed = atoi(argv[++i]) & 0xFF;
    else if (!strcmp(a, "--text-only")) text_only = true;
    else if (!strcmp(a, "--show-pictures")) show_pictures = true;
    else if (!strcmp(a, "--screen") && i + 1 < argc) screen = argv[++i];
    else if (!strcmp(a, "--dump") && i + 1 < argc) dump = argv[++i];
    else if (!strcmp(a, "--save-tape") && i + 1 < argc) savetape = argv[++i];
    else if (!strcmp(a, "--trace-keys")) trace_keys = true;
    else if (!strcmp(a, "--original-bugs")) original_bugs = true;
    else if (!strcmp(a, "--video-start") && i + 1 < argc) video_start = atoi(argv[++i]);
    else if (!strcmp(a, "--video") && i + 1 < argc) {
      if (!(video = fopen(argv[++i], "wb"))) {
        perror(argv[i]);
        return 1;
      }
    }
    else if (!strcmp(a, "--max-idle") && i + 1 < argc) idle_seconds = atof(argv[++i]);
    else if (a[0] == '-' && a[1]) usage();
    else if (!tape) tape = a;
    else if (!script) script = a;
    else usage();
  }
  if (!tape) usage();

  char err[256];
  if (spec_init(&s, tape, rom, err, sizeof err) != 0) {
    fprintf(stderr, "hobbit-clean: %s\n", err);
    return 1;
  }
  s.trace_keys = trace_keys;
  if (savetape) snprintf(s.tape_path, sizeof s.tape_path, "%s", savetape);

  setvbuf(stdout, NULL, _IONBF, 0);
  FILE *in = stdin;
  if (script && strcmp(script, "-") != 0 && !(in = fopen(script, "r"))) {
    perror(script);
    return 1;
  }
  if (script_read(in, script ? script : "stdin") != 0) return 1;
  script_attach(&s, show_pictures, text_only);

  s.seed = seed;
  if (video) {
    s.on_read = video_on_read;
    clean_watch = video_watch;
  }
  uint16_t crash_addr;
  CleanEnd end = clean_run(&s, idle_seconds, &crash_addr);
  if (end == CLEAN_CRASHED) {
    fprintf(stderr, "hobbit-clean: the game jumped to $%04X\n", crash_addr);
    script_annotate(&s, "[crash: the game jumped into the ROM and the Spectrum reset]\n");
  }
  bool hung = end == CLEAN_HUNG;
  script_end();
  if (hung) {
    fprintf(stderr, "hobbit-clean: the game stopped asking for input (after script line %d of %d)\n", s.in_pos,
            script_length());
    printf("[hang: the game stopped asking for input]\n");
  }
  if (video) { /* the last screen, a while */
    video_advance(&s, 1200000.0);
    fclose(video);
  }
  if (screen) write_ppm(screen);
  if (dump) {
    FILE *f = fopen(dump, "wb");
    if (!f || fwrite(s.mem, 1, sizeof s.mem, f) != sizeof s.mem) perror(dump);
    if (f) fclose(f);
  }
  fprintf(stderr, "[clean] seed=$%02X getkey-scans=%lu\n", s.initial_seed & 0xFF, s.getkey_calls);
  return 0;
}
