/*
 * Everything around the fight (see ui.h).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include "ui.h"
#include "font.h"
#include "draw.h"

extern const uint32_t font_small_tim[], font_large_tim[], logo_tim[];

Settings settings;
int ui_hires;

static uint32_t *ot;
static uint8_t *prim, *prim_end;
static uint16_t font_tpage[2], font_clut, logo_tpage[2], logo_clut;

static int SW(void) { return ui_hires ? 640 : 320; }
static int SH(void) { return ui_hires ? 480 : 240; }

/* ---- Settings ------------------------------------------------------------------------ */

static const uint16_t DEFAULT_KEYS[NUM_ACTIONS] = { PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1 };

void settings_defaults(void) {
	settings.music = 7;
	settings.sound = 8;
	settings.parry_wide = 0;
	settings.slow = 0;
	memcpy(settings.keys, DEFAULT_KEYS, sizeof(DEFAULT_KEYS));
}

uint16_t settings_map(uint16_t pad) {
	uint16_t out = pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_START | PAD_SELECT);
	for (int a = 0; a < NUM_ACTIONS; a++)
		if (pad & settings.keys[a]) out |= DEFAULT_KEYS[a];
	return out;
}

/* ---- Drawing --------------------------------------------------------------------------- */

void ui_init(void) {
	load_tim(font_small_tim);
	load_tim(font_large_tim);
	load_tim(logo_tim);
	font_tpage[0] = getTPage(0, 0, FONT_TPAGE_X, FONT_TPAGE_Y);
	font_tpage[1] = getTPage(0, 0, FONT_TPAGE_X, FONT_TPAGE_Y + 256);
	font_clut = getClut(FONT_CLUT_X, FONT_CLUT_Y);
	logo_tpage[0] = getTPage(0, 0, LOGO_TPAGE_X, LOGO_TPAGE_Y);
	logo_tpage[1] = getTPage(0, 0, LOGO_TPAGE_X + 64, LOGO_TPAGE_Y);
	logo_clut = getClut(LOGO_CLUT_X, LOGO_CLUT_Y);
	settings_defaults();
}

void ui_begin(uint32_t *ot_, uint8_t *prim_, uint8_t *end) {
	ot = ot_;
	prim = prim_;
	prim_end = end;
}

uint8_t *ui_end(void) {
	return prim;
}

static int room(int bytes) {
	return prim + bytes <= prim_end;
}

int text_width(const char *s, int font) {
	int w = 0;
	for (; *s; s++) {
		int c = (uint8_t)*s - 32;
		if (c < 0 || c >= 95) c = 0;
		w += font ? font_large[c][2] : font_small[c][2];
	}
	return w;
}

void text(int x, int y, const char *s, int font, int r, int g, int b, int align, int layer) {
	if (align) x -= text_width(s, font) >> (align == 1 ? 1 : 0);
	const uint8_t (*tab)[3] = font ? font_large : font_small;
	int cw = font ? LARGE_W : SMALL_W, ch = font ? LARGE_H : SMALL_H;
	for (; *s; s++) {
		int c = (uint8_t)*s - 32;
		if (c < 0 || c >= 95) c = 0;
		if (c && room(sizeof(POLY_FT4))) {
			POLY_FT4 *q = (POLY_FT4 *)prim;
			setPolyFT4(q);
			setXY4(q, x, y, x + cw, y, x, y + ch, x + cw, y + ch);
			int u = tab[c][0], v = tab[c][1];
			setUV4(q, u, v, u + cw, v, u, v + ch, u + cw, v + ch);
			setRGB0(q, r >> 1, g >> 1, b >> 1);
			q->tpage = font_tpage[font];
			q->clut = font_clut;
			addPrim(ot + layer, q);
			prim += sizeof(POLY_FT4);
		}
		x += tab[c][2];
	}
}

static void tile(int x, int y, int w, int h, int r, int g, int b, int layer) {
	if (w <= 0 || h <= 0 || !room(sizeof(TILE))) return;
	TILE *t = (TILE *)prim;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	addPrim(ot + layer, t);
	prim += sizeof(TILE);
}

/* Semi-transparency mode for the shapes just before it in the slot (added after them). */
static void blend(int abr, int layer) {
	if (!room(sizeof(DR_TPAGE))) return;
	DR_TPAGE *tp = (DR_TPAGE *)prim;
	setDrawTPage(tp, 0, 1, getTPage(0, abr, 0, 0));
	addPrim(ot + layer, tp);
	prim += sizeof(DR_TPAGE);
}

void dark_box(int x, int y, int w, int h, int amount, int layer) {
	if (w <= 0 || h <= 0 || !room(sizeof(TILE))) return;
	TILE *t = (TILE *)prim;
	setTile(t);
	setSemiTrans(t, 1);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, amount, amount, amount);
	addPrim(ot + layer, t);
	prim += sizeof(TILE);
	blend(2, layer);   /* subtract */
}

void panel(int x, int y, int w, int h, int layer) {
	/* Added front to back: edges, then the face, then the shadow behind. */
	int e = ui_hires ? 2 : 1;
	tile(x, y, w, e, 104, 98, 92, layer);                  /* lit top */
	tile(x, y, e, h, 92, 86, 82, layer);                   /* lit left */
	tile(x, y + h - e, w, e, 12, 11, 12, layer);           /* dark bottom */
	tile(x + w - e, y, e, h, 16, 15, 16, layer);           /* dark right */
	gradient(x, y, w, h, 46, 43, 44, 26, 24, 26, layer + 1);
	dark_box(x + 3 * e, y + 3 * e, w, h, 60, layer + 2);   /* a soft shadow */
}

void gradient(int x, int y, int w, int h, int r0, int g0, int b0, int r1, int g1, int b1, int layer) {
	if (!room(sizeof(POLY_G4))) return;
	POLY_G4 *q = (POLY_G4 *)prim;
	setPolyG4(q);
	setXY4(q, x, y, x + w, y, x, y + h, x + w, y + h);
	setRGB0(q, r0, g0, b0);
	setRGB1(q, r0, g0, b0);
	setRGB2(q, r1, g1, b1);
	setRGB3(q, r1, g1, b1);
	addPrim(ot + layer, q);
	prim += sizeof(POLY_G4);
}

void bar(int x, int y, int w, int h, int value, int max, int r, int g, int b, int layer) {
	int fill = value <= 0 ? 0 : (w - 2) * value / max;
	if (fill > 0 && room(sizeof(POLY_G4))) {
		/* Brighter at the top, a dark lower edge. */
		POLY_G4 *q = (POLY_G4 *)prim;
		setPolyG4(q);
		setXY4(q, x + 1, y + 1, x + 1 + fill, y + 1, x + 1, y + h - 1, x + 1 + fill, y + h - 1);
		setRGB0(q, r, g, b);
		setRGB1(q, r, g, b);
		setRGB2(q, r / 2, g / 2, b / 2);
		setRGB3(q, r / 2, g / 2, b / 2);
		addPrim(ot + layer, q);
		prim += sizeof(POLY_G4);
	}
	tile(x, y, w, h, 10, 8, 10, layer + 1);
}

void logo(int cx, int y, int bright, int layer) {
	int scale = ui_hires ? 1 : 2;   /* the logo is drawn for 640 x 480 */
	int w = LOGO_W / scale, h = LOGO_H / scale, x = cx - w / 2;
	for (int half = 0; half < 2; half++) {
		if (!room(sizeof(POLY_FT4))) return;
		POLY_FT4 *q = (POLY_FT4 *)prim;
		setPolyFT4(q);
		int x0 = x + half * w / 2;
		setXY4(q, x0, y, x0 + w / 2, y, x0, y + h, x0 + w / 2, y + h);
		setUV4(q, 0, 0, 255, 0, 0, LOGO_H, 255, LOGO_H);
		setRGB0(q, bright >> 1, bright >> 1, bright >> 1);
		q->tpage = logo_tpage[half];
		q->clut = logo_clut;
		addPrim(ot + layer, q);
		prim += sizeof(POLY_FT4);
	}
}

/* ---- Fades and bars --------------------------------------------------------------------- */

static int fade = 255, fade_target = 255, fade_speed = 8, bars;

void fade_to(int target, int speed) {
	fade_target = target;
	fade_speed = speed;
}

int fade_level(void) { return fade; }
int fade_done(void) { return fade == fade_target; }

void letterbox(int amount) {
	bars = amount;
}

void fade_draw(void) {
	if (fade < fade_target) fade = fade + fade_speed > fade_target ? fade_target : fade + fade_speed;
	if (fade > fade_target) fade = fade - fade_speed < fade_target ? fade_target : fade - fade_speed;
	/* Letterbox bars, over everything but the fade. */
	if (bars > 0) {
		int h = SH() / 8 * bars >> 8;
		tile(0, 0, SW(), h, 0, 0, 0, 0);
		tile(0, SH() - h, SW(), h, 0, 0, 0, 0);
	}
	/* The fade: added first in the top slot, so it's drawn over everything there. */
	if (fade > 0) dark_box(0, 0, SW(), SH(), fade, 0);
}

/* ---- The title screen's backdrop ---------------------------------------------------------- */

void title_draw(int frame, int show_logo) {
	int w = SW(), h = SH();
	if (show_logo) {
		int y = h / 5 + (isin((frame * 8) & 4095) >> 10);
		logo(w / 2, y, 200 + (isin((frame * 23) & 4095) >> 8), 5);
		text(w / 2, y + LOGO_H / (ui_hires ? 1 : 2) + 6, "A TRIAL OF SEVEN SINS", ui_hires ? FONT_LARGE : FONT_SMALL, 120, 112, 104, 1, 5);
	}
	/* Ash drifting down through grey light, a dark floor of cloud below. */
	for (int i = 0; i < 70; i++) {
		int x = (i * 131 + (isin((frame * 5 + i * 700) & 4095) * 12 >> 12)) % w;
		int y = ((frame * (1 + i % 3)) / 3 + i * 53) % (h + 20) - 10;
		int c = 60 + (i * 29) % 70, s = ui_hires ? 2 + (i % 3 == 0) : 1 + (i % 4 == 0);
		tile(x, y, s, s, c, c - 4, c - 6, 6);
	}
	gradient(0, 0, w, h * 2 / 3, 70, 64, 66, 18, 16, 20, 8);
	gradient(0, h * 2 / 3, w, h / 3 + 1, 18, 16, 20, 4, 4, 6, 8);
}

/* ---- Menus ---------------------------------------------------------------------------------- */

typedef struct {
	const char *label;
	int kind;    /* 0 button, 1 slider 0..10, 2 toggle, 3 key */
} Item;

static int menu = MENU_NONE, back_to = MENU_NONE, sel, waiting_key, menu_t;

static const Item TITLE_ITEMS[] = { { "NEW TRIAL", 0 }, { "OPTIONS", 0 }, { "CREDITS", 0 } };
static const Item PAUSE_ITEMS[] = { { "RESUME", 0 }, { "OPTIONS", 0 }, { "RETURN TO TITLE", 0 } };
static const Item OPTION_ITEMS[] = { { "MUSIC", 1 }, { "SOUND", 1 }, { "PARRY WINDOW", 2 }, { "GAME SPEED", 2 }, { "CONTROLS", 0 }, { "BACK", 0 } };
static const char *ACTION_NAMES[NUM_ACTIONS] = { "ATTACK", "DODGE", "BLOCK", "FLASK", "SKILL I", "SKILL II" };

static int count(void) {
	switch (menu) {
	case MENU_TITLE: return 3;
	case MENU_PAUSE: return 3;
	case MENU_OPTIONS: return 6;
	case MENU_CONTROLS: return NUM_ACTIONS + 2;   /* + DEFAULTS, BACK */
	case MENU_CREDITS: return 1;
	}
	return 0;
}

void menu_open(int m) {
	if (m == MENU_OPTIONS || m == MENU_CREDITS) back_to = menu;   /* Controls always returns to Options */
	menu = m;
	sel = 0;
	waiting_key = 0;
	menu_t = 0;
}

int menu_current(void) { return menu; }

static const char *button_name(uint16_t b) {
	switch (b) {
	case PAD_CROSS: return "CROSS";
	case PAD_CIRCLE: return "CIRCLE";
	case PAD_SQUARE: return "SQUARE";
	case PAD_TRIANGLE: return "TRIANGLE";
	case PAD_L1: return "L1";
	case PAD_R1: return "R1";
	case PAD_L2: return "L2";
	case PAD_R2: return "R2";
	case PAD_L3: return "L3";
	case PAD_R3: return "R3";
	}
	return "?";
}

int menu_update(uint16_t held, uint16_t pressed) {
	(void)held;
	menu_t++;
	if (menu == MENU_NONE) return UI_NOTHING;
	if (waiting_key) {
		/* Rebinding: the next face or shoulder button pressed. */
		static const uint16_t ALLOWED[] = { PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3 };
		for (unsigned i = 0; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); i++) {
			if (!(pressed & ALLOWED[i])) continue;
			uint16_t old = settings.keys[sel];
			for (int a = 0; a < NUM_ACTIONS; a++)
				if (settings.keys[a] == ALLOWED[i]) settings.keys[a] = old;   /* swap */
			settings.keys[sel] = ALLOWED[i];
			waiting_key = 0;
			break;
		}
		return UI_NOTHING;
	}
	int n = count();
	if (pressed & PAD_UP) sel = (sel + n - 1) % n;
	if (pressed & PAD_DOWN) sel = (sel + 1) % n;
	int lr = (pressed & PAD_RIGHT) ? 1 : (pressed & PAD_LEFT) ? -1 : 0;
	int ok = pressed & (PAD_CROSS | PAD_START);
	int back = pressed & (PAD_CIRCLE | PAD_TRIANGLE);

	switch (menu) {
	case MENU_TITLE:
		if (ok && sel == 0) { menu = MENU_NONE; return UI_NEW_GAME; }
		if (ok && sel == 1) menu_open(MENU_OPTIONS);
		if (ok && sel == 2) menu_open(MENU_CREDITS);
		break;
	case MENU_PAUSE:
		if ((ok && sel == 0) || back || (pressed & PAD_START && !ok)) { menu = MENU_NONE; return UI_RESUME; }
		if (ok && sel == 1) menu_open(MENU_OPTIONS);
		if (ok && sel == 2) { menu = MENU_NONE; return UI_QUIT_TO_TITLE; }
		break;
	case MENU_OPTIONS:
		if (sel == 0 && lr) settings.music = settings.music + lr > 10 ? 10 : settings.music + lr < 0 ? 0 : settings.music + lr;
		if (sel == 1 && lr) settings.sound = settings.sound + lr > 10 ? 10 : settings.sound + lr < 0 ? 0 : settings.sound + lr;
		if (sel == 2 && (lr || ok)) settings.parry_wide ^= 1;
		if (sel == 3 && (lr || ok)) settings.slow ^= 1;
		if (ok && sel == 4) menu_open(MENU_CONTROLS);
		if ((ok && sel == 5) || back) { menu = back_to; sel = 1; }
		break;
	case MENU_CONTROLS:
		if (ok && sel < NUM_ACTIONS) waiting_key = 1;
		if (ok && sel == NUM_ACTIONS) memcpy(settings.keys, DEFAULT_KEYS, sizeof(DEFAULT_KEYS));
		if ((ok && sel == NUM_ACTIONS + 1) || back) { menu = MENU_OPTIONS; sel = 4; }
		break;
	case MENU_CREDITS:
		if (ok || back) { menu = back_to; sel = 2; }
		break;
	}
	return UI_NOTHING;
}

static void slider(int x, int y, int value) {
	int seg = ui_hires ? 14 : 7, h = ui_hires ? 10 : 5;
	for (int i = 0; i < 10; i++) {
		int on = i < value;
		tile(x + i * (seg + 2), y, seg, h, on ? 200 : 40, on ? 170 : 36, on ? 110 : 38, 2);
	}
}

void menu_draw(int frame) {
	if (menu == MENU_NONE) return;
	int hi = ui_hires, F = hi ? FONT_LARGE : FONT_SMALL, lh = hi ? 40 : 18;
	int cx = SW() / 2;
	char line[48];

	if (menu == MENU_CREDITS) {
		static const char *C[] = {
			"GRID SOULS", "",
			"DESIGN", "SANDRINSOL", "",
			"MADE WITH", "CLAUDE CODE",
			"PSN00BSDK", "", "LETTERING", "LIBERATION SERIF (SIL OFL)", "",
			"THANK YOU FOR PLAYING",
		};
		int n = sizeof(C) / sizeof(C[0]), y0 = SH() / 2 - n * lh / 2 + lh / 2;
		panel(cx - (hi ? 260 : 130), y0 - lh, hi ? 520 : 260, n * lh + lh / 2, 3);
		for (int i = 0; i < n; i++) {
			int head = i == 0 || (C[i][0] && i + 1 < n && C[i + 1][0] && (i == 2 || i == 5 || i == 9));
			text(cx, y0 + i * lh - lh / 2, C[i], F, head ? 220 : 160, head ? 190 : 150, head ? 130 : 140, 1, 1);
		}
		return;
	}

	const Item *items = menu == MENU_TITLE ? TITLE_ITEMS : menu == MENU_PAUSE ? PAUSE_ITEMS : OPTION_ITEMS;
	int n = count();
	int pw = hi ? (menu == MENU_TITLE ? 360 : 520) : (menu == MENU_TITLE ? 180 : 260);
	int y0 = menu == MENU_TITLE ? SH() * 3 / 5 : SH() / 2 - n * lh / 2;
	if (menu == MENU_PAUSE || menu == MENU_OPTIONS || menu == MENU_CONTROLS) {
		const char *title = menu == MENU_PAUSE ? "PAUSED" : menu == MENU_OPTIONS ? "OPTIONS" : "CONTROLS";
		text(cx, y0 - lh * 2, title, hi ? FONT_LARGE : FONT_LARGE, 210, 196, 170, 1, 1);
	}
	panel(cx - pw / 2, y0 - lh / 3, pw, n * lh + lh / 3, 3);
	for (int i = 0; i < n; i++) {
		int y = y0 + i * lh, on = i == sel;
		int pulse = on ? 30 + (isin((frame * 100) & 4095) * 30 >> 12) : 0;
		int r = on ? 225 + pulse / 3 : 130, g = on ? 190 + pulse / 3 : 124, b = on ? 120 : 118;
		const char *label;
		int kind;
		if (menu == MENU_CONTROLS) {
			label = i < NUM_ACTIONS ? ACTION_NAMES[i] : i == NUM_ACTIONS ? "DEFAULTS" : "BACK";
			kind = i < NUM_ACTIONS ? 3 : 0;
		} else {
			label = items[i].label;
			kind = items[i].kind;
		}
		int lx = kind ? cx - pw / 2 + (hi ? 30 : 14) : cx;
		text(lx, y, label, F, r, g, b, kind ? 0 : 1, 1);
		int vx = cx + (hi ? 40 : 20);
		if (kind == 1) slider(vx, y + (hi ? 9 : 4), i == 0 ? settings.music : settings.sound);
		if (kind == 2) {
			const char *v = i == 2 ? (settings.parry_wide ? "WIDE" : "NORMAL") : (settings.slow ? "SLOW" : "NORMAL");
			sprintf(line, "< %s >", v);
			text(vx, y, line, F, r, g, b, 0, 1);
		}
		if (kind == 3) {
			const char *v = on && waiting_key ? ((frame & 16) ? "PRESS A BUTTON" : "") : button_name(settings.keys[i]);
			text(vx, y, v, F, r, g, b, 0, 1);
		}
		if (on) {
			/* A small flame marks the choice. */
			int fx = (kind ? lx : cx - text_width(label, F) / 2) - (hi ? 22 : 11);
			tile(fx, y + (hi ? 10 : 5), hi ? 8 : 4, hi ? 8 : 4, 230, 160 + pulse, 60, 1);
		}
	}
	if (menu == MENU_TITLE && (frame & 32))
		text(cx, SH() - (hi ? 50 : 24), "CROSS: CHOOSE", hi ? FONT_SMALL : FONT_SMALL, 110, 104, 98, 1, 1);
	/* The pause menu darkens the fight behind it. */
	if (menu != MENU_TITLE && !hi) dark_box(0, 0, SW(), SH(), 110, 6);
}
