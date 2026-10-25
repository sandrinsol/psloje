/* Drawing the trial (see draw.c). */
#pragma once
#include <stdint.h>

/* Ordering table: depth-sorted 3D, the HUD in the first slots. */
#define OT_LEN 1024

typedef struct {
	uint8_t r, g, b;
} Colour;

void draw_init(void);
/* Uploads a TIM (image and CLUT) to VRAM and waits for it. */
void load_tim(const uint32_t *data);
/* The colour behind everything (the void), for the floor. */
void draw_void_colour(Colour *c);
/* Adds the frame to the ordering table; returns the next free prim byte.
 * ticked: the game moved on this frame (not paused or held back). */
uint8_t *draw_frame(uint32_t *ot, uint8_t *prim, uint8_t *end, int frame, int ticked);
