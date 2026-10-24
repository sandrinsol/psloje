/* Drawing the trial (see draw.c). */
#pragma once
#include <stdint.h>

/* Ordering table layers: higher = drawn first (further back). */
enum { L_HUD = 0, L_EFFECTS = 2, L_ACTORS = 4, L_TELEGRAPH = 7, L_TILES = 9, L_VOID = 12, OT_LEN = 14 };

typedef struct {
	uint8_t r, g, b;
} Colour;

void draw_init(void);
/* The colour behind everything (the void), for the floor. */
void draw_void_colour(Colour *c);
/* Adds the frame to the ordering table; returns the next free prim byte. */
uint8_t *draw_frame(uint32_t *ot, uint8_t *prim, uint8_t *end, int frame);
