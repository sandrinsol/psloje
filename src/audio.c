/*
 * Sound effects: the samples from tools/makesfx.mjs go into SPU RAM at
 * start-up; each game event plays one on the next of 8 voices.
 */
#include <stdint.h>
#include <psxspu.h>
#include <hwregs_c.h>
#include "audio.h"
#include "game.h"
#include "sfx.h"
#include "music.h"
#include "ui.h"

extern const uint32_t sfx_bin[];

#define SPU_BASE  0x1010   /* after the SPU's reserved first 4 KB */
#define VOICES    6        /* 6 and 7: voices (music.c), 8..23: music */

static uint32_t addr[NUM_SFX];
static int next_voice;

void audio_init(void) {
	SpuInit();
	SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
	SpuSetTransferStartAddr(SPU_BASE);
	SpuWrite(sfx_bin, SFX_BYTES);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	for (int i = 0; i < NUM_SFX; i++) addr[i] = SPU_BASE + sfx_table[i][0];
	SpuSetCommonMasterVolume(0x3fff, 0x3fff);
}

uint32_t audio_free_addr(void) {
	return (SPU_BASE + SFX_BYTES + 63) & ~63;
}

/* vol 0..0x3fff, pitch 4096 = as recorded. */
void audio_play(int id, int vol, int pitch) {
	if (id < 0 || id >= NUM_SFX) return;
	int v = next_voice;
	next_voice = (next_voice + 1) % VOICES;
	SpuSetKey(0, 1 << v);
	vol = vol * settings.sound / 10;
	SpuSetVoiceVolume(v, vol, vol);
	SpuSetVoicePitch(v, (SFX_RATE * pitch / 44100) * 4096 >> 12);
	SpuSetVoiceStartAddr(v, addr[id]);
	/* Full volume at once, held until the sample's end flag stops it. */
	SPU_CH_ADSR1(v) = 0x00ff;
	SPU_CH_ADSR2(v) = 0x0000;
	SpuSetKey(1, 1 << v);
}

void audio_events(void) {
	static const int16_t SOUND[] = {
		[EV_TURN] = SFX_TURN, [EV_STEP] = SFX_TURN, [EV_DODGE] = SFX_DODGE, [EV_SWING] = SFX_SWING, [EV_HIT] = SFX_HIT,
		[EV_HEAVY] = SFX_RIPOSTE, [EV_KILL] = SFX_KILL, [EV_JUMP] = SFX_DODGE, [EV_CHARGED] = SFX_PARRY, [EV_LAND] = SFX_TURN, [EV_RIPOSTE] = SFX_RIPOSTE,
		[EV_HURT] = SFX_HURT, [EV_DEATH] = SFX_DEATH, [EV_WINDUP] = SFX_WINDUP, [EV_SLAM] = SFX_SLAM,
		[EV_HEAL] = SFX_HEAL, [EV_RECOVER] = SFX_RECOVER, [EV_CLEAR] = SFX_RECOVER, [EV_RISE] = -1, [EV_TRIAL] = SFX_HEAL, [EV_BOSS] = -1, [EV_BELL] = -1, [EV_WAKE] = SFX_WINDUP,
		[EV_FALL] = SFX_DODGE, [EV_CRUMBLE] = -1, [EV_PERFECT] = SFX_PARRY, [EV_SPAWN] = -1, [EV_LOCK] = SFX_TURN,
	};
	for (int i = 0; i < num_events; i++) {
		int t = game_events[i].type;
		if (t < 0 || t >= (int)(sizeof(SOUND) / sizeof(SOUND[0]))) continue;
		/* A little variety in pitch so repeats don't sound identical. */
		int pitch = 4096 + ((game_frames * 37 + i * 101) % 400) - 200;
		audio_play(SOUND[t], t == EV_WINDUP ? 0x2000 : 0x3800, pitch);
		/* Voices: a choir swells on a riposte and when the floor falls
		 * silent; the soul sighs as it rises again. */
		if (t == EV_RIPOSTE || t == EV_CLEAR) music_voice(INST_SWELL, 57, 0x2c00, 0);
		if (t == EV_RISE) music_voice(INST_SIGH, 60, 0x3000, 0);
		if (t == EV_BELL) music_voice(INST_BELL, 64 + (game_frames / 20) % 2 * 3, 0x3400, (game_events[i].x - 4) * 12);   /* a toll, from its side */
		if (t == EV_WAKE) audio_play(SFX_SLAM, 0x3000, 2800);   /* a low roar */
		if (t == EV_FALL) music_voice(INST_SIGH, 55, 0x3000, 0);  /* the soul, falling away */
		if (t == EV_CRUMBLE) audio_play(SFX_SLAM, 0x3400, 2200);  /* stone giving way, deep */
		if (t == EV_SPAWN) audio_play(SFX_DODGE, 0x2400, 2600);   /* ash drawn together */
		if (t == EV_PERFECT) music_voice(INST_SWELL, 64, 0x2400, 0);
	}
}
