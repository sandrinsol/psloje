/*
 * The pad in slot 1, read directly (sio.c) rather than by the BIOS, so a
 * DualShock's motors can be driven: rumble on hits, falls, the boss.
 */
#pragma once
#include <stdint.h>

/* Puts a DualShock into analog mode with its motors mapped (a no-op for a
 * digital pad); safe to call again if a pad is plugged in later. */
void pad_init(void);
/* Once a frame: fills a PADTYPE-shaped buffer (stat, type and length,
 * buttons, right stick, left stick) and sends the motors their strengths. */
void pad_poll(uint8_t *out);
/* Shake: the small motor (on/off) for small frames, the big one at strength
 * (0..255) for big frames; a weaker request doesn't cut a stronger one. */
void pad_rumble(int small, int strength, int big);
