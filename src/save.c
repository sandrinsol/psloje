/*
 * Saving to the memory card in slot 1. The card is spoken to directly, a
 * 128-byte frame at a time over the controller port (sio.c; the BIOS's file calls
 * misbehave in emulators), keeping to the card's own layout: block 0 holds
 * the directory, a save is one 8 KB block that starts with the header the
 * console's card manager shows (title in Shift-JIS, a 16-colour icon: a
 * little bell), then our data.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <psxapi.h>
#include <psxpad.h>
#include <hwregs_c.h>
#include "save.h"
#include "sio.h"
#include "ui.h"

#define FILE_NAME  "BESLES-99999GRIDSOUL"   /* at most 20 characters */
#define BLOCK      8192
#define MAGIC      0x31565347   /* "GSV1" */


Record record;
volatile int32_t save_state;

typedef struct {
	uint32_t magic;
	Record record;
	Settings settings;
	uint32_t sum;
} SaveData;

static uint8_t block[BLOCK] __attribute__((aligned(4)));   /* the BIOS wants it word-aligned */

/* The icon, 16 x 16, a hex digit a pixel (colours below). */
static const char ICON[16][17] = {
	"0000000220000000",
	"0000002332000000",
	"0000023443200000",
	"0000234554320000",
	"0002345555432000",
	"0002345555432000",
	"0002345555432000",
	"0023455555543200",
	"0023455555543200",
	"0234555555554320",
	"0234555555554320",
	"2222222222222222",
	"0000000660000000",
	"0000006776000000",
	"0000000660000000",
	"0000000000000000",
};
static const uint16_t ICON_CLUT[16] = {
	0x0000, 0x0421, 0x0c63, 0x10a8, 0x1d2e, 0x29d4, 0x3def, 0x5ef7,
};

static uint32_t checksum(const SaveData *d) {
	const uint8_t *p = (const uint8_t *)d;
	uint32_t s = 0x1234;
	for (unsigned i = 0; i < offsetof(SaveData, sum); i++) s = s * 31 + p[i];
	return s;
}

/* ASCII to full-width Shift-JIS, for the card manager's title. */
static void sjis_title(uint8_t *out, const char *s) {
	for (; *s; s++) {
		int c = *s, code;
		if (c >= 'A' && c <= 'Z') code = 0x8260 + c - 'A';
		else if (c >= '0' && c <= '9') code = 0x824f + c - '0';
		else code = 0x8140;   /* space */
		*out++ = code >> 8;
		*out++ = code & 0xff;
	}
}

/* ---- The card, a frame at a time ---------------------------------------------------- */

static int read_frame(int sector, uint8_t *buf) {
	sio_select();
	int ok = sio_xfer(0x81, 1) >= 0 && sio_xfer('R', 1) >= 0 && sio_xfer(0, 1) == 0x5a && sio_xfer(0, 1) == 0x5d &&
	         sio_xfer(sector >> 8, 1) >= 0 && sio_xfer(sector & 0xff, 1) >= 0 &&
	         sio_xfer(0, 1) == 0x5c && sio_xfer(0, 1) == 0x5d && sio_xfer(0, 1) == (sector >> 8) && sio_xfer(0, 1) == (sector & 0xff);
	int chk = (sector >> 8) ^ (sector & 0xff);
	for (int i = 0; ok && i < 128; i++) {
		int v = sio_xfer(0, 1);
		if (v < 0) ok = 0;
		buf[i] = v;
		chk ^= v;
	}
	ok = ok && sio_xfer(0, 1) == chk && sio_xfer(0, 0) == 'G';
	sio_deselect();
	return ok;
}

static int write_frame(int sector, const uint8_t *buf) {
	sio_select();
	int ok = sio_xfer(0x81, 1) >= 0 && sio_xfer('W', 1) >= 0 && sio_xfer(0, 1) == 0x5a && sio_xfer(0, 1) == 0x5d &&
	         sio_xfer(sector >> 8, 1) >= 0 && sio_xfer(sector & 0xff, 1) >= 0;
	int chk = (sector >> 8) ^ (sector & 0xff);
	for (int i = 0; ok && i < 128; i++) {
		if (sio_xfer(buf[i], 1) < 0) ok = 0;
		chk ^= buf[i];
	}
	ok = ok && sio_xfer(chk, 1) >= 0 && sio_xfer(0, 1) == 0x5c && sio_xfer(0, 1) == 0x5d && sio_xfer(0, 0) == 'G';
	sio_deselect();
	return ok;
}

static int read_frame3(int sector, uint8_t *buf) {
	for (int i = 0; i < 3; i++) if (read_frame(sector, buf)) return 1;
	return 0;
}

static int write_frame3(int sector, const uint8_t *buf) {
	for (int i = 0; i < 3; i++) if (write_frame(sector, buf)) return 1;
	return 0;
}

/* The block our save was last found in: checked again (one frame) rather
 * than the whole directory read each time, in case the card was swapped. */
static int known_block;

/* The directory (frames 1..15, one a block): our save's block, or a free
 * one (*fresh = 1), or 0 if there's no card or no room. */
static int find_block(int *fresh) {
	uint8_t f[128];
	int free_block = 0;
	if (known_block) {
		if (read_frame3(known_block, f) && f[0] == 0x51 && !strncmp((const char *)f + 10, FILE_NAME, 20)) {
			*fresh = 0;
			return known_block;
		}
		known_block = 0;
	}
	if (!read_frame3(0, f) || f[0] != 'M' || f[1] != 'C') return 0;
	for (int b = 1; b < 16; b++) {
		if (!read_frame3(b, f)) return 0;
		uint32_t state = f[0] | f[1] << 8 | f[2] << 16 | (uint32_t)f[3] << 24;
		if (state == 0x51 && !strncmp((const char *)f + 10, FILE_NAME, 20)) { *fresh = 0; return known_block = b; }
		if ((state & 0xf0) == 0xa0 && !free_block) free_block = b;
	}
	*fresh = 1;
	return free_block;
}

int save_load(void) {
	int fresh, b;
	b = find_block(&fresh);
	int ok = b && !fresh;
	for (int i = 0; ok && i < 8; i++) ok = read_frame3(b * 64 + i, block + i * 128);   /* the header and our data */
	if (!ok) { save_state = -1; return 0; }
	SaveData d;
	memcpy(&d, block + 0x200, sizeof(d));
	if (d.magic != MAGIC || d.sum != checksum(&d)) { save_state = -1; return 0; }
	record = d.record;
	settings = d.settings;
	save_state = 1;
	return 1;
}

int save_write(void) {
	memset(block, 0, sizeof(block));
	block[0] = 'S'; block[1] = 'C';
	block[2] = 0x11;   /* one icon frame */
	block[3] = 1;      /* one block */
	sjis_title(block + 4, "GRID SOULS");
	memcpy(block + 0x60, ICON_CLUT, sizeof(ICON_CLUT));
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x += 2) {
			int a = ICON[y][x] - '0', b = ICON[y][x + 1] - '0';
			block[0x80 + y * 8 + x / 2] = a | (b << 4);
		}
	SaveData d;
	memset(&d, 0, sizeof(d));
	d.magic = MAGIC;
	d.record = record;
	d.settings = settings;
	d.sum = checksum(&d);
	memcpy(block + 0x200, &d, sizeof(d));

	int fresh, bl = find_block(&fresh), ok = bl != 0;
	if (ok && fresh) {
		/* Its directory entry: in use, one block, the last of its file. */
		uint8_t f[128];
		memset(f, 0, sizeof(f));
		f[0] = 0x51;
		f[5] = BLOCK >> 8 & 0xff; f[6] = BLOCK >> 16 & 0xff;
		f[8] = 0xff; f[9] = 0xff;
		memcpy(f + 10, FILE_NAME, strlen(FILE_NAME));
		for (int i = 0; i < 127; i++) f[127] ^= f[i];
		ok = write_frame3(bl, f);
	}
	/* Only the frames with something in them (the rest of the block is
	 * unused); the header (title, icon) once, when the save is made. */
	for (int i = fresh ? 0 : 0x200 / 128; ok && i < 8; i++) ok = write_frame3(bl * 64 + i, block + i * 128);
	if (ok && fresh) known_block = bl;
	save_state = ok ? 2 : -1;
	return ok;
}
