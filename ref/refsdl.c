/* hobbit-ref-sdl: play the original game in a window, at Spectrum speed.
 *
 *   hobbit-ref-sdl [--rom FILE] [--seed N] [--scale N] TAPE.tzx
 *
 * Keys: letters, digits, space, Enter. Backspace deletes (the 0 key). Shift is CAPS
 * SHIFT, Ctrl or Alt is SYMBOL SHIFT; '.' ',' '"' and '@' are mapped to
 * their symbol-shift combinations. Hold N while pressing a key at the title
 * for text only. Esc quits. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum.h"

/* Matrix keys for a host key, -1 if unmapped. Combinations (the symbol
 * shift punctuation, Backspace) press two keys. */
static int map_key(SDL_Keycode k, int rows[2], int bits[2]) {
  switch (k) {
  case SDLK_RETURN: case SDLK_KP_ENTER: return spec_char_keys('\n', rows, bits);
  case SDLK_BACKSPACE: return spec_char_keys('\b', rows, bits);
  case SDLK_LSHIFT: case SDLK_RSHIFT: rows[0] = ROW_CAPS_V, bits[0] = 0; return 1;
  case SDLK_LCTRL: case SDLK_RCTRL: case SDLK_LALT: case SDLK_RALT:
    rows[0] = ROW_SPACE_B, bits[0] = 1;
    return 1;
  case SDLK_QUOTE: return spec_char_keys('"', rows, bits);
  }
  if (k >= 0x20 && k < 0x7F) return spec_char_keys((char)k, rows, bits);
  return 0;
}

int main(int argc, char **argv) {
  const char *tape = NULL, *rom = NULL;
  int seed = -1, scale = 3;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = atoi(argv[++i]) & 0xFF;
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
    else if (argv[i][0] != '-' && !tape) tape = argv[i];
    else {
      fprintf(stderr, "usage: hobbit-ref-sdl [--rom FILE] [--seed N] [--scale N] TAPE.tzx\n");
      return 2;
    }
  }
  if (!tape) {
    fprintf(stderr, "usage: hobbit-ref-sdl [--rom FILE] [--seed N] [--scale N] TAPE.tzx\n");
    return 2;
  }

  static Spectrum s;
  char err[256];
  if (spec_init(&s, tape, rom, err, sizeof err) != 0) {
    fprintf(stderr, "hobbit-ref-sdl: %s\n", err);
    return 1;
  }
  s.seed = seed;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *win = SDL_CreateWindow("The Hobbit (reference)", SDL_WINDOWPOS_CENTERED,
                                     SDL_WINDOWPOS_CENTERED, SPEC_SCREEN_W * scale,
                                     SPEC_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(ren, SPEC_SCREEN_W, SPEC_SCREEN_H);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                       SPEC_SCREEN_W, SPEC_SCREEN_H);
  static uint32_t px[SPEC_SCREEN_W * SPEC_SCREEN_H];

  /* Count how many host keys hold each matrix key, so releasing one of two
   * keys that share a matrix key (e.g. Shift and Backspace) works. */
  static int held[8][5];
  Uint64 freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
  unsigned frame = 0;
  for (bool quit = false; !quit;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) quit = true;
      if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
        if (e.key.keysym.sym == SDLK_ESCAPE) quit = true;
        int rows[2], bits[2], n = map_key(e.key.keysym.sym, rows, bits);
        for (int i = 0; i < n; i++) {
          int *h = &held[rows[i]][bits[i]];
          *h += e.type == SDL_KEYDOWN ? 1 : -1;
          if (*h < 0) *h = 0;
          spec_key(&s, rows[i], bits[i], *h > 0);
        }
      }
    }
    spec_run(&s, SPEC_FRAME_T);
    spec_render(&s, px, (frame++ / 16) & 1);
    SDL_UpdateTexture(tex, NULL, px, SPEC_SCREEN_W * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
    /* Pace to 50 frames a second. */
    next += freq / 50;
    Uint64 now = SDL_GetPerformanceCounter();
    if (now < next) SDL_Delay((Uint32)((next - now) * 1000 / freq));
    else next = now;
  }
  SDL_Quit();
  return 0;
}
