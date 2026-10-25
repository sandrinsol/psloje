/*
 * Music and voices (see music.c): songs from tools/makemusic.mjs played by a
 * little sequencer on SPU voices 8..23, through the SPU's reverb.
 */
#pragma once
#include "music_data.h"

/* After audio_init(): uploads the instruments, sets up the reverb. */
void music_init(void);
/* Starts a song from the top (fading in over fade frames); -1: silence. */
void music_play(int song, int fade);
/* The song's loudness, 0..256, reached over speed frames (pauses, death). */
void music_level(int level, int speed);
/* Once a frame. */
void music_update(void);
/* A one-shot voice (whisper, sigh, choir swell) on a sound-effect voice:
 * vol 0..0x3fff, pan -64..64. */
void music_voice(int inst, int note, int vol, int pan);
/* The wind and rain over the floor, under the music (on its own voice). */
void music_ambience(int on);
/* One note on the music's own voices, while no song plays (the studio's
 * chord): slot 0..15. */
void music_note(int slot, int inst, int note, int vol, int pan);
