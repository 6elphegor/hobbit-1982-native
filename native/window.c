/* The window of the native programs: see window.h. Keys as on the
 * Spectrum: letters, space, Enter; Backspace deletes; the arrow keys as
 * the first key of a line move (N S E W); Ctrl/Alt is SYMBOL SHIFT (so
 * Ctrl+M is "." and Ctrl+P is a quote); hold N while pressing a key at
 * the title for text only. Esc quits. */
#include "window.h"

#include <SDL.h>
#include <stdio.h>

/* The original's speed: a GetKey scan (with its Pause) takes about 30,000
 * T-states, other keyboard reads about 40; at 3.5MHz. */
#define SCAN_US 8600
#define READ_US 12

jmp_buf window_quit;

static Spectrum *m;
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static uint32_t px[SPEC_SCREEN_W * SPEC_SCREEN_H];
static int held[8][5];
static Uint64 freq, start, virtual_us, last_draw;
static unsigned long last_scans;
static unsigned frames;

static int map_key(SDL_Keycode k, int rows[2], int bits[2]) {
  switch (k) {
  case SDLK_RETURN: case SDLK_KP_ENTER: return spec_char_keys('\n', rows, bits);
  case SDLK_BACKSPACE: return spec_char_keys(KEY_DELETE, rows, bits);
  case SDLK_UP: return spec_char_keys('7', rows, bits);
  case SDLK_DOWN: return spec_char_keys('6', rows, bits);
  case SDLK_LEFT: return spec_char_keys('5', rows, bits);
  case SDLK_RIGHT: return spec_char_keys('8', rows, bits);
  case SDLK_LSHIFT: case SDLK_RSHIFT: rows[0] = ROW_CAPS_V, bits[0] = 0; return 1;
  case SDLK_LCTRL: case SDLK_RCTRL: case SDLK_LALT: case SDLK_RALT:
    rows[0] = ROW_SPACE_B, bits[0] = 1;
    return 1;
  case SDLK_QUOTE: return spec_char_keys('"', rows, bits);
  }
  if (k >= 0x20 && k < 0x7F) return spec_char_keys((char)k, rows, bits);
  return 0;
}

static Uint64 now_us(void) { return (SDL_GetPerformanceCounter() - start) * 1000000 / freq; }

void window_draw(void) {
  spec_render(m, px, (frames++ / 16) & 1);
  SDL_UpdateTexture(tex, NULL, px, SPEC_SCREEN_W * 4);
  SDL_RenderClear(ren);
  SDL_RenderCopy(ren, tex, NULL, NULL);
  SDL_RenderPresent(ren);
}

/* Take the events waiting: keys to the keyboard; Esc or closing quits. */
static void take_events(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) longjmp(window_quit, 1);
    if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
      if (e.key.keysym.sym == SDLK_ESCAPE) longjmp(window_quit, 1);
      int rows[2], bits[2], n = map_key(e.key.keysym.sym, rows, bits);
      for (int i = 0; i < n; i++) {
        int *h = &held[rows[i]][bits[i]];
        *h += e.type == SDL_KEYDOWN ? 1 : -1;
        if (*h < 0) *h = 0;
        spec_key(m, rows[i], bits[i], *h > 0);
      }
    }
  }
}

void window_on_read(Spectrum *s) {
  if (s->getkey_calls != last_scans) {
    virtual_us += (Uint64)(s->getkey_calls - last_scans) * SCAN_US;
    last_scans = s->getkey_calls;
  } else {
    virtual_us += READ_US;
  }
  Uint64 now = now_us();
  if (virtual_us > now + 1000) SDL_Delay((Uint32)((virtual_us - now) / 1000));
  if (now - last_draw < 20000) return;
  last_draw = now;
  take_events();
  window_draw();
}

bool window_open(Spectrum *s, int scale) {
  m = s;
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return false;
  }
  win = SDL_CreateWindow("The Hobbit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SPEC_SCREEN_W * scale,
                         SPEC_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
  ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(ren, SPEC_SCREEN_W, SPEC_SCREEN_H);
  tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SPEC_SCREEN_W, SPEC_SCREEN_H);
  freq = SDL_GetPerformanceFrequency();
  start = SDL_GetPerformanceCounter();
  return true;
}

void window_wait(void) {
  if (setjmp(window_quit)) return;
  for (;;) {
    take_events();
    window_draw();
    SDL_Delay(20);
  }
}

void window_close(void) { SDL_Quit(); }
