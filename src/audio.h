/* Sound effects (see audio.c). */
#pragma once

void audio_init(void);
/* vol 0..0x3fff, pitch 4096 = as recorded. */
void audio_play(int id, int vol, int pitch);
/* Plays the sounds for this frame's game events. */
void audio_events(void);
