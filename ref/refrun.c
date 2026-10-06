/* hobbit-ref: run the original game headless from a script of commands and
 * print everything it writes to the text window.
 *
 *   hobbit-ref [options] TAPE.tzx [SCRIPT]
 *
 * SCRIPT: commands to type, one per line (see script.c); tests/run.sh
 * reads options from a first line "#opts ARGS".
 * Without a SCRIPT, commands are read from stdin. */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "../port/routines.h"
#include "hybrid.h"
#include "script.h"
#include "spectrum.h"


static void usage(void) {
  fprintf(stderr,
          "usage: hobbit-ref [options] TAPE.tzx [SCRIPT]\n"
          "  --rom FILE        Spectrum 48K ROM. The game reads ROM bytes as data (its\n"
          "                    random number generator), so without it results differ\n"
          "                    from real hardware\n"
          "  --seed N          force the random seed ($B70E) to N (0-255)\n"
          "  --text-only       start with pictures off (as if N was held at the title)\n"
          "  --show-pictures   mark picture pauses in the output as [picture $NN]\n"
          "  --screen FILE     write the final screen to FILE as a PPM image\n"
          "  --dump FILE       write the final 64K memory image to FILE\n"
          "  --save-tape FILE  tape file for SAVE/LOAD (default hobbit-save.tap)\n"
          "  --trace-rom       report reads of the ROM outside the CHARSET\n"
          "  --trace-keys      report each key the typist presses\n"
          "  --hybrid          run ported routines in place of the originals\n"
          "  --rng clean       with --hybrid/--native: a proper random number generator\n"
          "                    instead of the original's (which reads all of memory)\n"
          "  --clean           run the clean edition's routines (clean/) where there are\n"
          "                    any (implies --rng clean; with --native, no Z80)\n"
          "  --clean-skip LIST with --clean: the faithful routines at these addresses\n"
          "                    (hex, comma-separated) instead of the clean ones\n"
          "  --check-clean     compare each clean routine with the faithful port's on\n"
          "                    every call (implies --rng clean)\n"
          "  --check-limit N   with --check-clean: only the first N calls of each routine\n"
          "                    (default 20; 0 for every call)\n"
          "  --mutate M        with --check-clean: run each checked call M more times with\n"
          "                    other inputs (from other calls of the routine, or random)\n"
          "  --native          run only ported routines, with no Z80: the game as a\n"
          "                    native program (reaching unported code ends the run)\n"
          "  --check           run ported routines and originals side by side and\n"
          "                    report differences (exit status 3 if any)\n"
          "  --only LIST       limit --hybrid/--check to these routines (hex addresses,\n"
          "                    comma-separated)\n"
          "  --dump-mismatch F save the state before a mismatching call to F\n"
          "  --strict-stack    in check mode, also compare the stack below the return\n"
          "                    address, and every register and flag (what routines leave\n"
          "                    there can reach memory, which the RNG reads)\n"
          "  --max-idle N      give up after N steps without the game asking for input\n"
          "                    (default 60000000 instructions, or with --native\n"
          "                    3000000 routine calls)\n");
  exit(2);
}

/* --native: there is no C version of the code at pc. The game's own
 * crashes end up here too (a return into data, the ROM), and are shown as
 * the emulated machine shows them; anything else is a gap in the port. */
static void no_native_code(Spectrum *s, uint16_t pc) {
  fprintf(stderr, "hobbit-ref: no C version of the code at $%04X\n", pc);
  script_annotate(s, "[crash: the game jumped into the ROM and the Spectrum reset]\n");
}

static unsigned long idle_limit; /* steps without the game asking for input (0: the default) */

static void write_ppm(const Spectrum *s, const char *path) {
  static uint32_t px[SPEC_SCREEN_W * SPEC_SCREEN_H];
  spec_render(s, px, false);
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

/* Runs share the machine: at most HOBBIT_SLOTS at once (default: the
 * number of cores), machine-wide, however they were started (several
 * people or agents testing at once would otherwise swamp it). A run holds
 * an flock on one of the slot files, and waits, without spinning, for one
 * if all are taken. */
static void take_slot(void) {
  char path[64];
  const char *env = getenv("HOBBIT_SLOTS");
  long slots = env ? strtol(env, NULL, 10) : sysconf(_SC_NPROCESSORS_ONLN);
  if (slots < 1) slots = 1;
  for (int pass = 0; pass < 2; pass++)
    for (long i = 0; i < slots; i++) {
      snprintf(path, sizeof path, "/tmp/hobbit-ref-slot-%ld", i);
      int fd = open(path, O_RDWR | O_CREAT, 0666);
      if (fd < 0) return;
      if (flock(fd, pass == 0 ? LOCK_EX | LOCK_NB : LOCK_EX) == 0) return; /* held until exit */
      close(fd);
      if (pass == 1) break;
    }
}

/* The watchdog: besides the steps counted (idle_limit), a run that reads
 * no key for WATCHDOG_SECONDS of processor time is hung (the faithful
 * port can loop for ever inside one routine, where no step is counted). */
#define WATCHDOG_SECONDS 1
static Spectrum s;
static sigjmp_buf hang_jmp;
static unsigned long watchdog_reads;

static void on_watchdog(int sig) {
  (void)sig;
  if (s.reads == watchdog_reads && !hybrid_abandon_trial()) siglongjmp(hang_jmp, 1);
  watchdog_reads = s.reads;
}

int main(int argc, char **argv) {
  take_slot();
  const char *tape = NULL, *script = NULL, *rom = NULL, *screen = NULL, *dump = NULL, *savetape = NULL;
  int seed = -1;
  HybridMode hmode = HYBRID_OFF;
  bool native = false, clean = false;
  long check_limit = -1; /* -1: the default, 20 */
  int mutate = 0;
  const char *only = NULL, *clean_skip = NULL;
  bool trace_rom = false, trace_keys = false, show_pictures = false, text_only = false;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--rom") && i + 1 < argc) rom = argv[++i];
    else if (!strcmp(a, "--seed") && i + 1 < argc) seed = atoi(argv[++i]) & 0xFF;
    else if (!strcmp(a, "--text-only")) text_only = true;
    else if (!strcmp(a, "--show-pictures")) show_pictures = true;
    else if (!strcmp(a, "--screen") && i + 1 < argc) screen = argv[++i];
    else if (!strcmp(a, "--dump") && i + 1 < argc) dump = argv[++i];
    else if (!strcmp(a, "--save-tape") && i + 1 < argc) savetape = argv[++i];
    else if (!strcmp(a, "--trace-rom")) trace_rom = true;
    else if (!strcmp(a, "--trace-keys")) trace_keys = true;
    else if (!strcmp(a, "--hybrid")) hmode = HYBRID_REPLACE;
    else if (!strcmp(a, "--check")) hmode = HYBRID_CHECK;
    else if (!strcmp(a, "--native")) hmode = HYBRID_REPLACE, native = true;
    else if (!strcmp(a, "--clean")) clean = true;
    else if (!strcmp(a, "--clean-skip") && i + 1 < argc) clean_skip = argv[++i];
    else if (!strcmp(a, "--check-clean")) hmode = HYBRID_CHECK_CLEAN;
    else if (!strcmp(a, "--check-limit") && i + 1 < argc) check_limit = strtol(argv[++i], NULL, 0);
    else if (!strcmp(a, "--mutate") && i + 1 < argc) mutate = atoi(argv[++i]);
    else if (!strcmp(a, "--only") && i + 1 < argc) only = argv[++i];
    else if (!strcmp(a, "--dump-mismatch") && i + 1 < argc) hybrid_dump_mismatch(argv[++i]);
    else if (!strcmp(a, "--strict-stack")) hybrid_strict_stack(true);
    else if (!strcmp(a, "--rng") && i + 1 < argc) {
      const char *v = argv[++i];
      if (!strcmp(v, "clean")) port_clean_rng = true;
      else if (strcmp(v, "original")) usage();
    }
    else if (!strcmp(a, "--max-idle") && i + 1 < argc) idle_limit = strtoul(argv[++i], NULL, 0);
    else if (a[0] == '-' && a[1]) usage();
    else if (!tape) tape = a;
    else if (!script) script = a;
    else usage();
  }
  if (!tape) usage();

  char err[256];
  if (spec_init(&s, tape, rom, err, sizeof err) != 0) {
    fprintf(stderr, "hobbit-ref: %s\n", err);
    return 1;
  }
  s.seed = seed;
  s.trace_rom = trace_rom;
  s.trace_keys = trace_keys;
  if (savetape) snprintf(s.tape_path, sizeof s.tape_path, "%s", savetape);
  if (clean || hmode == HYBRID_CHECK_CLEAN) {
    port_clean_rng = true; /* the clean edition has only the clean generator */
    if (hmode == HYBRID_OFF) hmode = HYBRID_REPLACE;
    hybrid_use_clean(clean);
  }
  if (port_clean_rng && hmode != HYBRID_REPLACE && hmode != HYBRID_CHECK_CLEAN) {
    fprintf(stderr, "hobbit-ref: --rng clean needs --hybrid or --native (the original code has only "
                    "its own generator)\n");
    return 2;
  }
  if (hybrid_attach(&s, hmode, only) != 0) return 2;
  if (clean_skip && hybrid_clean_skip(clean_skip) != 0) return 2;
  hybrid_check_sampling(check_limit < 0 ? 20 : (unsigned long)check_limit, mutate);
  if (native) {
    s.no_cpu = true;
    s.on_missing = no_native_code;
  }

  setvbuf(stdout, NULL, _IONBF, 0);
  FILE *in = stdin;
  if (script && strcmp(script, "-") != 0 && !(in = fopen(script, "r"))) {
    perror(script);
    return 1;
  }
  if (script_read(in, script ? script : "stdin") != 0) return 1;

  script_attach(&s, show_pictures, text_only);
  jmp_buf stop;
  s.stop_jmp = &stop;
  /* A step is an instruction, or with --native a whole routine, so a hang
   * shows much sooner there. */
  s.idle_limit = idle_limit ? idle_limit : native ? 3000000UL : 60000000UL;
  int status = 0;
  signal(SIGVTALRM, on_watchdog);
  struct itimerval tick = {{WATCHDOG_SECONDS, 0}, {WATCHDOG_SECONDS, 0}};
  setitimer(ITIMER_VIRTUAL, &tick, NULL);
  if (sigsetjmp(hang_jmp, 1)) {
    s.hung = true;
  } else if (_setjmp(stop) == 0) {
    for (;;) {
      spec_step(&s);
      /* The game never runs ROM code (the tape routines are trapped), so
       * being there means it has crashed, as a jump to $0000 does. */
      if (s.cpu.pc < 0x4000 && s.cpu.pc != ROM_SA_BYTES && s.cpu.pc != ROM_LD_BYTES) {
        script_annotate(&s, "[crash: the game jumped into the ROM and the Spectrum reset]\n");
        break;
      }

    }
  }
  struct itimerval off = {{0, 0}, {0, 0}};
  setitimer(ITIMER_VIRTUAL, &off, NULL);
  script_end();
  fflush(stdout);
  if (s.hung) {
    /* The original's own infinite loops are an outcome like its crashes:
     * marked in the transcript, not a failure of the run. */
    fprintf(stderr,
            "hobbit-ref: the game stopped asking for input (after script line %d of %d), "
            "now at PC=$%04X SP=$%04X\n",
            s.in_pos, script_length(), s.cpu.pc, s.cpu.sp);
    printf("[hang: the game stopped asking for input]\n");
  }

  if (screen) write_ppm(&s, screen);
  if (dump) {
    FILE *f = fopen(dump, "wb");
    if (!f || fwrite(s.mem, 1, sizeof s.mem, f) != sizeof s.mem) perror(dump);
    if (f) fclose(f);
  }
  fprintf(stderr, "[ref] seed=$%02X t-states=%lu getkey-scans=%lu\n", s.initial_seed & 0xFF,
          s.cpu.cyc, s.getkey_calls);
  if (hmode != HYBRID_OFF && hybrid_report(stderr) > 0 && status == 0) status = 3;
  return status;
}
