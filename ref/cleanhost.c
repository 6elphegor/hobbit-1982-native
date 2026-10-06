/* The host of the clean edition alone: see cleanhost.h. */
#include "cleanhost.h"

#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>

#include "../clean/game.h"
#include "../clean/platform.h"

static Spectrum *m;
static double idle;
static sigjmp_buf end_jmp; /* to clean_run: CLEAN_HUNG or CLEAN_CRASHED */
static jmp_buf restart_jmp;
static uint16_t crashed_at;

/* The watchdog: the game's own infinite loops are an outcome to report,
 * like its crashes. Every keyboard read restarts the timer. */
static void watchdog(void) {
  if (idle <= 0) return;
  struct itimerval t = {{0, 0}, {(time_t)idle, (suseconds_t)((idle - (time_t)idle) * 1e6)}};
  setitimer(ITIMER_VIRTUAL, &t, NULL);
}

static void watchdog_off(void) {
  struct itimerval off = {{0, 0}, {0, 0}};
  setitimer(ITIMER_VIRTUAL, &off, NULL);
}

static void on_watchdog(int sig) {
  (void)sig;
  siglongjmp(end_jmp, CLEAN_HUNG);
}

/* ---------- clean/platform.h ---------- */

uint8_t device_in(uint16_t port, uint16_t at) {
  watchdog();
  m->ipc = at;
  return spec_port_in(m, port);
}

void device_out(uint16_t port, uint8_t v) { spec_port_out(m, port, v); }

void (*clean_watch)(Spectrum *s, uint16_t addr);

void device_at(uint16_t addr, uint8_t a) {
  m->cpu.a = a;
  spec_at(m, addr);
  if (clean_watch) clean_watch(m, addr);
}

uint8_t device_random_byte(void) { return m->cpu.r; }

bool device_tape(bool save, uint16_t start, uint16_t len, bool verify) {
  return save ? spec_tape_save(m, 0xFF, start, len) : spec_tape_load(m, 0xFF, start, len, !verify);
}

_Noreturn void device_restart(void) { longjmp(restart_jmp, 1); }

_Noreturn void device_hang(void) { siglongjmp(end_jmp, CLEAN_HUNG); }

_Noreturn void device_crash(uint16_t addr) {
  crashed_at = addr;
  siglongjmp(end_jmp, CLEAN_CRASHED);
}

/* ---------- running ---------- */

CleanEnd clean_run(Spectrum *s, double idle_seconds, uint16_t *crash_addr) {
  m = s;
  idle = idle_seconds;
  mem = s->mem;
  jmp_buf stop;
  s->stop_jmp = &stop;
  if (idle > 0) signal(SIGVTALRM, on_watchdog);
  CleanEnd end = CLEAN_STOPPED;
  int how = sigsetjmp(end_jmp, 1);
  if (how) {
    end = (CleanEnd)how;
  } else if (setjmp(stop) == 0) {
    watchdog();
    if (setjmp(restart_jmp) == 0) game_start(); /* never returns */
    for (;;) { /* each restart comes back here */
      watchdog();
      if (setjmp(restart_jmp) == 0) game_restart();
    }
  }
  watchdog_off();
  if (crash_addr) *crash_addr = crashed_at;
  return end;
}
