/* The location pictures (clean edition).
 *
 * A picture is a little program in the game's data: the picture table at
 * PICTURE_TABLE gives each location with a picture the address of its
 * data (pictures may start inside each other's data). The data is
 *   border colour, attribute for the picture area,
 *   then commands to a $00:
 *   $08 x y      move the pen to (x, y)
 *   $80-$FF n    a line from the pen (see draw_line)
 *   $40-$7F x y  flood fill from (x, y) with ink (cmd & 7)
 *   $20-$3F h l  paint paper (cmd & 7) on a path of attribute cells from
 *                the cell at address hl: bytes to $FF, each (b & 3) a
 *                direction (0 up, 1 right, 2 down, 3 left) and (b >> 2) + 1
 *                cells
 *   others       ignored.
 *
 * Pictures are drawn in picture coordinates: x 0-255 left to right, y
 * 0-127 from the bottom, on 32 x 16 attribute cells of 8 x 8 points.
 * Drawing goes through a Canvas, so a picture can be drawn on anything;
 * the Spectrum canvas draws into the top two thirds of the screen memory
 * ($4000-$4FFF, $5800-$59FF), exactly as the original. */
#ifndef HOBBIT_CLEAN_DRAWING_H
#define HOBBIT_CLEAN_DRAWING_H

#include "clean.h"

#define PICTURE_TABLE 0xCC00 /* [location][data address] triples, $FF ends */
#define V_PICTURE_LOC 0x7F77 /* location whose picture is shown, $FF none */
#define V_DRAW_INK 0x824E    /* ink for plotting: the fill's colour while filling, then 0
                                ($38 when the game starts) */

enum { PICTURE_WIDTH = 256, PICTURE_HEIGHT = 128, PICTURE_COLS = 32, PICTURE_ROWS = 16 };

typedef struct __attribute__((packed)) {
  uint8_t location;
  uint16_t data;
} PictureEntry;

/* Something to draw on, in picture coordinates. */
typedef struct Canvas Canvas;
struct Canvas {
  /* Start a picture: the border colour, and every cell cleared to the
   * attribute (paper, ink, bright and flash as on the Spectrum). */
  void (*begin)(Canvas *cv, uint8_t border, uint8_t attr);
  /* Is the point set? */
  bool (*test)(Canvas *cv, uint8_t x, uint8_t y);
  /* Set the point, and give its cell the ink: ink is the raw V_DRAW_INK
   * byte (0-7, or $38 before the first fill, which turns the cell's paper
   * white and its ink black); an ink equal to the cell's paper is
   * inverted, and BRIGHT and FLASH go. */
  void (*plot)(Canvas *cv, uint8_t x, uint8_t y, uint8_t ink);
  /* Give the cell the paper (0-7); its ink stays (inverted if equal to
   * the paper), BRIGHT and FLASH go. */
  void (*paint)(Canvas *cv, uint8_t col, uint8_t row, uint8_t paper);
};

/* The Spectrum's screen memory (and its border, OUT $FE). */
Canvas *spectrum_canvas(void);

/* Draw the picture of the location, if graphics are on and it has one;
 * V_PICTURE_LOC says which was drawn ($FF: none). */
void draw_location_picture(uint8_t location);

/* Draw the picture whose data is at the address. */
void draw_picture(Canvas *cv, uint16_t data);

/* The pen. Each move goes one point, and is false (the pen staying) at
 * the edge of the picture. */
typedef struct {
  uint8_t x, y;
} Pen;
bool pen_up(Pen *p);
bool pen_down(Pen *p);
bool pen_right(Pen *p);
bool pen_left(Pen *p);

/* A line of up to count points from the pen, stepping along the major
 * axis each time and along the other once every `every` points (dir: bit 0
 * set: y is major; bit 1: down; bit 2: left). Each point is plotted
 * before the step, and the pen ends after the last; at the edge the line
 * stops. */
void draw_line(Canvas *cv, Pen *p, uint8_t dir, uint8_t count, uint8_t every);

/* Flood fill with the ink from (x, y), up to set points, as the original
 * does it (its quirks included). V_DRAW_INK is the ink while filling, then
 * 0. */
void fill(Canvas *cv, uint8_t x, uint8_t y, uint8_t ink);

#endif
