/* Sound effects (see audio.c). */
#pragma once
#include <stdint.h>

void audio_init(void);
/* The first free byte of SPU RAM after the sound effects. */
uint32_t audio_free_addr(void);
/* vol 0..0x3fff, pitch 4096 = as recorded. */
void audio_play(int id, int vol, int pitch);
/* Plays the sounds for this frame's game events. */
void audio_events(void);
