/*
 * The pad in slot 1, spoken to directly: see pad.h. Commands (from the
 * DualShock's protocol): 0x42 read (the motors' strengths ride along),
 * 0x43 enter or leave config, 0x44 analog mode, 0x4d map the motors.
 */
#include <stdint.h>
#include <string.h>
#include "pad.h"
#include "sio.h"
#include "ui.h"

static int small_t, big_t, big_level, configured;
static const uint8_t POLL[] = { 0x01, 0x42, 0x00, 0x00, 0x00 };

/* A pad wants a little time between commands. */
static void pause(void) {
	for (volatile int i = 0; i < 3000; i++);
}

/* One command: tx out (zeros past its end), rx back (as many bytes as the
 * pad's reply has). */
static int exchange(const uint8_t *tx, int len, uint8_t *rx) {
	sio_select();
	int got = 0, n = len;
	for (int i = 0; i < n && i < 24; i++) {
		int r = sio_xfer(i < len ? tx[i] : 0, i + 1 < n);
		if (r < 0) break;
		rx[i] = r;
		got = i + 1;
		/* The reply's length: the low nibble of its ID, in halfwords. */
		if (i == 1 && (r & 0x0f)) n = 3 + (r & 0x0f) * 2 > n ? 3 + (r & 0x0f) * 2 : n;
	}
	sio_deselect();
	return got;
}

void pad_init(void) {
	static const uint8_t ENTER[] = { 0x01, 0x43, 0x00, 0x01, 0x00 };
	static const uint8_t ANALOG[] = { 0x01, 0x44, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00 };   /* analog, the button still free */
	static const uint8_t MOTORS[] = { 0x01, 0x4d, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff };   /* byte 0: small, byte 1: big */
	static const uint8_t LEAVE[] = { 0x01, 0x43, 0x00, 0x00, 0x5a, 0x5a, 0x5a, 0x5a, 0x5a };
	uint8_t rx[24];
	configured = 0;
	if (exchange(ENTER, sizeof(ENTER), rx) < 3) return;
	/* (It answers in the mode it was in, then switches.) Not in config
	 * mode now: a digital pad, nothing to set. */
	pause();
	if (exchange(POLL, sizeof(POLL), rx) < 3 || rx[1] != 0xf3) {
		configured = 1;
		return;
	}
	pause();
	exchange(ANALOG, sizeof(ANALOG), rx);
	pause();
	exchange(MOTORS, sizeof(MOTORS), rx);
	/* Out of config mode again (a pad can take a moment to answer it). */
	for (int i = 0; i < 8; i++) {
		pause();
		exchange(LEAVE, sizeof(LEAVE), rx);
		pause();
		if (exchange(POLL, sizeof(POLL), rx) >= 3 && rx[1] != 0xf3) break;
	}
	configured = 1;
}

void pad_rumble(int small, int strength, int big) {
	if (settings.no_rumble) return;
	if (small > small_t) small_t = small;
	if (strength >= big_level || big_t <= 0) { big_level = strength; if (big > big_t) big_t = big; }
}

void pad_poll(uint8_t *out) {
	uint8_t tx[9] = { 0x01, 0x42, 0x00, small_t > 0 ? 0x01 : 0x00, big_t > 0 ? big_level : 0x00, 0, 0, 0, 0 };
	uint8_t rx[24];
	if (small_t > 0) small_t--;
	if (big_t > 0) big_t--;
	int n = exchange(tx, 5, rx);
	memset(out, 0xff, 8);
	if (n < 5 || rx[2] != 0x5a) {   /* nothing there: set it up when one comes */
		configured = 0;
		out[0] = 0xff;
		return;
	}
	if (!configured) pad_init();
	out[0] = 0;          /* read fine */
	out[1] = rx[1];      /* type (high nibble) and length */
	for (int i = 3; i < n && i < 9; i++) out[i - 1] = rx[i];   /* buttons, then the sticks */
}
