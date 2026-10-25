/*
 * Drawing the trial in 3D: a floor of carved stone slabs floating over a
 * sea of ash, sunken statues in the haze, the soul and the husks, falling
 * ash, and the HUD on top.
 *
 * World: units, a tile is 256; x right, y down, z up the grid (row 0 is
 * furthest); the floor's top at y = 0. The camera follows the soul from
 * behind and above and swings round when it turns.
 */
#include <stdint.h>
#include <stdio.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "draw.h"
#include "audio.h"
#include "figtex.h"
#include "sfx.h"
#include "game.h"
#include "glyphs.h"
#include "models.h"
#include "anim.h"
#include "slabs.h"
#include "ui.h"

extern const uint32_t glyphs_tim[], slabs_tim[], figtex_tim[];

#define TILE_U      256
#define SLAB_GAP    10          /* each side: the void shows between slabs */
#define SLAB_DEPTH  56
#define SCREEN_H    170         /* projection distance */
#define NEAR_Z      96
#define CAM_PITCH   620         /* looking down, 4096 = full turn (~55 degrees) */
#define CAM_DIST_PER_TILE 228   /* camera distance from the floor's middle, per tile of width */
#define CAM_LOOK_Z  -90         /* looks a little this side of the middle */

/* Floor 1, Sloth: ash, grey stone, a dull light. */
static const Colour SKY_TOP = { 78, 72, 74 }, SKY_BOTTOM = { 10, 9, 13 }, FOG = { 58, 54, 57 };
#define AMBIENT     92          /* /256 */
#define SOUL_LIGHT  210
#define FOG_NEAR    3400
#define FOG_FAR     7600

static uint16_t glyph_tpage, glyph_clut, slab_tpage, slab_clut[3];
static uint16_t fig_tpage, fig_tpage_add, fig_tpage_sub, fig_clut[7];   /* figtex: normal, additive, subtractive */
static uint8_t *prim, *prim_end;
static uint32_t *ot;

/* Camera. */
static MATRIX cam;
static int32_t cam_x, cam_y, cam_z, kick_x, kick_z, zoom;
static int32_t soul_x, soul_z;   /* world position of the soul (for its light) */

/* Lights that come and go: parries, hits, the flame where you fell. */
#define MAX_LIGHTS 8
typedef struct {
	int16_t x, y, z, radius;      /* radius: 1/16 tiles */
	uint8_t r, g, b, life, max;
} DLight;
static DLight dlights[MAX_LIGHTS];
/* This frame's lights, their colour already faded by age. */
static struct { int x, y, z, r, g, b, r2; } lit[MAX_LIGHTS + 2];
static int nlit;

static int flash;             /* lightning, 0..255 */
static int soul_glow = 256;   /* the soul's light; goes out when it falls */
static int desat, dim = 256;  /* colour grading: grey, darker (death) */
static int death_t = -1;

/* PSn00bSDK's LoadImage() sends 16 or more words in whole blocks of 16,
 * rounding up: the extra words reach the GPU as commands and garble VRAM.
 * So a rectangle whose size isn't a multiple of 16 words (a CLUT of 3 or 7
 * rows) goes up a row at a time (a 16-colour row is 8 words, one block). */
static void load_rect(const RECT *r, const uint32_t *data) {
	int words = r->w * r->h / 2;
	if (words < 16 || !(words & 15)) {
		LoadImage((RECT *)r, (uint32_t *)data);
		DrawSync(0);
		return;
	}
	RECT row = *r;
	row.h = 1;
	for (int y = 0; y < r->h; y++) {
		row.y = r->y + y;
		LoadImage(&row, (uint32_t *)data + r->w * y / 2);
		DrawSync(0);
	}
}

void load_tim(const uint32_t *data) {
	TIM_IMAGE tim;
	GetTimInfo(data, &tim);
	load_rect(tim.prect, tim.paddr);
	if (tim.mode & 8) load_rect(tim.crect, tim.caddr);
}

void draw_init(void) {
	load_tim(glyphs_tim);
	load_tim(slabs_tim);
	load_tim(figtex_tim);
	glyph_tpage = getTPage(0, 1, GLYPH_TPAGE_X, GLYPH_TPAGE_Y);
	glyph_clut = getClut(GLYPH_CLUT_X, GLYPH_CLUT_Y);
	slab_tpage = getTPage(0, 0, SLAB_TPAGE_X, SLAB_TPAGE_Y);
	for (int i = 0; i < 3; i++) slab_clut[i] = getClut(SLAB_CLUT_X, SLAB_CLUT_Y + i);
	fig_tpage = getTPage(0, 0, FIG_TPAGE_X, FIG_TPAGE_Y);
	fig_tpage_add = getTPage(0, 1, FIG_TPAGE_X, FIG_TPAGE_Y);
	fig_tpage_sub = getTPage(0, 2, FIG_TPAGE_X, FIG_TPAGE_Y);
	for (int i = 0; i < 7; i++) fig_clut[i] = getClut(FIG_CLUT_X, FIG_CLUT_Y + i);
	models_init();
}

void draw_void_colour(Colour *c) {
	*c = SKY_BOTTOM;
}

/* PSn00bSDK's gte_SetRotMatrix() clobbers $t0-$t2 but only declares two of
 * them: called through a real function they're saved by the caller. */
__attribute__((noinline)) static void load_matrix(const MATRIX *m) {
	gte_SetRotMatrix(m);
	gte_SetTransMatrix(m);
}

/* The same for the lights: directions (rows) and colours (columns). */
__attribute__((noinline)) static void load_light_matrix(const MATRIX *m) {
	gte_SetLightMatrix(m);
}

__attribute__((noinline)) static void load_light_colours(const MATRIX *m, int r, int g, int b) {
	gte_SetColorMatrix(m);
	gte_SetBackColor(r, g, b);
}

static int isqrt(int v) {
	int r = 0, bit = 1 << 30;
	while (bit > v) bit >>= 2;
	while (bit) {
		if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return r;
}

/* Scales v to length 4096. */
static void normalise(int *v) {
	int x = v[0], y = v[1], z = v[2];
	while (x > 30000 || x < -30000 || y > 30000 || y < -30000 || z > 30000 || z < -30000) { x >>= 1; y >>= 1; z >>= 1; }
	int len = isqrt(x * x + y * y + z * z);
	if (!len) { v[0] = 0; v[1] = -4096; v[2] = 0; return; }
	v[0] = x * 4096 / len; v[1] = y * 4096 / len; v[2] = z * 4096 / len;
}

/* ---- Projection ------------------------------------------------------------------ */

typedef struct {
	int16_t x, y;
	int32_t z;   /* depth; < NEAR_Z: don't draw */
} Proj;

static void project(int x, int y, int z, Proj *p) {
	SVECTOR v = { x, y, z, 0 };
	DVECTOR s;
	int32_t sz;
	gte_ldv0(&v);
	gte_rtps();
	gte_stsxy(&s);
	gte_stsz(&sz);
	p->x = s.vx;
	p->y = s.vy;
	p->z = sz;
}

static inline int front(const Proj *a, const Proj *b, const Proj *c) {
	return (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x) > 0;
}

/* In front of the camera, not absurdly far out, and not all off one edge.
 * (Hot: one unsigned compare checks both ends of a range.) */
static inline int on_screen(const Proj *p, int n) {
	unsigned out_l = 1, out_r = 1, out_t = 1, out_b = 1;
	for (int i = 0; i < n; i++) {
		int x = p[i].x, y = p[i].y;
		if (p[i].z < NEAR_Z || (unsigned)(x + 600) > 1520 || (unsigned)(y + 600) > 1440) return 0;
		out_l &= x < 0; out_r &= x >= 320; out_t &= y < 0; out_b &= y >= 240;
	}
	return !(out_l | out_r | out_t | out_b);
}

static inline uint32_t *slot(int z) {
	int o = z >> 3;
	if (o < 4) o = 4;
	if (o > OT_LEN - 4) o = OT_LEN - 4;
	return ot + o;
}

static inline int fog_of(int z) {
	if (z <= FOG_NEAR) return 0;
	if (z >= FOG_FAR) return 256;
	return (z - FOG_NEAR) * 256 / (FOG_FAR - FOG_NEAR);
}

/* Colour grading: towards grey and darker as the soul dies. Applied to the
 * lights (and sky, sparks), not to every vertex: that was too slow. */
static inline void grade(int *r, int *g, int *b) {
	if (desat) {
		int l = (*r * 77 + *g * 150 + *b * 29) >> 8;
		*r += (l - *r) * desat >> 8;
		*g += (l - *g) * desat >> 8;
		*b += (l - *b) * desat >> 8;
	}
	if (dim < 256) { *r = *r * dim >> 8; *g = *g * dim >> 8; *b = *b * dim >> 8; }
}

static inline void fogged(int *r, int *g, int *b, int z) {
	int f = fog_of(z);
	*r += (FOG.r - *r) * f >> 8;
	*g += (FOG.g - *g) * f >> 8;
	*b += (FOG.b - *b) * f >> 8;
}

static inline int clamp255(int v) {
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Semi-transparency mode for what follows in this slot (set just before it). */
static void blend(uint32_t *s, int abr) {
	DR_TPAGE *tp = (DR_TPAGE *)prim;
	setDrawTPage(tp, 0, 1, getTPage(0, abr, 0, 0));
	addPrim(s, tp);
	prim += sizeof(DR_TPAGE);
}

/* ---- Camera ---------------------------------------------------------------------- */

static int ease_angle(int from, int to, int div) {
	int d = ((to - from + 2048) & 4095) - 2048;
	return (from + d / div + (d > 0) - (d < 0)) & 4095;
}

static void tile_world(int tx16, int ty16, int *x, int *z) {
	/* tx16, ty16: tile x16 (between tiles while moving) */
	int half = (grid_n - 1) * 8;
	*x = (tx16 - half) * TILE_U / 16;
	*z = (half - ty16) * TILE_U / 16;
}

/* The camera stays put: south of the floor, high, looking down at its
 * middle, far enough back to see all of it. */
static void update_camera(int frame) {
	int sp = isin(CAM_PITCH), cp = icos(CAM_PITCH);
	int dist = grid_n * CAM_DIST_PER_TILE - zoom;
	/* A floor begins: the camera drifts in from far above. */
	if (floor_state == F_WAVE_INTRO && wave == 0 && floor_t < 150) {
		int k = 150 - floor_t;
		dist += 1600 * k / 150 * k / 150;
	}
	cam_x = kick_x;
	cam_y = -(dist * sp >> 12);
	cam_z = CAM_LOOK_Z - (dist * cp >> 12) + kick_z;
	if (shake_t > 0) {
		cam_x += (frame & 1) ? shake_t * 2 : -shake_t * 2;
		cam_y += (frame & 2) ? shake_t : -shake_t;
	}
	kick_x = kick_x * 3 / 4;
	kick_z = kick_z * 3 / 4;
	zoom = zoom * 7 / 8;

	/* Rows: camera right, down and forward, in world axes (facing +z). */
	cam.m[0][0] = 4096; cam.m[0][1] = 0;  cam.m[0][2] = 0;
	cam.m[1][0] = 0;    cam.m[1][1] = cp; cam.m[1][2] = -sp;
	cam.m[2][0] = 0;    cam.m[2][1] = sp; cam.m[2][2] = cp;
	for (int r = 0; r < 3; r++)
		cam.t[r] = -((cam.m[r][0] * cam_x + cam.m[r][1] * cam_y + cam.m[r][2] * cam_z) >> 12);
	load_matrix(&cam);
}

/* ---- The void ------------------------------------------------------------------- */

/* Sprites from figtex: centre, half sizes, an effect tile (FX_*), its CLUT;
 * add: additive; flip: 0..3 mirrors the image (tumbling chunks). */
static void sprite(uint32_t *s, int cx, int cy, int hw, int hh, int fx, int clut, int r, int g, int b, int add, int flip) {
	if (prim + sizeof(POLY_FT4) > prim_end) return;
	POLY_FT4 *q = (POLY_FT4 *)prim;
	setPolyFT4(q);
	setXY4(q, cx - hw, cy - hh, cx + hw, cy - hh, cx - hw, cy + hh, cx + hw, cy + hh);
	int u0 = fx * 64, u1 = u0 + 63, v0 = 64, v1 = 127;
	if (flip & 1) { int t = u0; u0 = u1; u1 = t; }
	if (flip & 2) { int t = v0; v0 = v1; v1 = t; }
	setUV4(q, u0, v0, u1, v0, u0, v1, u1, v1);
	setRGB0(q, r, g, b);
	q->tpage = add ? fig_tpage_add : fig_tpage;
	q->clut = fig_clut[clut];
	if (add) setSemiTrans(q, 1);
	addPrim(s, q);
	prim += sizeof(POLY_FT4);
}

/* Lightning now and then: the sky flashes twice, a bolt, thunder after. */
static int prand(int n);
static int next_bolt, bolt_x, bolt_t = -1;

static void update_weather(int frame) {
	if (!next_bolt) next_bolt = frame + 200;
	if (frame >= next_bolt) {
		next_bolt = frame + 500 + prand(700);
		bolt_t = 0;
		bolt_x = prand(2) ? 20 + prand(50) : 250 + prand(50);
	}
	if (bolt_t >= 0) {
		if (bolt_t == 0) flash = 255;
		if (bolt_t == 7) flash = 190;   /* the second flicker */
		if (bolt_t == 40) audio_play(SFX_SLAM, 0x2400, 1600);   /* thunder, low */
		if (++bolt_t > 40) bolt_t = -1;
	}
	flash = flash * 13 / 16;
}

static void draw_sky(int frame) {
	int top[3] = { SKY_TOP.r + flash / 3, SKY_TOP.g + flash / 3, SKY_TOP.b + flash * 2 / 5 };
	int bot[3] = { SKY_BOTTOM.r + flash / 8, SKY_BOTTOM.g + flash / 8, SKY_BOTTOM.b + flash / 7 };
	grade(&top[0], &top[1], &top[2]);
	grade(&bot[0], &bot[1], &bot[2]);
	POLY_G4 *q = (POLY_G4 *)prim;
	setPolyG4(q);
	setXY4(q, 0, 0, 320, 0, 0, 240, 320, 240);
	setRGB0(q, clamp255(top[0]), clamp255(top[1]), clamp255(top[2]));
	setRGB1(q, clamp255(top[0]), clamp255(top[1]), clamp255(top[2]));
	setRGB2(q, bot[0], bot[1], bot[2]);
	setRGB3(q, bot[0], bot[1], bot[2]);
	addPrim(ot + OT_LEN - 1, q);
	prim += sizeof(POLY_G4);

	uint32_t *s = ot + OT_LEN - 2;
	/* High cloud drifting across the top. */
	for (int i = 0; i < 3; i++) {
		int x = ((frame / 3 + i * 173) % 520) - 100, y = 10 + i * 11;
		int c = 30 + flash / 4;
		sprite(s, x, y, 64, 18, FX_CLOUD, CLUT_CLOUD, c, c, c + 4, 1, i & 1);
	}
	/* The eye, far off above the floor. It blinks; it closes as you die. */
	int open = 4096, t = frame % 640;
	if (t < 18) open = 4096 - isin(t * 2048 / 18);
	if (death_t >= 0) open = open * (death_t < 90 ? 90 - death_t : 0) / 90;
	int hh = 15 * open >> 12;
	if (hh > 0) {
		int c = 84 + flash / 3;
		int ec[3] = { c * 5 / 4, c, c * 7 / 8 };
		grade(&ec[0], &ec[1], &ec[2]);
		sprite(s, 160, 40, 40, hh, FX_EYE, CLUT_EYE, clamp255(ec[0]), clamp255(ec[1]), clamp255(ec[2]), 0, 0);
	}
	sprite(s, 160, 40, 60, 26, FX_GLOW, CLUT_GLOW, 40 + flash / 6, 12, 8, 1, 0);
	/* The bolt: a jagged line down the side of the sky. */
	if (flash > 140) {
		int x = bolt_x, y = 0;
		for (int i = 0; i < 6 && prim + sizeof(LINE_F2) < prim_end; i++) {
			int nx = x + ((frame * 13 + i * 37) % 21) - 10, ny = y + 9 + i % 3 * 3;
			LINE_F2 *l = (LINE_F2 *)prim;
			setLineF2(l);
			setXY2(l, x, y, nx, ny);
			setRGB0(l, 230, 230, 255);
			addPrim(s, l);
			prim += sizeof(LINE_F2);
			x = nx; y = ny;
		}
	}
}

/* Banks of ash cloud drifting under the floor. */
static void draw_clouds(int frame) {
	static const int16_t C[][4] = {   /* x, y, z, size */
		{ -1800, 700, 900, 1500 }, { 1500, 900, -600, 1700 }, { 200, 1200, 2600, 2000 },
		{ -900, 1000, -2200, 1800 }, { 2400, 650, 1800, 1300 }, { -2600, 850, -400, 1500 },
	};
	for (int i = 0; i < 6; i++) {
		int drift = ((frame + i * 300) % 2400) - 1200;
		int x = C[i][0] + drift, y = C[i][1], z = C[i][2], h = C[i][3];
		Proj p;
		project(x, y, z, &p);
		if (p.z < NEAR_Z) continue;
		int hw = h * SCREEN_H / p.z;
		if (hw > 80) hw = 80;
		int hh = hw / 2;
		if (p.x + hw < 0 || p.x - hw > 320 || p.y + hh < 0 || p.y - hh > 240) continue;
		int c = 60 - fog_of(p.z) / 8 + flash / 5;
		sprite(ot + OT_LEN - 2, p.x, p.y, hw, hh, FX_CLOUD, CLUT_CLOUD, c, c, c + 6, 1, i & 3);
	}
}

/* ---- Figures -------------------------------------------------------------------- */

/* Unit vector towards the light (above, front-left), 4096 = 1. */
static const int LIGHT[3] = { -1700, -3400, 1500 };

static inline void rot_x(int a, int *y, int *z) {
	int s = isin(a), c = icos(a), ny = (*y * c - *z * s) >> 12, nz = (*y * s + *z * c) >> 12;
	*y = ny; *z = nz;
}

static inline void rot_y(int a, int *x, int *z) {
	int s = isin(a), c = icos(a), nx = (*x * c + *z * s) >> 12, nz = (-*x * s + *z * c) >> 12;
	*x = nx; *z = nz;
}

/* 3x3 rotations, 4096 = 1. */
typedef struct { int m[3][3]; } Rot;

static void rot_mul(const Rot *a, const Rot *b, Rot *o) {
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			o->m[r][c] = (a->m[r][0] * b->m[0][c] + a->m[r][1] * b->m[1][c] + a->m[r][2] * b->m[2][c]) >> 12;
}

/* Pitch (about x, positive = forward / down) then yaw (clockwise from above). */
static void rot_make(int pitch, int yaw, Rot *o) {
	int sp = isin(pitch), cp = icos(pitch), sy = isin(yaw), cy = icos(yaw);
	/* Ry * Rx */
	o->m[0][0] = cy;  o->m[0][1] = -(sy * sp >> 12); o->m[0][2] = sy * cp >> 12;
	o->m[1][0] = 0;   o->m[1][1] = cp;               o->m[1][2] = -sp;
	o->m[2][0] = -sy; o->m[2][1] = -(cy * sp >> 12); o->m[2][2] = cy * cp >> 12;
}

static inline void rot_apply(const Rot *r, int x, int y, int z, int *ox, int *oy, int *oz) {
	*ox = (r->m[0][0] * x + r->m[0][1] * y + r->m[0][2] * z) >> 12;
	*oy = (r->m[1][0] * x + r->m[1][1] * y + r->m[1][2] * z) >> 12;
	*oz = (r->m[2][0] * x + r->m[2][1] * y + r->m[2][2] * z) >> 12;
}

/* ---- Lights ----------------------------------------------------------------------- */

/* A light flares up and fades over life frames; radius in 1/16 tiles. */
static void light_add(int x, int y, int z, int r, int g, int b, int radius, int life) {
	DLight *best = &dlights[0];
	for (int i = 0; i < MAX_LIGHTS; i++) {
		DLight *l = &dlights[i];
		if (!l->life) { best = l; break; }
		if (l->life < best->life) best = l;
	}
	best->x = x; best->y = y; best->z = z; best->radius = radius;
	best->r = r; best->g = g; best->b = b; best->life = best->max = life;
}

static void light_tile(int tx, int ty, int r, int g, int b, int radius, int life) {
	int x, z;
	tile_world(tx * 16, ty * 16, &x, &z);
	light_add(x, -120, z, r, g, b, radius, life);
}

/* This frame's lights (and the flame where you fell). */
static void gather_lights(int ticked) {
	nlit = 0;
	for (int i = 0; i < MAX_LIGHTS; i++) {
		DLight *l = &dlights[i];
		if (!l->life) continue;
		int k = l->life * 256 / l->max;
		lit[nlit].x = l->x; lit[nlit].y = l->y; lit[nlit].z = l->z;
		lit[nlit].r = l->r * k >> 8; lit[nlit].g = l->g * k >> 8; lit[nlit].b = l->b * k >> 8;
		lit[nlit].r2 = l->radius * l->radius;
		nlit++;
		if (ticked) l->life--;
	}
	/* A charging blow glows on the sword, brighter as it fills. */
	if (player.state == P_CHARGE) {
		int k = player.t >= CHARGE_FULL ? 256 : player.t * 200 / CHARGE_FULL;
		if (player.t >= CHARGE_FULL) k += isin((game_frames * 400) & 4095) >> 7;
		lit[nlit].x = soul_x; lit[nlit].y = -200; lit[nlit].z = soul_z;
		lit[nlit].r = 255 * k >> 8; lit[nlit].g = 200 * k >> 8; lit[nlit].b = 110 * k >> 8;
		lit[nlit].r2 = 20 * 20;
		nlit++;
	}
	if (drop_amount > 0) {
		int x, z, f = 200 + (isin((game_frames * 300) & 4095) >> 7);
		tile_world(drop_x * 16, drop_y * 16, &x, &z);
		lit[nlit].x = x; lit[nlit].y = -70; lit[nlit].z = z;
		lit[nlit].r = f; lit[nlit].g = f * 5 / 8; lit[nlit].b = f / 4;
		lit[nlit].r2 = 24 * 24;
		nlit++;
	}
}

/* How much of light i reaches (x, z), 0..256. */
static inline int light_reach(int i, int x, int z) {
	int dx = (x - lit[i].x) >> 4, dz = (z - lit[i].z) >> 4;   /* 1/16 tile */
	return 256 * lit[i].r2 / (lit[i].r2 + dx * dx + dz * dz);
}

/* The soul's light at a point: bright around it, fading over ~2 tiles. */
static int soul_light_at(int x, int z) {
	int dx = (x - soul_x) >> 4, dz = (z - soul_z) >> 4;
	const int R2 = 40 * 40;
	return (SOUL_LIGHT * soul_glow >> 8) * R2 / (R2 + dx * dx + dz * dz);
}

/* The floor's light at a point (0..~300 a channel, 128 = plain). */
static void floor_light(int x, int z, int *r, int *g, int *b) {
	int l = AMBIENT + soul_light_at(x, z) + flash / 2;
	*r = *g = l * 128 >> 8;
	*b = *g + 6;
	for (int i = 0; i < nlit; i++) {
		int k = light_reach(i, x, z);
		*r += lit[i].r * k >> 9; *g += lit[i].g * k >> 9; *b += lit[i].b * k >> 9;
	}
}

/* ---- Figures, lit by the GTE ------------------------------------------------------- */

/* Up to three lights on a figure: the sky (from above, front-left), the
 * soul's light (or, on the soul itself, a soft fill from the camera) and the
 * brightest light nearby. Directions in world space, towards the light;
 * colours 4096 = 1. */
static int fig_dir[3][3], fig_col[3][3], fig_bk[3];
static int model_red;          /* the telegraph's red under the figure, 0..256 */
static int model_glow = 256;   /* how bright its glowing parts are */
/* Statues never move: their lit colours are worked out once and kept here
 * (a face's four corners), only brightened by lightning after that. */
static uint8_t (*face_cache)[4][3];
static int face_cache_ready, face_cache_write;

static void figure_lights(int wx, int wz, int is_soul, int light) {
	/* The sky. */
	fig_dir[0][0] = LIGHT[0]; fig_dir[0][1] = LIGHT[1]; fig_dir[0][2] = LIGHT[2];
	int sun = 2300 + flash * 10;
	fig_col[0][0] = sun; fig_col[0][1] = sun; fig_col[0][2] = sun + 200;
	/* The soul's light. */
	if (is_soul) {
		fig_dir[1][0] = 0; fig_dir[1][1] = -1800; fig_dir[1][2] = -3600;
		int f = 1400 * soul_glow >> 8;
		fig_col[1][0] = f; fig_col[1][1] = f; fig_col[1][2] = f * 9 / 8;
	} else {
		fig_dir[1][0] = soul_x - wx; fig_dir[1][1] = -160; fig_dir[1][2] = soul_z - wz;
		normalise(fig_dir[1]);
		int f = soul_light_at(wx, wz) * 20;
		fig_col[1][0] = f; fig_col[1][1] = f; fig_col[1][2] = f * 9 / 8;
	}
	/* The brightest light about. */
	int best = -1, bk = 0;
	for (int i = 0; i < nlit; i++) {
		int k = light_reach(i, wx, wz) * (lit[i].r + lit[i].g + lit[i].b) >> 8;
		if (k > bk) { bk = k; best = i; }
	}
	fig_col[2][0] = fig_col[2][1] = fig_col[2][2] = 0;
	fig_dir[2][0] = 0; fig_dir[2][1] = -4096; fig_dir[2][2] = 0;
	if (best >= 0) {
		int k = light_reach(best, wx, wz);
		fig_dir[2][0] = lit[best].x - wx; fig_dir[2][1] = lit[best].y + 40; fig_dir[2][2] = lit[best].z - wz;
		normalise(fig_dir[2]);
		fig_col[2][0] = lit[best].r * k >> 4; fig_col[2][1] = lit[best].g * k >> 4; fig_col[2][2] = lit[best].b * k >> 4;
	}
	/* Ambient (255 = 1), and the red glow of a telegraph under it. */
	int amb = 90 + flash / 4;
	fig_bk[0] = amb + model_red; fig_bk[1] = amb - model_red / 4; fig_bk[2] = amb + 6 - model_red / 4;
	for (int c = 0; c < 3; c++) grade(&fig_col[c][0], &fig_col[c][1], &fig_col[c][2]);
	grade(&fig_bk[0], &fig_bk[1], &fig_bk[2]);
	MATRIX lc;
	for (int c = 0; c < 3; c++)
		for (int ch = 0; ch < 3; ch++) {
			int v = fig_col[c][ch] * light >> 8;
			lc.m[ch][c] = v > 4095 ? 4095 : v;
		}
	for (int ch = 0; ch < 3; ch++) {
		fig_bk[ch] = fig_bk[ch] * light >> 8;
		if (fig_bk[ch] < 0) fig_bk[ch] = 0;
	}
	load_light_colours(&lc, fig_bk[0], fig_bk[1], fig_bk[2]);
}

/* The light on a normal (model space, after the part's light matrix is
 * loaded): 64 = 1. */
static inline void light_normal(int nx, int ny, int nz, CVECTOR *out) {
	SVECTOR n = { nx, ny, nz, 0 };
	static const CVECTOR grey = { 64, 64, 64, 0 };
	gte_ldv0(&n);
	gte_ldrgb(&grey);
	gte_nccs();
	gte_strgb(out);
}

/* tint: 0 normal, 1 white (hit), 2 red (hurt), 3 blue (staggered);
 * light 256 = normal; fade 256 = solid. Each part goes through the GTE with
 * its own matrix (camera x placement x pose), its lights turned into the
 * part's own space. Textured faces are Gouraud shaded, rounded ones by their
 * vertex normals. */
static void draw_model(const Model *m, const Pose *pose, int wx, int wy, int wz, int yaw, int light, int tint, int fade, int is_soul) {
	Proj pv[64];
	CVECTOR vlit[64];
	uint8_t vdone[64];
	Rot cr, obj;
	figure_lights(wx, wz, is_soul, light);
	int cfi = 0;   /* face number through the model, for the cache */
	for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) cr.m[r][c] = cam.m[r][c];
	rot_make(pose ? pose->lean : 0, yaw, &obj);
	int dy = pose ? pose->dy : 0;
	/* Each part's turn and where its origin lands, built on its parent's
	 * (parents come first): rot = parent * own, origin = parent rot *
	 * (pivot - own * pivot) + parent origin. */
	Rot prot[MAX_PARTS];
	int poff[MAX_PARTS][3];
	for (int pi = 0; pi < m->nparts; pi++) {
		const Part *pt = &m->parts[pi];
		Rot prt, full;
		rot_make(pose ? pose->pitch[pi] : 0, pose ? pose->yaw[pi] : 0, &prt);
		const Rot *up = pt->parent < 0 ? &obj : &prot[pt->parent];
		int base[3] = { wx, wy + dy, wz };
		const int *uo = pt->parent < 0 ? base : poff[pt->parent];
		rot_mul(up, &prt, &prot[pi]);           /* part -> world rotation */
		const Rot *world_r = &prot[pi];
		rot_mul(&cr, world_r, &full);           /* part -> camera */
		int qx, qy, qz, ox, oy, oz;
		rot_apply(&prt, pt->px, pt->py, pt->pz, &qx, &qy, &qz);
		rot_apply(up, pt->px - qx, pt->py - qy, pt->pz - qz, &ox, &oy, &oz);
		ox += uo[0]; oy += uo[1]; oz += uo[2];
		poff[pi][0] = ox; poff[pi][1] = oy; poff[pi][2] = oz;
		MATRIX g;
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) g.m[r][c] = full.m[r][c];
			g.t[r] = ((cam.m[r][0] * ox + cam.m[r][1] * oy + cam.m[r][2] * oz) >> 12) + cam.t[r];
		}
		/* The lights in the part's space: world^T * direction. */
		MATRIX lm;
		for (int l = 0; l < 3; l++)
			for (int c = 0; c < 3; c++)
				lm.m[l][c] = (world_r->m[0][c] * fig_dir[l][0] + world_r->m[1][c] * fig_dir[l][1] + world_r->m[2][c] * fig_dir[l][2]) >> 12;
		load_light_matrix(&lm);
		load_matrix(&g);
		int nverts = pt->nverts < 64 ? pt->nverts : 64;
		for (int i = 0; i < nverts; i++) {
			project(pt->verts[i].x, pt->verts[i].y, pt->verts[i].z, &pv[i]);
			vdone[i] = 0;
		}
		for (int fi = 0; fi < pt->nfaces; fi++) {
			const MFace *f = &pt->faces[fi];
			int tri = f->v[3] == 255, nv = tri ? 3 : 4;
			Proj p[4] = { pv[f->v[0]], pv[f->v[1]], pv[f->v[2]], tri ? pv[f->v[2]] : pv[f->v[3]] };
			uint8_t (*cached)[3] = face_cache ? face_cache[cfi++] : 0;
			if (!front(&p[0], &p[1], &p[2]) || !on_screen(p, nv)) continue;
			if (prim + sizeof(POLY_GT4) > prim_end) break;
			int col[4][3];
			if (cached && face_cache_ready && (cached[0][0] | cached[0][1] | cached[0][2])) {
				int fl = flash / 5;
				for (int k = 0; k < nv; k++) {
					col[k][0] = clamp255(cached[k][0] + fl); col[k][1] = clamp255(cached[k][1] + fl); col[k][2] = clamp255(cached[k][2] + fl);
				}
				goto emit;
			}
			/* The light at each corner. */
			CVECTOR cl[4];
			if (f->glow) {
				for (int k = 0; k < 4; k++) cl[k].r = cl[k].g = cl[k].b = model_glow >> 2;
			} else if (f->smooth) {
				for (int k = 0; k < nv; k++) {
					int v = f->v[k];
					if (!vdone[v]) {
						light_normal(pt->normals[v].x, pt->normals[v].y, pt->normals[v].z, &vlit[v]);
						vdone[v] = 1;
					}
					cl[k] = vlit[v];
				}
			} else {
				light_normal(f->nx, f->ny, f->nz, &cl[0]);
				cl[1] = cl[2] = cl[3] = cl[0];
			}
			/* Flat faces have one colour: work it out once. */
			int nc = f->smooth ? nv : 1;
			for (int k = 0; k < nc; k++) {
				int r = f->r * cl[k].r >> 6, g2 = f->g * cl[k].g >> 6, b = f->b * cl[k].b >> 6;
				if (tint == 1) { r = g2 = b = 240; }
				else if (tint == 2) { r = r / 2 + 140; g2 /= 3; b /= 3; }
				else if (tint == 3) { r = r * 3 / 4; g2 = g2 * 4 / 5; b = b + 36; }
				if (fade < 256) { r = r * fade >> 8; g2 = g2 * fade >> 8; b = b * fade >> 8; }
				if (p[k].z > FOG_NEAR) fogged(&r, &g2, &b, p[k].z);
				col[k][0] = clamp255(r); col[k][1] = clamp255(g2); col[k][2] = clamp255(b);
			}
			for (int k = nc; k < nv; k++) { col[k][0] = col[0][0]; col[k][1] = col[0][1]; col[k][2] = col[0][2]; }
			if (cached && face_cache_write)
				for (int k = 0; k < nv; k++) { cached[k][0] = col[k][0]; cached[k][1] = col[k][1]; cached[k][2] = col[k][2]; }
		emit:;
			int z = (p[0].z + p[1].z + p[2].z) / 3;
			int u = f->mat * 64;
			if (tri && f->mat >= 0) {
				POLY_GT3 *q = (POLY_GT3 *)prim;
				setPolyGT3(q);
				setXY3(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
				setUV3(q, u + 32, 0, u, 63, u + 63, 63);
				setRGB0(q, col[0][0], col[0][1], col[0][2]);
				setRGB1(q, col[1][0], col[1][1], col[1][2]);
				setRGB2(q, col[2][0], col[2][1], col[2][2]);
				q->tpage = fig_tpage;
				q->clut = fig_clut[f->mat];
				addPrim(slot(z), q);
				prim += sizeof(POLY_GT3);
			} else if (f->mat >= 0) {
				POLY_GT4 *q = (POLY_GT4 *)prim;
				setPolyGT4(q);
				setXY4(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
				setUV4(q, u, 0, u + 63, 0, u, 63, u + 63, 63);
				setRGB0(q, col[0][0], col[0][1], col[0][2]);
				setRGB1(q, col[1][0], col[1][1], col[1][2]);
				setRGB2(q, col[2][0], col[2][1], col[2][2]);
				setRGB3(q, col[3][0], col[3][1], col[3][2]);
				q->tpage = fig_tpage;
				q->clut = fig_clut[f->mat];
				addPrim(slot(z), q);
				prim += sizeof(POLY_GT4);
			} else if (tri) {
				POLY_G3 *q = (POLY_G3 *)prim;
				setPolyG3(q);
				setXY3(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
				setRGB0(q, col[0][0], col[0][1], col[0][2]);
				setRGB1(q, col[1][0], col[1][1], col[1][2]);
				setRGB2(q, col[2][0], col[2][1], col[2][2]);
				addPrim(slot(z), q);
				prim += sizeof(POLY_G3);
			} else {
				POLY_G4 *q = (POLY_G4 *)prim;
				setPolyG4(q);
				setXY4(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
				setRGB0(q, col[0][0], col[0][1], col[0][2]);
				setRGB1(q, col[1][0], col[1][1], col[1][2]);
				setRGB2(q, col[2][0], col[2][1], col[2][2]);
				setRGB3(q, col[3][0], col[3][1], col[3][2]);
				addPrim(slot(z), q);
				prim += sizeof(POLY_G4);
			}
		}
	}
	load_matrix(&cam);
}

/* A soft round shadow under a figure: the glow texture, subtracted. */
static void shadow(int x, int z, int r) {
	Proj p[4];
	project(x - r, -2, z + r, &p[0]);
	project(x + r, -2, z + r, &p[1]);
	project(x - r, -2, z - r, &p[2]);
	project(x + r, -2, z - r, &p[3]);
	if (!on_screen(p, 4) || prim + sizeof(POLY_FT4) > prim_end) return;
	POLY_FT4 *q = (POLY_FT4 *)prim;
	setPolyFT4(q);
	setSemiTrans(q, 1);
	setXY4(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
	int u = FX_GLOW * 64;
	setUV4(q, u, 64, u + 63, 64, u, 127, u + 63, 127);
	setRGB0(q, 70, 70, 70);
	q->tpage = fig_tpage_sub;
	q->clut = fig_clut[CLUT_GLOW];
	addPrim(slot(p[0].z) - 2, q);
	prim += sizeof(POLY_FT4);
}

/* ---- The floor ------------------------------------------------------------------- */

static int tele[MAX_N][MAX_N];
static int32_t fall_at[MAX_N][MAX_N];   /* floor_t when each slab starts to fall */

static void draw_floor(int frame) {
	int n = grid_n, px, py;
	int moving = (player.state == P_DODGE || player.state == P_WALK) && player.t < 5;
	px = moving ? player.from_x : player.x;
	py = moving ? player.from_y : player.y;
	int any_tele = 0;
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) any_tele |= tele[y][x] = game_telegraph(x, y);
	/* The light at each corner where four slabs meet. */
	static int16_t corner[MAX_N + 1][MAX_N + 1][3];
	for (int y = 0; y <= n; y++)
		for (int x = 0; x <= n; x++) {
			int wx, wz, r, g, b;
			tile_world(x * 16 - 8, y * 16 - 8, &wx, &wz);
			floor_light(wx, wz, &r, &g, &b);
			grade(&r, &g, &b);
			corner[y][x][0] = r; corner[y][x][1] = g; corner[y][x][2] = b;
		}
	for (int ty = 0; ty < n; ty++)
		for (int tx = 0; tx < n; tx++) {
			int wx, wz, dy = 0;
			tile_world(tx * 16, ty * 16, &wx, &wz);
			int tg = tele[ty][tx];
			if (tg >= 256) dy = (frame & 2) ? 3 : -2;   /* the slab trembles */
			/* Won: the outer ring breaks away into the ash, each slab in turn,
			 * but not while the soul stands on it. */
			if (floor_state != F_CLEARED) fall_at[ty][tx] = 0;
			else if ((tx == 0 || ty == 0 || tx == n - 1 || ty == n - 1) && !(tx == px && ty == py)) {
				if (!fall_at[ty][tx]) fall_at[ty][tx] = floor_t + 40 + (tx * 7 + ty * 13) % 40;
				int t = floor_t - fall_at[ty][tx];
				if (t > 0) dy += t * t / 3;
				if (dy > 2600) continue;
			} else if (fall_at[ty][tx] && floor_t > fall_at[ty][tx]) {
				fall_at[ty][tx] = floor_t;   /* stood on: waits */
			}
			int x0 = wx - TILE_U / 2 + SLAB_GAP, x1 = wx + TILE_U / 2 - SLAB_GAP;
			int z0 = wz - TILE_U / 2 + SLAB_GAP, z1 = wz + TILE_U / 2 - SLAB_GAP;
			Proj t[4], b[4];
			project(x0, dy, z1, &t[0]);
			project(x1, dy, z1, &t[1]);
			project(x0, dy, z0, &t[2]);
			project(x1, dy, z0, &t[3]);
			if (!on_screen(t, 4)) continue;
			if (prim + 6 * sizeof(POLY_GT4) > prim_end) return;
			/* Which sides face the camera (only those get their bottom corners). */
			int see[4] = { cam_z > z1, cam_z < z0, cam_x > x1, cam_x < x0 };   /* N S E W */
			/* Inner sides are thin strips from the fixed camera: only the
			 * platform's outer edge shows its thickness (and falling slabs). */
			if (!dy) {
				if (ty != 0) see[0] = 0;
				if (ty != n - 1) see[1] = 0;
				if (tx != n - 1) see[2] = 0;
				if (tx != 0) see[3] = 0;
			}
			if (see[0] || see[3]) project(x0, dy + SLAB_DEPTH, z1, &b[0]);
			if (see[0] || see[2]) project(x1, dy + SLAB_DEPTH, z1, &b[1]);
			if (see[1] || see[3]) project(x0, dy + SLAB_DEPTH, z0, &b[2]);
			if (see[1] || see[2]) project(x1, dy + SLAB_DEPTH, z0, &b[3]);
			/* Light: the soul's, the telegraph's red, the fog. */
			int col[4][3];
			int own = tx == px && ty == py && tg > 0;
			for (int i = 0; i < 4; i++) {
				const int16_t *cl = corner[ty + (i >> 1)][tx + (i & 1)];
				int r = cl[0], g = cl[1], bb = cl[2];
				/* Red glow from this and the neighbouring telegraphs. */
				if (any_tele) {
					int red = tg / 3;
					int nx = tx + (i & 1 ? 1 : -1), ny = ty + (i < 2 ? -1 : 1);
					if (nx >= 0 && nx < n && tele[ty][nx] / 6 > red) red = tele[ty][nx] / 6;
					if (ny >= 0 && ny < n && tele[ny][tx] / 6 > red) red = tele[ny][tx] / 6;
					r += red; g -= red / 4; bb -= red / 4;
				}
				if (own && (frame & 4)) { r += 90; g -= 30; bb -= 30; }   /* your own tile flashes */
				if (t[i].z > FOG_NEAR) {
					int f = 256 - fog_of(t[i].z);
					r = r * f >> 8; g = g * f >> 8; bb = bb * f >> 8;
				}
				col[i][0] = clamp255(r); col[i][1] = clamp255(g); col[i][2] = clamp255(bb);
			}
			int zmax = t[0].z;
			for (int i = 1; i < 4; i++) if (t[i].z > zmax) zmax = t[i].z;
			uint32_t *s = slot(zmax);
			POLY_GT4 *q = (POLY_GT4 *)prim;
			setPolyGT4(q);
			setXY4(q, t[0].x, t[0].y, t[1].x, t[1].y, t[2].x, t[2].y, t[3].x, t[3].y);
			int k = (tx * 5 + ty * 3 + (tx * ty) % 3) % NUM_SLABS, u = k * 64;
			setUV4(q, u, 0, u + 63, 0, u, 63, u + 63, 63);
			setRGB0(q, col[0][0], col[0][1], col[0][2]);
			setRGB1(q, col[1][0], col[1][1], col[1][2]);
			setRGB2(q, col[2][0], col[2][1], col[2][2]);
			setRGB3(q, col[3][0], col[3][1], col[3][2]);
			q->tpage = slab_tpage;
			q->clut = slab_clut[tg >= 256 ? 2 : tg > 0 ? 1 : 0];
			addPrim(s, q);
			prim += sizeof(POLY_GT4);
			/* The slab's sides: dark stone, lit like the top's edge. */
			const Proj *quads[4][4] = {
				{ &t[1], &t[0], &b[1], &b[0] },   /* north (+z) */
				{ &t[2], &t[3], &b[2], &b[3] },   /* south */
				{ &t[3], &t[1], &b[3], &b[1] },   /* east */
				{ &t[0], &t[2], &b[0], &b[2] },   /* west */
			};
			static const uint8_t CI[4][2] = { { 1, 0 }, { 2, 3 }, { 3, 1 }, { 0, 2 } };
			for (int sd = 0; sd < 4; sd++) {
				if (!see[sd]) continue;
				const Proj *a = quads[sd][0], *bb = quads[sd][1], *c = quads[sd][2], *d = quads[sd][3];
				Proj four[4] = { *a, *bb, *c, *d };
				if (!on_screen(four, 4)) continue;
				POLY_G4 *g = (POLY_G4 *)prim;
				setPolyG4(g);
				setXY4(g, a->x, a->y, bb->x, bb->y, c->x, c->y, d->x, d->y);
				const int *c0 = col[CI[sd][0]], *c1 = col[CI[sd][1]];
				setRGB0(g, c0[0] * 2 / 5, c0[1] * 2 / 5, c0[2] * 2 / 5);
				setRGB1(g, c1[0] * 2 / 5, c1[1] * 2 / 5, c1[2] * 2 / 5);
				setRGB2(g, c0[0] / 8, c0[1] / 8, c0[2] / 8);
				setRGB3(g, c1[0] / 8, c1[1] / 8, c1[2] / 8);
				addPrim(slot(zmax + 24), g);
				prim += sizeof(POLY_G4);
			}
		}
}

/* ---- Statues in the haze ---------------------------------------------------------- */

static void draw_statues(void) {
	/* Around the floor and far behind it, half sunk in the ash. Lit once,
	 * the first time they're drawn in calm (no lightning, nothing dying). */
	static uint8_t cache[4][160][4][3];
	static const int16_t AT[4][5] = {   /* x, y, z, yaw, light */
		{ -3900, 1300, 4200, 1400, 230 }, { 4000, 1100, 3600, 3300, 230 },
		{ 900, 1000, 6200, 300, 220 }, { -1800, 1100, 6800, 0, 210 },
	};
	const Model *M[4] = { &head_statue, &hand_statue, &pillar_statue, &pillar_statue };
	static int ready;
	int calm = !flash && !desat && dim == 256 && !nlit && !(floor_state == F_WAVE_INTRO && wave == 0);
	model_red = 0;
	for (int i = 0; i < 4; i++) {
		face_cache = cache[i];
		face_cache_ready = ready;
		face_cache_write = calm;
		/* Faces whose back is turned still take their slot in the cache. */
		draw_model(M[i], 0, AT[i][0], AT[i][1], AT[i][2], AT[i][3], AT[i][4], 0, 256, 0);
	}
	face_cache = 0;
	if (calm) ready = 1;
}

/* ---- Particles ----------------------------------------------------------------------- */

#define MAX_PARTICLES 128
enum { PK_ASH, PK_GLOW, PK_CHUNK };
typedef struct {
	int16_t x, y, z, vx, vy, vz;
	uint8_t life, r, g, b, size, gravity, kind;
} Particle;
static Particle parts[MAX_PARTICLES];

static uint32_t prng = 0x9e3779b9;
static int prand(int n) {
	prng = prng * 1664525 + 1013904223;
	return (prng >> 16) % n;
}

static void spawn(int kind, int x, int y, int z, int vx, int vy, int vz, int life, int r, int g, int b, int size, int gravity) {
	for (int i = 0; i < MAX_PARTICLES; i++) {
		Particle *p = &parts[i];
		if (p->life) continue;
		p->x = x; p->y = y; p->z = z; p->vx = vx; p->vy = vy; p->vz = vz;
		p->life = life; p->r = r; p->g = g; p->b = b; p->size = size; p->gravity = gravity; p->kind = kind;
		return;
	}
}

/* Sparks of light flying from a tile. */
static void burst(int tx, int ty, int n, int r, int g, int b, int speed, int height) {
	int x, z;
	tile_world(tx * 16, ty * 16, &x, &z);
	for (int i = 0; i < n; i++)
		spawn(PK_GLOW, x + prand(60) - 30, -height - prand(60), z + prand(60) - 30,
		      prand(speed * 2 + 1) - speed, -prand(speed) - speed / 2, prand(speed * 2 + 1) - speed,
		      20 + prand(20), r, g, b, 14 + prand(10), 1);
}

/* A husk breaks into stone: chunks that tumble and bounce, embers rising. */
static void shatter(int tx, int ty) {
	int x, z;
	tile_world(tx * 16, ty * 16, &x, &z);
	for (int i = 0; i < 16; i++)
		spawn(PK_CHUNK, x + prand(120) - 60, -40 - prand(260), z + prand(120) - 60,
		      prand(25) - 12, -prand(14) - 4, prand(25) - 12, 60 + prand(30), 0, 0, 0, 50 + prand(60), 2);
	for (int i = 0; i < 14; i++)
		spawn(PK_GLOW, x + prand(140) - 70, -60 - prand(220), z + prand(140) - 70,
		      prand(5) - 2, -2 - prand(3), prand(5) - 2, 40 + prand(30), 255, 130, 50, 8 + prand(8), 0);
}

static void update_particles(int frame, int ticked) {
	int ash = 0;
	int edge = grid_n * TILE_U / 2;
	for (int i = 0; i < MAX_PARTICLES; i++) {
		Particle *p = &parts[i];
		if (!p->life) continue;
		if (p->kind == PK_ASH) ash++;
		if (!ticked) continue;
		if (p->life < 255) p->life--;
		p->x += p->vx; p->y += p->vy; p->z += p->vz;
		if (p->gravity) {
			p->vy += p->gravity;
			/* Chunks bounce on the floor (and fall past its edge). */
			if (p->kind == PK_CHUNK && p->y > -p->size / 3 && p->vy > 0 &&
			    p->x > -edge && p->x < edge && p->z > -edge && p->z < edge) {
				p->y = -p->size / 3;
				p->vy = -p->vy * 2 / 5;
				p->vx = p->vx * 3 / 4; p->vz = p->vz * 3 / 4;
				if (p->vy > -2) { p->vy = 0; p->gravity = 0; p->vx = p->vz = 0; }
			}
		} else if (p->kind == PK_ASH) {
			p->x += isin((frame * 20 + i * 300) & 4095) >> 11;   /* drifting */
			if (p->y > 700) p->life = 0;
		}
	}
	/* Ash keeps falling around the soul. */
	while (ash < 44) {
		spawn(PK_ASH, soul_x + prand(2600) - 1300, -900 - prand(300), soul_z + prand(2600) - 1100, 0, 2 + prand(3), 0, 255, 150, 146, 140, 7 + prand(5), 0);
		ash++;
	}
}

static void draw_particles(int frame) {
	for (int i = 0; i < MAX_PARTICLES; i++) {
		const Particle *p = &parts[i];
		if (!p->life) continue;
		Proj s;
		project(p->x, p->y, p->z, &s);
		if (s.z < NEAR_Z || s.x < -20 || s.x >= 340 || s.y < -20 || s.y >= 260) continue;
		int sz = p->size * SCREEN_H / s.z;
		if (p->kind == PK_GLOW) {
			/* Additive: fades out at the end of its life. */
			int k = p->life < 16 ? p->life * 16 : 256;
			int r = p->r * k >> 9, g = p->g * k >> 9, b = p->b * k >> 9;
			grade(&r, &g, &b);
			sz = sz * 2 / 3;
			if (sz < 2) sz = 2;
			if (sz > 9) sz = 9;   /* additive fill is dear: keep sparks small */
			sprite(slot(s.z) - 1, s.x, s.y, sz, sz, FX_GLOW, CLUT_GLOW, r, g, b, 1, 0);
			continue;
		}
		if (p->kind == PK_CHUNK) {
			int r, g, b;
			floor_light(p->x, p->z, &r, &g, &b);
			r = r * 9 >> 4; g = g * 9 >> 4; b = b * 9 >> 4;   /* darker than the slabs */
			grade(&r, &g, &b);
			if (p->life < 20) { r = r * p->life / 20; g = g * p->life / 20; b = b * p->life / 20; }
			fogged(&r, &g, &b, s.z);
			sz = sz * 3 / 4;
			if (sz < 3) sz = 3;
			if (sz > 16) sz = 16;
			int flip = p->gravity ? ((frame >> 2) + i) & 3 : i & 3;   /* tumbling while it flies */
			sprite(slot(s.z), s.x, s.y, sz, sz, FX_CHUNK, CLUT_STONE, clamp255(r), clamp255(g), clamp255(b), 0, flip);
			continue;
		}
		if (sz < 1) sz = 1;
		if (sz > 6) sz = 6;
		int r = p->r * 3 / 4, g = p->g * 3 / 4, b = p->b * 3 / 4;
		fogged(&r, &g, &b, s.z);
		TILE *t = (TILE *)prim;
		setTile(t);
		setXY0(t, s.x, s.y);
		setWH(t, sz, sz);
		setRGB0(t, r, g, b);
		addPrim(slot(s.z) - 1, t);
		prim += sizeof(TILE);
	}
}

/* ---- Glyph billboards and the HUD ---------------------------------------------------- */

static void glyph(uint32_t *s, int g, int cx, int cy, int size, int r, int gr, int b, int glow) {
	if (prim + sizeof(POLY_FT4) > prim_end) return;
	POLY_FT4 *q = (POLY_FT4 *)prim;
	setPolyFT4(q);
	int h = size / 2;
	setXY4(q, cx - h, cy - h, cx - h + size, cy - h, cx - h, cy - h + size, cx - h + size, cy - h + size);
	int u = (g % 15) * 16, v = (g / 15) * 16;
	setUV4(q, u, v, u + 16, v, u, v + 16, u + 16, v + 16);
	setRGB0(q, r >> 1, gr >> 1, b >> 1);
	q->tpage = glyph_tpage;
	q->clut = glyph_clut;
	if (glow) setSemiTrans(q, 1);
	addPrim(s, q);
	prim += sizeof(POLY_FT4);
}

static void box2d(int layer, int x, int y, int w, int h, int r, int g, int b) {
	if (w <= 0 || h <= 0) return;
	TILE *t = (TILE *)prim;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	addPrim(ot + layer, t);
	prim += sizeof(TILE);
}

static const char *ROMAN[] = { "I", "II", "III", "IV", "V", "VI", "VII" };

/* The HUD: two carved panels on top (life, stamina, flasks; absolution),
 * the wave, messages in a band across the middle. */
static void draw_hud(int frame) {
	char line[48];
	const Player *p = &player;
	ui_begin(ot, prim, prim_end);
	panel(6, 5, 148, 25, 1);
	bar(11, 9, 100, 7, p->hp, HP_MAX, 200, 36, 30, 0);
	bar(11, 19, 100, 6, p->stamina, STAMINA_MAX * 16, 150, 170, 80, 0);
	prim = ui_end();
	for (int i = 0; i < 3; i++)
		glyph(ot, G_FLASK, 122 + i * 11, 17, 14, i < p->flasks ? 120 : 30, i < p->flasks ? 200 : 34, i < p->flasks ? 170 : 40, 0);
	/* Absolution: a flame that never quite holds still. */
	int fl = 15 + ((frame * 7) % 5 == 0) + (isin((frame * 150) & 4095) >> 11);
	glyph(ot, G_FLAME, 262, 16 - (fl - 15), fl, 240, 170 + (isin((frame * 90) & 4095) >> 8), 70, 0);
	ui_begin(ot, prim, prim_end);
	panel(250, 5, 64, 25, 1);
	sprintf(line, "%d", absolution);
	text(306, 10, line, FONT_SMALL, 230, 205, 150, 2, 0);
	if (floor_state == F_FIGHT || floor_state == F_WAVE_INTRO) {
		sprintf(line, "WAVE %s", ROMAN[wave < 3 ? wave : 2]);
		text(202, 11, line, FONT_SMALL, 170, 160, 150, 1, 0);
	}
	if (message && (message_t > 0 || floor_state == F_DEAD || floor_state == F_CLEARED)) {
		int big = text_width(message, FONT_LARGE) <= 280;
		int dead = floor_state == F_DEAD;
		int y = dead ? 82 : 92;
		dark_box(0, y, 320, big ? 38 : 26, 120, 2);
		text(160, y + (big ? 5 : 6), message, big ? FONT_LARGE : FONT_SMALL, dead ? 210 : 220, dead ? 50 : 200, dead ? 40 : 160, 1, 0);
	}
	if (floor_state == F_DEAD) {
		if (floor_t > 90 && (game_frames & 32)) text(160, 126, "PRESS CROSS TO RISE AGAIN", FONT_SMALL, 200, 190, 180, 1, 0);
		sprintf(line, "GUILT %d", guilt + 1);
		text(160, 148, line, FONT_SMALL, 150, 120, 110, 1, 0);
	}
	prim = ui_end();
}

/* ---- Poses ----------------------------------------------------------------------------- */

/* Smoothstep, k 0..256. */
static int ease256(int k) {
	if (k <= 0) return 0;
	if (k >= 256) return 256;
	return k * k * (768 - 2 * k) >> 16;
}

/* A figure's animation: what it's playing, and the pose it was in when that
 * started, blended out over the first frames. */
typedef struct {
	int anim, state;
	int since;   /* frames since it changed */
	Pose from, last;
} Animator;

#define BLEND_FRAMES 6

/* t: frames into the animation (the game's own timers, so hit-stop freezes
 * it); blend: frames to blend in from the previous pose. */
static void animate(Animator *an, int state, int anim, int t, int len, int blend, Pose *out) {
	if (state != an->state || anim != an->anim) {
		an->from = an->last;
		an->since = an->state < 0 ? 999 : 0;   /* first sight: no blend */
		an->state = state;
		an->anim = anim;
	}
	anim_sample(anim, t, len, out);
	if (an->since < blend) pose_blend(&an->from, out, ease256(an->since * 256 / blend), out);
	if (an->since < 999) an->since++;
	an->last = *out;
}

static Animator soul_anim = { -1, -1 };

static void soul_pose(Pose *ps) {
	const Player *p = &player;
	switch (p->state) {
	case P_WALK:   animate(&soul_anim, P_WALK, A_SOUL_WALK, p->t, WALK_FRAMES, BLEND_FRAMES, ps); break;
	case P_DODGE:  animate(&soul_anim, P_DODGE, A_SOUL_DODGE, p->t, 9, 2, ps); break;
	case P_JUMP:   animate(&soul_anim, P_JUMP, A_SOUL_JUMP, p->t, JUMP_FRAMES, 3, ps); break;
	case P_CHARGE:
		animate(&soul_anim, P_CHARGE, A_SOUL_CHARGE, p->t, CHARGE_FULL, 3, ps);
		if (p->t >= CHARGE_FULL && (game_frames & 2)) ps->pitch[SOUL_SWORD_ARM] += 60;   /* straining */
		break;
	case P_ATTACK:
		if (p->heavy) animate(&soul_anim, P_ATTACK + 100, A_SOUL_HEAVY, p->t, 30, 0, ps);
		else animate(&soul_anim, P_ATTACK, A_SOUL_ATTACK, p->t, 22, 2, ps);
		break;
	case P_DRINK:  animate(&soul_anim, P_DRINK, A_SOUL_DRINK, p->t, 45, BLEND_FRAMES, ps); break;
	case P_DEAD:   animate(&soul_anim, P_DEAD, A_SOUL_DEAD, p->t, 60, 4, ps); break;
	default:       animate(&soul_anim, P_IDLE, A_SOUL_IDLE, game_frames, 120, BLEND_FRAMES, ps); break;
	}
	/* Flinch when hit. */
	if (p->hurt_t < 8 && p->state != P_DEAD) {
		int k = 8 - p->hurt_t;
		ps->lean -= k * 25;
		ps->pitch[SOUL_HEAD] -= k * 30;
	}
}

static Animator husk_anim[MAX_ENEMIES];

static void husk_pose(int i, Pose *ps) {
	const Enemy *e = &enemies[i];
	Animator *an = &husk_anim[i];
	int phase = i * 37;   /* not all breathing together */
	switch (e->state) {
	case E_MOVE:    animate(an, E_MOVE, A_HUSK_MOVE, e->t, 36, BLEND_FRAMES, ps); break;
	case E_WINDUP:
		animate(an, E_WINDUP, A_HUSK_WINDUP, e->t, e->windup, 8, ps);
		if (e->attack == 2) { ps->yaw[HUSK_RIGHT_ARM] += 600; ps->yaw[HUSK_LEFT_ARM] -= 600; }   /* the cross: arms spread */
		if (e->t > e->windup - 15) ps->dy += (game_frames & 2) ? -2 : 2;                        /* about to fall */
		break;
	case E_RECOVER: animate(an, E_RECOVER, A_HUSK_RECOVER, e->t, 50, 2, ps); break;
	case E_STAGGER: animate(an, E_STAGGER, A_HUSK_STAGGER, e->t, 30, 4, ps); break;
	case E_DYING:   *ps = an->last; break;
	default:        animate(an, E_IDLE, A_HUSK_IDLE, game_frames + phase, 100, BLEND_FRAMES, ps); break;
	}
}

/* ---- Effects from game events --------------------------------------------------------- */

static int enemy_yaw[MAX_ENEMIES];

static void handle_events(void) {
	for (int i = 0; i < num_events; i++) {
		const GameEvent *e = &game_events[i];
		int s = 0, c = 4096;   /* kicks go away from the camera */
		switch (e->type) {
		case EV_HIT:
			burst(e->x, e->y, 10, 230, 210, 180, 10, 150);
			light_tile(e->x, e->y, 220, 200, 170, 20, 12);
			kick_x += s * 14 >> 12; kick_z += c * 14 >> 12;
			break;
		case EV_HEAVY:
			burst(e->x, e->y, 18, 255, 220, 170, 14, 150);
			light_tile(e->x, e->y, 255, 220, 160, 30, 24);
			kick_x += s * 30 >> 12; kick_z += c * 30 >> 12;
			zoom += 90;
			break;
		case EV_RIPOSTE:
			burst(e->x, e->y, 24, 255, 200, 120, 16, 150);
			light_tile(e->x, e->y, 255, 200, 120, 36, 30);
			zoom += 120;
			break;
		case EV_KILL:
			shatter(e->x, e->y);
			light_tile(e->x, e->y, 255, 120, 40, 28, 40);
			break;
		case EV_CHARGED:
			burst(player.x, player.y, 12, 255, 220, 130, 8, 230);
			light_tile(player.x, player.y, 255, 210, 110, 24, 18);
			break;
		case EV_JUMP:
		case EV_LAND:
			burst(e->x, e->y, 6, 150, 140, 130, 6, 0);
			break;
		case EV_HURT:
			burst(e->x, e->y, 12, 220, 40, 30, 9, 120);
			light_tile(e->x, e->y, 230, 40, 30, 22, 16);
			break;
		case EV_SLAM:
			burst(e->x, e->y, 14, 150, 120, 90, 12, 10);
			break;
		case EV_DEATH:
			burst(e->x, e->y, 30, 170, 180, 220, 6, 90);
			break;
		case EV_HEAL:
			light_tile(player.x, player.y, 120, 220, 170, 26, 40);
			break;
		}
	}
}

/* ---- The frame ------------------------------------------------------------------------- */

uint8_t *draw_frame(uint32_t *ot_, uint8_t *prim_, uint8_t *end, int frame, int ticked) {
	ot = ot_;
	prim = prim_;
	prim_end = end;

	/* Where the soul is (between tiles while dodging). */
	int fx = player.x * 16, fy = player.y * 16;
	if (player.state == P_DODGE || player.state == P_WALK) {
		int n = player.state == P_WALK ? WALK_FRAMES : 9;
		fx = player.from_x * 16 + (player.x - player.from_x) * 16 * player.t / n;
		fy = player.from_y * 16 + (player.y - player.from_y) * 16 * player.t / n;
	}
	tile_world(fx, fy, &soul_x, &soul_z);

	/* The soul falls: its light goes out, the colour drains away. */
	if (floor_state == F_DEAD) {
		if (death_t < 0) death_t = 0;
		death_t++;
		soul_glow = death_t * 3 < 256 ? 256 - death_t * 3 : 0;
		desat = death_t * 3 < 230 ? death_t * 3 : 230;
		dim = death_t < 70 ? 256 - death_t : 186;
	} else {
		death_t = -1;
		soul_glow = 256; desat = 0; dim = 256;
	}

	update_camera(frame);
	if (ticked) handle_events();
	update_particles(frame, ticked);
	update_weather(frame);
	gather_lights(ticked);

	draw_sky(frame);
	draw_clouds(frame);
	draw_statues();
	draw_floor(frame);

	/* Absolution where you fell: a flame floating over the slab. */
	if (drop_amount > 0) {
		int x, z;
		tile_world(drop_x * 16, drop_y * 16, &x, &z);
		Proj p;
		project(x, -70 + (isin((frame * 60) & 4095) >> 9), z, &p);
		if (p.z > NEAR_Z) {
			int sz = 90 * SCREEN_H / p.z;
			glyph(slot(p.z) - 1, G_FLAME, p.x, p.y, sz, 255, 200, 90, 1);
			if (frame & 1) spawn(PK_GLOW, x + prand(40) - 20, -60, z + prand(40) - 20, 0, -2, 0, 20, 255, 170, 70, 8, 0);
		}
	}

	/* The husks. */
	for (int i = 0; i < MAX_ENEMIES; i++) {
		const Enemy *e = &enemies[i];
		if (e->state == E_NONE) { enemy_yaw[i] = -1; husk_anim[i].state = -1; continue; }
		int ex = e->x * 16, ey = e->y * 16;
		if (e->state == E_MOVE) {
			ex = e->from_x * 16 + (e->x - e->from_x) * 16 * e->t / 36;
			ey = e->from_y * 16 + (e->y - e->from_y) * 16 * e->t / 36;
		}
		int x, z;
		tile_world(ex, ey, &x, &z);
		/* Turn to face the soul. */
		int want = 0;
		{
			int dx = soul_x - x, dz = soul_z - z, adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
			want = adx > adz ? (dx > 0 ? 1024 : 3072) : (dz > 0 ? 0 : 2048);
		}
		if (enemy_yaw[i] < 0) enemy_yaw[i] = want;
		if (e->state != E_WINDUP && e->state != E_RECOVER) enemy_yaw[i] = ease_angle(enemy_yaw[i], want, 6);
		Pose ps;
		husk_pose(i, &ps);
		/* Killed: a white flash, then it's the stone chunks. */
		if (e->state == E_DYING && e->t > 3) continue;
		int tint = e->hit_t < 5 || e->state == E_DYING ? 1 : e->state == E_STAGGER ? 3 : 0;
		model_red = e->x >= 0 && e->x < grid_n && e->y >= 0 && e->y < grid_n ? tele[e->y][e->x] / 3 : 0;
		draw_model(&husk_model, &ps, x, 0, z, enemy_yaw[i], 256, tint, 256, 0);
		shadow(x, z, 110);
		/* Health over it once hurt. */
		if (e->hp < 60 && e->state != E_DYING) {
			Proj p;
			project(x, -270, z, &p);
			if (p.z > NEAR_Z) {
				box2d(2, p.x - 12, p.y, 24, 3, 30, 10, 10);
				box2d(1, p.x - 12, p.y, e->hp * 24 / 60, 3, 200, 40, 40);
			}
		}
	}

	/* The soul. */
	{
		Pose ps;
		soul_pose(&ps);
		int tint = player.hurt_t < 8 ? 2 : 0;
		int dodge_glow = player.state == P_DODGE && player.t >= 2 && player.t <= 7;
		model_red = tele[player.y][player.x] / 3;
		model_glow = soul_glow;
		draw_model(&soul_model, &ps, soul_x, 0, soul_z, player.facing * 1024, dodge_glow ? 300 : 256, tint, 256, 1);
		model_glow = 256;
		/* Off the ground: the shadow shrinks under it. */
		shadow(soul_x, soul_z, 80 + (ps.dy < 0 ? ps.dy / 4 : 0));
		/* The sword's arc as it strikes. */
		int strike = player.heavy ? 6 : 8;
		if (player.state == P_ATTACK && player.t >= strike && player.t < strike + 5) {
			static const int8_t DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };
			int ax, az;
			tile_world((player.x + DX[player.facing]) * 16, (player.y + DY[player.facing]) * 16, &ax, &az);
			Proj p;
			project((soul_x + ax) / 2, -110, (soul_z + az) / 2, &p);
			if (p.z > NEAR_Z) glyph(slot(p.z) - 2, G_SLASH, p.x, p.y, (player.heavy ? 300 : 200) * SCREEN_H / p.z, 255, player.heavy ? 220 : 255, player.heavy ? 150 : 255, 1);
		}
	}

	draw_particles(frame);
	draw_hud(frame);
	return prim;
}
