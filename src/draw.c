/*
 * Drawing the trial: the void, the grid, telegraphs, the glyphs, the HUD.
 *
 * Everything is 2D, in a small ordering table used as layers (higher index
 * = drawn first = further back). Glyphs come from glyphs.tim (16 x 16,
 * 4-bit, grey) and are tinted: glyph() takes the colour the white parts
 * should be.
 */
#include <stdint.h>
#include <stdio.h>
#include <psxgpu.h>
#include <psxgte.h>
#include "draw.h"
#include "game.h"
#include "glyphs.h"

extern const uint32_t glyphs_tim[];

static uint16_t glyph_tpage, glyph_clut;
static uint8_t *prim, *prim_end;
static uint32_t *ot;

/* Floor 1, Sloth: ash greys and a dull blue void. */
static const Colour VOID_COL = { 14, 16, 24 };
static const Colour TILE_COL = { 38, 42, 52 };
static const Colour CORNER_COL = { 70, 76, 92 };

void draw_init(void) {
	TIM_IMAGE tim;
	GetTimInfo(glyphs_tim, &tim);
	LoadImage(tim.prect, tim.paddr);
	LoadImage(tim.crect, tim.caddr);
	DrawSync(0);
	glyph_tpage = getTPage(0, 1, GLYPH_TPAGE_X, GLYPH_TPAGE_Y);   /* 4-bit, additive when semi-transparent */
	glyph_clut = getClut(GLYPH_CLUT_X, GLYPH_CLUT_Y);
}

void draw_void_colour(Colour *c) {
	*c = VOID_COL;
}

/* A glyph centred on (cx, cy), size px square, turned rot quarter turns
 * clockwise (0 = as drawn, pointing up). */
static void glyph(int layer, int g, int cx, int cy, int size, int rot, int r, int gr, int b, int glow) {
	if (prim + sizeof(POLY_FT4) > prim_end) return;
	POLY_FT4 *q = (POLY_FT4 *)prim;
	setPolyFT4(q);
	int h = size / 2, x0 = cx - h, y0 = cy - h, x1 = x0 + size, y1 = y0 + size;
	int px[4] = { x0, x1, x0, x1 }, py[4] = { y0, y0, y1, y1 };   /* TL TR BL BR on screen */
	static const uint8_t ROT[4][4] = { { 0, 1, 2, 3 }, { 1, 3, 0, 2 }, { 3, 2, 1, 0 }, { 2, 0, 3, 1 } };
	const uint8_t *m = ROT[rot & 3];
	setXY4(q, px[m[0]], py[m[0]], px[m[1]], py[m[1]], px[m[2]], py[m[2]], px[m[3]], py[m[3]]);
	int u = (g % 15) * 16, v = (g / 15) * 16;
	setUV4(q, u, v, u + 16, v, u, v + 16, u + 16, v + 16);
	setRGB0(q, r >> 1, gr >> 1, b >> 1);   /* colours are 0..255 = glyph white x 0..2 */
	q->tpage = glyph_tpage;
	q->clut = glyph_clut;
	if (glow) setSemiTrans(q, 1);
	addPrim(ot + layer, q);
	prim += sizeof(POLY_FT4);
}

static void box(int layer, int x, int y, int w, int h, int r, int g, int b, int trans) {
	if (prim + sizeof(TILE) + sizeof(DR_TPAGE) > prim_end || w <= 0 || h <= 0) return;
	TILE *t = (TILE *)prim;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	if (trans) setSemiTrans(t, 1);
	addPrim(ot + layer, t);
	prim += sizeof(TILE);
	if (trans) {
		/* Semi-transparency mode for plain shapes: additive (set before it). */
		DR_TPAGE *tp = (DR_TPAGE *)prim;
		setDrawTPage(tp, 0, 1, getTPage(0, 1, 0, 0));
		addPrim(ot + layer, tp);
		prim += sizeof(DR_TPAGE);
	}
}

static void text(int x, int y, const char *s) {
	prim = (uint8_t *)FntSort(ot + L_HUD, prim, x, y, s);
}

/* ---- The scene -------------------------------------------------------------------- */

/* Grid geometry on screen. */
static int tile_px, grid_x, grid_y;

static void tile_centre(int tx16, int ty16, int *x, int *y) {
	/* tx16, ty16: tile coordinates x16 (sub-tile positions while moving) */
	*x = grid_x + tx16 * tile_px / 16 + tile_px / 2;
	*y = grid_y + ty16 * tile_px / 16 + tile_px / 2;
}

static int lerp16(int from, int to, int t, int n) {
	return from * 16 + (to - from) * 16 * t / n;
}

static void draw_void(int frame) {
	/* Dust drifting up through the dark. */
	for (int i = 0; i < 28; i++) {
		int x = (i * 73 + (isin((frame * 3 + i * 400) & 4095) * 6 >> 12)) % 320;
		int y = 240 - ((frame / 3 + i * 47) % 260) + 10;
		int c = 30 + (i * 13) % 40;
		glyph(L_VOID, G_DUST, x, y, 16, 0, c, c, c + 10, 0);
	}
}

static void draw_grid(int frame) {
	int n = grid_n;
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) {
			int sx = grid_x + x * tile_px, sy = grid_y + y * tile_px;
			int tg = game_telegraph(x, y);
			if (tg >= 256) {
				int pulse = (frame & 4) ? 40 : 0;
				box(L_TELEGRAPH, sx + 1, sy + 1, tile_px - 2, tile_px - 2, 170 + pulse, 30, 24, 0);
				glyph(L_TELEGRAPH - 1, G_DANGER, sx + tile_px / 2, sy + tile_px / 2, 16, 0, 255, 230, 200, 0);
			} else if (tg > 0) {
				int a = tg * (90 + (isin((frame * 96) & 4095) * 20 >> 12)) >> 8;
				box(L_TELEGRAPH, sx + 1, sy + 1, tile_px - 2, tile_px - 2, a, a / 5, a / 6, 0);
				glyph(L_TELEGRAPH - 1, G_WARN, sx + tile_px / 2, sy + tile_px / 2, 16, 0, 200, 90, 80, 0);
			}
			box(L_TILES, sx + 1, sy + 1, tile_px - 2, tile_px - 2, TILE_COL.r, TILE_COL.g, TILE_COL.b, 0);
			glyph(L_TILES - 1, G_CORNERS, sx + tile_px / 2, sy + tile_px / 2, tile_px - 2, 0, CORNER_COL.r, CORNER_COL.g, CORNER_COL.b, 0);
		}
	/* A faint frame around the floor. */
	int w = n * tile_px;
	box(L_TILES + 1, grid_x - 3, grid_y - 3, w + 6, w + 6, 26, 28, 36, 0);
}

static void draw_drop(int frame) {
	if (drop_amount <= 0) return;
	int x, y;
	tile_centre(drop_x * 16, drop_y * 16, &x, &y);
	int f = (frame & 8) ? 20 : 0;
	glyph(L_ACTORS + 1, G_FLAME, x, y - ((frame >> 3) & 1), 16, 0, 230 + f / 2, 180 + f, 80, 0);
}

static void draw_enemies(int frame) {
	for (int i = 0; i < MAX_ENEMIES; i++) {
		const Enemy *e = &enemies[i];
		if (e->state == E_NONE) continue;
		int fx = e->x * 16, fy = e->y * 16;
		if (e->state == E_MOVE) {
			fx = lerp16(e->from_x, e->x, e->t, 36);
			fy = lerp16(e->from_y, e->y, e->t, 36);
		}
		int x, y;
		tile_centre(fx, fy, &x, &y);
		int r = 150, g = 150, b = 166;
		if (e->state == E_WINDUP) {
			/* Rising red while it gathers itself. */
			int k = e->t * 128 / e->windup;
			r = 150 + k * 3 / 4; g = 150 - k / 2; b = 166 - k / 2;
			if (e->t > e->windup - 15) y -= 2;
		} else if (e->state == E_RECOVER && e->t < 6) {
			y += 2;   /* the blow lands */
		} else if (e->state == E_STAGGER) {
			x += (frame & 2) ? 1 : -1;
			r = 120; g = 140; b = 220;
		}
		if (e->hit_t < 6) r = g = b = 255;
		if (e->state == E_DYING) {
			int k = 255 - e->t * 255 / 30;
			r = r * k >> 8; g = g * k >> 8; b = b * k >> 8;
			y += e->t / 4;
		}
		glyph(L_ACTORS, G_HUSK, x, y, 16, 0, r, g, b, 0);
		/* Health under it once hurt. */
		if (e->hp < 60 && e->state != E_DYING) {
			box(L_ACTORS, x - 8, y + 10, 16, 2, 40, 10, 10, 0);
			box(L_ACTORS - 1, x - 8, y + 10, e->hp * 16 / 60, 2, 200, 40, 40, 0);
		}
	}
}

static void draw_player(int frame) {
	const Player *p = &player;
	int fx = p->x * 16, fy = p->y * 16;
	if (p->state == P_DODGE) {
		fx = lerp16(p->from_x, p->x, p->t, 9);
		fy = lerp16(p->from_y, p->y, p->t, 9);
	}
	int x, y;
	tile_centre(fx, fy, &x, &y);
	static const int8_t DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };
	int dx = DX[p->facing], dy = DY[p->facing];
	int r = 225, g = 225, b = 235;
	if (p->state == P_DEAD) {
		glyph(L_ACTORS, G_SKULL, x, y, 16, 0, 160, 150, 150, 0);
		return;
	}
	if (p->hurt_t < 8) { r = 255; g = 70; b = 60; }
	if (p->state == P_DODGE && p->t >= 2 && p->t <= 7) { r = 150; g = 170; b = 230; }   /* untouchable */
	if (p->state == P_STAGGER) x += (frame & 2) ? 1 : -1;
	if (p->state == P_DRINK) { g = 255; b = 200; }
	if (p->state == P_ATTACK && p->t < 8) { x -= dx * 2; y -= dy * 2; }   /* winding up */
	if (p->state == P_ATTACK && p->t >= 8 && p->t < 12) { x += dx * 3; y += dy * 3; }
	/* Afterimage of a dodge. */
	if (p->state == P_DODGE) {
		int ax, ay;
		tile_centre(p->from_x * 16, p->from_y * 16, &ax, &ay);
		glyph(L_ACTORS + 1, G_PLAYER, ax, ay, 16, 0, 40, 46, 70, 1);
	}
	glyph(L_ACTORS, G_PLAYER, x, y, 16, 0, r, g, b, 0);
	/* Facing: a wedge at the tile's edge. */
	int edge = tile_px / 2 - 2;
	glyph(L_ACTORS, G_ARROW, x + dx * edge, y + dy * edge, 16, p->facing, 255, 210, 120, 0);
	if (p->blocking)
		glyph(L_EFFECTS, G_SHIELD, x + dx * 8, y + dy * 8, 16, p->facing, 160, 170, 190, 0);
	if (p->state == P_ATTACK && p->t >= 8 && p->t < 16)
		glyph(L_EFFECTS, G_SLASH, x + dx * tile_px, y + dy * tile_px, 22, p->facing, 255, 255, 255, 1);
	if (p->parried_t < 14)
		glyph(L_EFFECTS, G_SPARK, x + dx * 10, y + dy * 10, 24, 0, 255, 240, 160, 1);
}

static void bar(int x, int y, int w, int value, int max, int r, int g, int b) {
	box(L_HUD + 1, x, y, w, 6, 30, 30, 36, 0);
	box(L_HUD, x + 1, y + 1, (w - 2) * value / max, 4, r, g, b, 0);
}

static void draw_hud(int frame) {
	char line[48];
	const Player *p = &player;
	bar(8, 6, 104, p->hp, HP_MAX, 190, 40, 40);
	bar(8, 14, 104, p->stamina, STAMINA_MAX * 16, 120, 190, 70);
	for (int i = 0; i < p->flasks; i++)
		glyph(L_HUD, G_FLASK, 124 + i * 14, 11, 16, 0, 120, 200, 170, 0);
	glyph(L_HUD, G_FLAME, 250, 11, 16, 0, 230, 180, 80, 0);
	sprintf(line, "%d", absolution);
	text(260, 8, line);
	if (floor_state == F_FIGHT || floor_state == F_WAVE_INTRO) {
		sprintf(line, "WAVE %d/3", wave + 1);
		text(176, 8, line);
	}
	if (message && (message_t > 0 || floor_state == F_DEAD || floor_state == F_CLEARED)) {
		int len = 0;
		while (message[len]) len++;
		box(L_HUD + 1, 160 - len * 4 - 8, 104, len * 8 + 16, 18, 10, 10, 14, 0);
		text(160 - len * 4, 109, message);
	}
	if (floor_state == F_DEAD && game_frames & 32)
		text(76, 132, "PRESS X TO RISE AGAIN");
	if (floor_state == F_DEAD) {
		sprintf(line, "GUILT %d", guilt + 1);
		text(132, 148, line);
	}
}

uint8_t *draw_frame(uint32_t *ot_, uint8_t *prim_, uint8_t *end, int frame) {
	ot = ot_;
	prim = prim_;
	prim_end = end;
	tile_px = 24;   /* 9 x 9: 216 pixels */
	int size = grid_n * tile_px;
	grid_x = (320 - size) / 2;
	grid_y = 24 + (216 - size) / 2;
	if (shake_t > 0) {
		grid_x += (frame & 1) ? shake_t / 3 : -shake_t / 3;
		grid_y += (frame & 2) ? shake_t / 4 : -shake_t / 4;
	}
	draw_void(frame);
	draw_grid(frame);
	draw_drop(frame);
	draw_enemies(frame);
	draw_player(frame);
	draw_hud(frame);
	return prim;
}
