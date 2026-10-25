/*
 * The sequencer: each track of a song owns one SPU voice (8 + track) and
 * plays its notes at their ticks (one tick a frame), keying off at their
 * ends; the song loops. All of it goes through the SPU's reverb (the "Hall"
 * preset), sound effects too, a little.
 */
#include <stdint.h>
#include <psxspu.h>
#include <hwregs_c.h>
#include "music.h"
#include "audio.h"
#include "ui.h"

extern const uint32_t music_bin[];

#define FIRST_VOICE  8
#define MAX_TRACKS   16

/* SPU RAM: sound effects from 1010h (audio.c), the instruments after them,
 * the reverb's work area at the very end. */
#define REVERB_SIZE  0xade0
#define REVERB_BASE  (0x80000 - REVERB_SIZE)
#define REV_REG(i)   _MMIO16((IOBASE | 0x1dc0) + 2 * (i))

/* psx-spx, "SPU Reverb Examples": Hall. */
static const uint16_t HALL[32] = {
	0x01a5, 0x0139, 0x6000, 0x5000, 0x4c00, 0xb800, 0xbc00, 0xc000,
	0x6000, 0x5c00, 0x15ba, 0x11bb, 0x14c2, 0x10bd, 0x11bc, 0x0dc1,
	0x11c0, 0x0dc3, 0x0dc0, 0x09c1, 0x0bc4, 0x07c1, 0x0a00, 0x06cd,
	0x09c2, 0x05c1, 0x05c0, 0x041a, 0x0274, 0x013a, 0x8000, 0x8000,
};

/* 2^(i/12), 4096 = 1. */
static const uint16_t SEMITONE[12] = { 4096, 4340, 4598, 4871, 5161, 5468, 5793, 6137, 6502, 6889, 7298, 7732 };

static uint32_t inst_addr[NUM_INSTRUMENTS];
static int song = -1, tick;
static int idx[MAX_TRACKS], off_at[MAX_TRACKS], note_vel[MAX_TRACKS];
static int level = 256 << 8, level_to = 256, level_step;   /* level: x256 */
static int next_fx_voice;

static int pitch_of(int inst, int note) {
	int d = note - instruments[inst].base_note, oct = 0;
	while (d < 0) { d += 12; oct--; }
	while (d >= 12) { d -= 12; oct++; }
	int p = (MUSIC_RATE * 4096 / 44100) * SEMITONE[d] >> 12;
	p = oct >= 0 ? p << oct : p >> -oct;
	return p > 0x3fff ? 0x3fff : p;
}

static void voice_volume(int v, int vol, int pan) {
	int l = vol * (64 - pan) / 64, r = vol * (64 + pan) / 64;
	if (l > vol) l = vol;
	if (r > vol) r = vol;
	SpuSetVoiceVolume(v, l, r);
}

static void key_on(int v, int inst, int note, int vol, int pan) {
	SpuSetKey(0, 1 << v);
	voice_volume(v, vol, pan);
	SpuSetVoicePitch(v, pitch_of(inst, note));
	SpuSetVoiceStartAddr(v, inst_addr[inst]);
	SPU_CH_ADSR1(v) = instruments[inst].adsr1;
	SPU_CH_ADSR2(v) = instruments[inst].adsr2;
	SpuSetKey(1, 1 << v);
}

void music_init(void) {
	/* Nothing, as much as the reverb's work area (in one go: many small
	 * transfers here took seconds). */
	static uint32_t zero[((REVERB_SIZE + 0xfff) & ~0xfff) / 4];
	/* The instruments, after the sound effects. */
	uint32_t base = audio_free_addr();
	SpuSetTransferStartAddr(base);
	SpuWrite(music_bin, MUSIC_BYTES);
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	for (int i = 0; i < NUM_INSTRUMENTS; i++) inst_addr[i] = base + instruments[i].offset;
	/* The reverb: its work area cleared first, else it plays back noise. */
	SPU_CTRL &= ~0x0080;
	SpuSetTransferStartAddr(0x80000 - sizeof(zero));
	SpuWrite(zero, sizeof(zero));
	SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
	for (int i = 0; i < 32; i++) REV_REG(i) = HALL[i];
	SPU_REVERB_ADDR = REVERB_BASE / 8;
	SPU_REVERB_VOL_L = 0x2800;
	SPU_REVERB_VOL_R = 0x2800;
	SPU_REVERB_ON1 = 0xffff;   /* everything into the hall */
	SPU_REVERB_ON2 = 0x00ff;
	SPU_CTRL |= 0x0080;
}

/* The volume a note plays at now: track, velocity, song level, the setting;
 * the whole mix kept under the sound effects (and clear of clipping). */
#define MUSIC_GAIN 45   /* % */
static int note_volume(const Track *t, int vel) {
	return (t->vol * vel >> 1) * (level >> 8) / 256 * settings.music / 10 * (0x3fff * MUSIC_GAIN / 100) / (127 * 127 / 2);
}

void music_play(int s, int fade) {
	for (int i = 0; i < MAX_TRACKS; i++) {
		SpuSetKey(0, 1 << (FIRST_VOICE + i));
		idx[i] = 0;
		off_at[i] = -1;
	}
	song = s;
	tick = 0;
	level = fade > 0 ? 0 : 256 << 8;
	music_level(256, fade);
}

void music_level(int to, int speed) {
	level_to = to;
	if (speed <= 0) { level = to << 8; level_step = 0; return; }
	level_step = ((to << 8) - level) / speed;
	if (!level_step) level = to << 8;
}

void music_update(void) {
	if (song < 0) return;
	const Song *sg = &songs[song];
	/* Fading. */
	if (level != level_to << 8) {
		level += level_step;
		if ((level_step > 0 && level > level_to << 8) || (level_step < 0 && level < level_to << 8)) level = level_to << 8;
	}
	for (int ti = 0; ti < sg->ntracks && ti < MAX_TRACKS; ti++) {
		const Track *t = &sg->tracks[ti];
		int v = FIRST_VOICE + ti;
		if (off_at[ti] == tick) { SpuSetKey(0, 1 << v); off_at[ti] = -1; }
		while (idx[ti] < t->nnotes && t->notes[idx[ti]].at == tick) {
			const Note *n = &t->notes[idx[ti]++];
			note_vel[ti] = n->vel;
			key_on(v, t->inst, n->note, note_volume(t, n->vel), t->pan);
			off_at[ti] = tick + n->len;
		}
		/* Keep up with fades and the volume setting. */
		if (off_at[ti] >= 0)
			voice_volume(v, note_volume(t, note_vel[ti]), t->pan);
	}
	if (++tick >= sg->length) {
		tick = 0;
		for (int ti = 0; ti < MAX_TRACKS; ti++) idx[ti] = 0;
	}
}

#define AMBIENCE_VOICE 5
void music_ambience(int on) {
	static int playing;
	if (on && !playing) key_on(AMBIENCE_VOICE, INST_WIND, 60, 0x0c00 * settings.sound / 10, 0);
	if (!on && playing) SpuSetKey(0, 1 << AMBIENCE_VOICE);   /* it dies away (its release) */
	playing = on;
}

void music_note(int slot, int inst, int note, int vol, int pan) {
	key_on(FIRST_VOICE + (slot & (MAX_TRACKS - 1)), inst, note, vol * settings.music / 10, pan);
}

void music_voice(int inst, int note, int vol, int pan) {
	/* Two of the sound-effect voices, so a voice doesn't cut a hit short. */
	int v = 6 + next_fx_voice;
	next_fx_voice ^= 1;
	key_on(v, inst, note, vol * settings.sound / 10, pan);
}
