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

extern const uint32_t sfx_bin[];

#define SPU_BASE  0x1010   /* after the SPU's reserved first 4 KB */
#define VOICES    8

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

/* vol 0..0x3fff, pitch 4096 = as recorded. */
void audio_play(int id, int vol, int pitch) {
	if (id < 0 || id >= NUM_SFX) return;
	int v = next_voice;
	next_voice = (next_voice + 1) % VOICES;
	SpuSetKey(0, 1 << v);
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
		[EV_RIPOSTE] = SFX_RIPOSTE, [EV_KILL] = SFX_KILL, [EV_BLOCK] = SFX_BLOCK, [EV_PARRY] = SFX_PARRY,
		[EV_HURT] = SFX_HURT, [EV_DEATH] = SFX_DEATH, [EV_WINDUP] = SFX_WINDUP, [EV_SLAM] = SFX_SLAM,
		[EV_HEAL] = SFX_HEAL, [EV_RECOVER] = SFX_RECOVER, [EV_CLEAR] = SFX_RECOVER,
	};
	for (int i = 0; i < num_events; i++) {
		int t = game_events[i].type;
		if (t < 0 || t >= (int)(sizeof(SOUND) / sizeof(SOUND[0]))) continue;
		/* A little variety in pitch so repeats don't sound identical. */
		int pitch = 4096 + ((game_frames * 37 + i * 101) % 400) - 200;
		audio_play(SOUND[t], t == EV_WINDUP ? 0x2000 : 0x3800, pitch);
	}
}
