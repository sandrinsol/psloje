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
#include "game.h"
#include "glyphs.h"
#include "models.h"
#include "slabs.h"
#include "ui.h"

extern const uint32_t glyphs_tim[], slabs_tim[];

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
static uint8_t *prim, *prim_end;
static uint32_t *ot;

/* Camera. */
static MATRIX cam;
static int32_t cam_x, cam_y, cam_z, kick_x, kick_z, zoom;
static int32_t soul_x, soul_z;   /* world position of the soul (for its light) */

void draw_init(void) {
	TIM_IMAGE tim;
	GetTimInfo(glyphs_tim, &tim);
	LoadImage(tim.prect, tim.paddr);
	LoadImage(tim.crect, tim.caddr);
	GetTimInfo(slabs_tim, &tim);
	LoadImage(tim.prect, tim.paddr);
	LoadImage(tim.crect, tim.caddr);
	DrawSync(0);
	glyph_tpage = getTPage(0, 1, GLYPH_TPAGE_X, GLYPH_TPAGE_Y);
	glyph_clut = getClut(GLYPH_CLUT_X, GLYPH_CLUT_Y);
	slab_tpage = getTPage(0, 0, SLAB_TPAGE_X, SLAB_TPAGE_Y);
	for (int i = 0; i < 3; i++) slab_clut[i] = getClut(SLAB_CLUT_X, SLAB_CLUT_Y + i);
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

static inline int on_screen(const Proj *p, int n) {
	int l = 1, r = 1, t = 1, bo = 1;
	for (int i = 0; i < n; i++) {
		if (p[i].z < NEAR_Z || p[i].x < -600 || p[i].x > 920 || p[i].y < -600 || p[i].y > 840) return 0;
		l &= p[i].x < 0; r &= p[i].x >= 320; t &= p[i].y < 0; bo &= p[i].y >= 240;
	}
	return !(l | r | t | bo);
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

static void draw_sky(void) {
	POLY_G4 *q = (POLY_G4 *)prim;
	setPolyG4(q);
	setXY4(q, 0, 0, 320, 0, 0, 240, 320, 240);
	setRGB0(q, SKY_TOP.r, SKY_TOP.g, SKY_TOP.b);
	setRGB1(q, SKY_TOP.r, SKY_TOP.g, SKY_TOP.b);
	setRGB2(q, SKY_BOTTOM.r, SKY_BOTTOM.g, SKY_BOTTOM.b);
	setRGB3(q, SKY_BOTTOM.r, SKY_BOTTOM.g, SKY_BOTTOM.b);
	addPrim(ot + OT_LEN - 1, q);
	prim += sizeof(POLY_G4);
}

/* Banks of ash cloud drifting under the floor. */
static void draw_clouds(int frame) {
	static const int16_t C[][4] = {   /* x, y, z, size */
		{ -1800, 700, 900, 1500 }, { 1500, 900, -600, 1700 }, { 200, 1200, 2600, 2000 },
		{ -900, 1000, -2200, 1800 }, { 2400, 650, 1800, 1300 }, { -2600, 850, -400, 1500 },
	};
	uint32_t *last = 0;
	for (int i = 0; i < 6; i++) {
		int drift = ((frame + i * 300) % 2400) - 1200;
		int x = C[i][0] + drift, y = C[i][1], z = C[i][2], h = C[i][3];
		Proj p[4];
		project(x - h, y, z + h, &p[0]);
		project(x + h, y, z + h, &p[1]);
		project(x - h, y, z - h, &p[2]);
		project(x + h, y, z - h, &p[3]);
		if (!on_screen(p, 4)) continue;
		POLY_G4 *q = (POLY_G4 *)prim;
		setPolyG4(q);
		setSemiTrans(q, 1);
		setXY4(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
		int f = 80 - fog_of(p[0].z) / 6;
		setRGB0(q, f / 3, f / 3, f / 3);
		setRGB1(q, f / 2, f / 2, f / 2);
		setRGB2(q, f / 2, f / 2, f / 2);
		setRGB3(q, f / 4, f / 4, f / 4);
		last = ot + OT_LEN - 2;
		addPrim(last, q);
		prim += sizeof(POLY_G4);
	}
	if (last) blend(last, 1);   /* additive */
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

/* tint: 0 normal, 1 white (hit), 2 red (hurt), 3 blue (staggered);
 * light 0..256 how lit; fade 256 = solid. Each part goes through the GTE
 * with its own matrix: camera x placement x pose. */
static void draw_model(const Model *m, const Pose *pose, int wx, int wy, int wz, int yaw, int light, int tint, int fade) {
	Proj pv[64];
	Rot cr, obj;
	for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) cr.m[r][c] = cam.m[r][c];
	rot_make(pose ? pose->lean : 0, yaw, &obj);
	int dy = pose ? pose->dy : 0;
	for (int pi = 0; pi < m->nparts; pi++) {
		const Part *pt = &m->parts[pi];
		Rot prt, world, full;
		rot_make(pose ? pose->pitch[pi] : 0, pose ? pose->yaw[pi] : 0, &prt);
		rot_mul(&obj, &prt, &world);            /* part -> world rotation */
		rot_mul(&cr, &world, &full);            /* part -> camera */
		/* Where the part's origin lands: obj * (pivot - prt * pivot) + position. */
		int qx, qy, qz, ox, oy, oz;
		rot_apply(&prt, pt->px, pt->py, pt->pz, &qx, &qy, &qz);
		rot_apply(&obj, pt->px - qx, pt->py - qy, pt->pz - qz, &ox, &oy, &oz);
		ox += wx; oy += wy + dy; oz += wz;
		MATRIX g;
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) g.m[r][c] = full.m[r][c];
			g.t[r] = ((cam.m[r][0] * ox + cam.m[r][1] * oy + cam.m[r][2] * oz) >> 12) + cam.t[r];
		}
		load_matrix(&g);
		for (int i = 0; i < pt->nverts && i < 64; i++) project(pt->verts[i].x, pt->verts[i].y, pt->verts[i].z, &pv[i]);
		for (int fi = 0; fi < pt->nfaces; fi++) {
			const MFace *f = &pt->faces[fi];
			int tri = f->v[3] == 255;
			Proj p[4] = { pv[f->v[0]], pv[f->v[1]], pv[f->v[2]], tri ? pv[f->v[2]] : pv[f->v[3]] };
			if (!on_screen(p, tri ? 3 : 4) || !front(&p[0], &p[1], &p[2])) continue;
			if (prim + sizeof(POLY_F4) > prim_end) break;
			int r = f->r, g2 = f->g, b = f->b;
			if (!f->glow) {
				int nx, ny, nz;
				rot_apply(&world, f->nx, f->ny, f->nz, &nx, &ny, &nz);
				int d = (nx * LIGHT[0] + ny * LIGHT[1] + nz * LIGHT[2]) >> 12;
				int shade = AMBIENT + ((d > 0 ? d : 0) * (256 - AMBIENT) >> 12);
				shade = shade * light >> 8;
				r = r * shade >> 7; g2 = g2 * shade >> 7; b = b * shade >> 7;
			}
			if (tint == 1) { r = g2 = b = 240; }
			else if (tint == 2) { r = r / 2 + 140; g2 /= 3; b /= 3; }
			else if (tint == 3) { r = r * 2 / 3; g2 = g2 * 3 / 4; b = b + 80; }
			int z = (p[0].z + p[1].z + p[2].z) / 3;
			r = r * fade >> 8; g2 = g2 * fade >> 8; b = b * fade >> 8;
			fogged(&r, &g2, &b, z);
			if (tri) {
				POLY_F3 *q = (POLY_F3 *)prim;
				setPolyF3(q);
				setXY3(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
				setRGB0(q, clamp255(r), clamp255(g2), clamp255(b));
				addPrim(slot(z), q);
				prim += sizeof(POLY_F3);
			} else {
				POLY_F4 *q = (POLY_F4 *)prim;
				setPolyF4(q);
				setXY4(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
				setRGB0(q, clamp255(r), clamp255(g2), clamp255(b));
				addPrim(slot(z), q);
				prim += sizeof(POLY_F4);
			}
		}
	}
	load_matrix(&cam);
}

/* The soul's light at a point: bright around it, fading over ~2 tiles. */
static int light_at(int x, int z) {
	int dx = (x - soul_x) >> 4, dz = (z - soul_z) >> 4;   /* 1/16 tile */
	int d2 = dx * dx + dz * dz;
	const int R2 = 40 * 40;
	return AMBIENT + SOUL_LIGHT * R2 / (R2 + d2);
}

static void shadow(int x, int z, int r) {
	Proj p[4];
	project(x - r, -2, z + r, &p[0]);
	project(x + r, -2, z + r, &p[1]);
	project(x - r, -2, z - r, &p[2]);
	project(x + r, -2, z - r, &p[3]);
	if (!on_screen(p, 4)) return;
	POLY_F4 *q = (POLY_F4 *)prim;
	setPolyF4(q);
	setSemiTrans(q, 1);
	setXY4(q, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
	setRGB0(q, 26, 26, 26);
	uint32_t *s = slot(p[0].z) - 2;
	addPrim(s, q);
	prim += sizeof(POLY_F4);
	blend(s, 2);   /* subtractive */
}

/* ---- The floor ------------------------------------------------------------------- */

static int tele[MAX_N][MAX_N];
static int32_t fall_at[MAX_N][MAX_N];   /* floor_t when each slab starts to fall */

static void draw_floor(int frame) {
	int n = grid_n, px, py;
	int moving = (player.state == P_DODGE || player.state == P_WALK) && player.t < 5;
	px = moving ? player.from_x : player.x;
	py = moving ? player.from_y : player.y;
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++) tele[y][x] = game_telegraph(x, y);
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
			int cx[4] = { x0, x1, x0, x1 }, cz[4] = { z1, z1, z0, z0 };
			int col[4][3];
			int own = tx == px && ty == py && tg > 0;
			for (int i = 0; i < 4; i++) {
				int l = light_at(cx[i], cz[i]);
				int r = l * 128 >> 8, g = r, bb = r + 6;
				/* Red glow from this and the neighbouring telegraphs. */
				int red = tg / 3;
				int nx = tx + (i & 1 ? 1 : -1), ny = ty + (i < 2 ? -1 : 1);
				if (nx >= 0 && nx < n && tele[ty][nx] / 6 > red) red = tele[ty][nx] / 6;
				if (ny >= 0 && ny < n && tele[ny][tx] / 6 > red) red = tele[ny][tx] / 6;
				r += red; g -= red / 4; bb -= red / 4;
				if (own && (frame & 4)) { r += 90; g -= 30; bb -= 30; }   /* your own tile flashes */
				int f = 256 - fog_of(t[i].z);
				col[i][0] = clamp255(r * f >> 8); col[i][1] = clamp255(g * f >> 8); col[i][2] = clamp255(bb * f >> 8);
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
	/* Around the floor and far behind it, half sunk in the ash. */
	draw_model(&head_statue, 0, -3900, 1300, 4200, 1400, 200, 0, 256);
	draw_model(&hand_statue, 0, 4000, 1100, 3600, 3300, 200, 0, 256);
	draw_model(&pillar_statue, 0, 900, 1000, 6200, 300, 190, 0, 256);
	draw_model(&pillar_statue, 0, -1800, 1100, 6800, 0, 180, 0, 256);
}

/* ---- Particles ----------------------------------------------------------------------- */

#define MAX_PARTICLES 96
typedef struct {
	int16_t x, y, z, vx, vy, vz;
	uint8_t life, r, g, b, size, gravity;
} Particle;
static Particle parts[MAX_PARTICLES];

static uint32_t prng = 0x9e3779b9;
static int prand(int n) {
	prng = prng * 1664525 + 1013904223;
	return (prng >> 16) % n;
}

static void spawn(int x, int y, int z, int vx, int vy, int vz, int life, int r, int g, int b, int size, int gravity) {
	for (int i = 0; i < MAX_PARTICLES; i++) {
		Particle *p = &parts[i];
		if (p->life) continue;
		p->x = x; p->y = y; p->z = z; p->vx = vx; p->vy = vy; p->vz = vz;
		p->life = life; p->r = r; p->g = g; p->b = b; p->size = size; p->gravity = gravity;
		return;
	}
}

static void burst(int tx, int ty, int n, int r, int g, int b, int speed, int height) {
	int x, z;
	tile_world(tx * 16, ty * 16, &x, &z);
	for (int i = 0; i < n; i++)
		spawn(x + prand(60) - 30, -height - prand(60), z + prand(60) - 30,
		      prand(speed * 2 + 1) - speed, -prand(speed) - speed / 2, prand(speed * 2 + 1) - speed,
		      20 + prand(20), r, g, b, 10 + prand(8), 1);
}

static void update_particles(int frame) {
	int ash = 0;
	for (int i = 0; i < MAX_PARTICLES; i++) {
		Particle *p = &parts[i];
		if (!p->life) continue;
		if (p->life < 255) p->life--;
		p->x += p->vx; p->y += p->vy; p->z += p->vz;
		if (p->gravity) p->vy += 1;
		else {
			ash++;
			p->x += isin((frame * 20 + i * 300) & 4095) >> 11;   /* drifting */
			if (p->y > 700) p->life = 0;
		}
	}
	/* Ash keeps falling around the soul. */
	while (ash < 44) {
		spawn(soul_x + prand(2600) - 1300, -900 - prand(300), soul_z + prand(2600) - 1100, 0, 2 + prand(3), 0, 255, 150, 146, 140, 7 + prand(5), 0);
		ash++;
	}
}

static void draw_particles(void) {
	for (int i = 0; i < MAX_PARTICLES; i++) {
		const Particle *p = &parts[i];
		if (!p->life) continue;
		Proj s;
		project(p->x, p->y, p->z, &s);
		if (s.z < NEAR_Z || s.x < 0 || s.x >= 320 || s.y < 0 || s.y >= 240) continue;
		int sz = p->size * SCREEN_H / s.z;
		if (sz < 1) sz = 1;
		if (sz > 6) sz = 6;
		int r = p->r, g = p->g, b = p->b;
		if (!p->gravity) { r = r * 3 / 4; g = g * 3 / 4; b = b * 3 / 4; }
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
		if (game_frames & 32) text(160, 126, "PRESS CROSS TO RISE AGAIN", FONT_SMALL, 200, 190, 180, 1, 0);
		sprintf(line, "GUILT %d", guilt + 1);
		text(160, 148, line, FONT_SMALL, 150, 120, 110, 1, 0);
	}
	prim = ui_end();
}

/* ---- Poses ----------------------------------------------------------------------------- */

static int lerp(int a, int b, int t, int n) {
	if (t <= 0) return a;
	if (t >= n) return b;
	return a + (b - a) * t / n;
}

static void soul_pose(Pose *ps) {
	const Player *p = &player;
	*ps = (Pose){ { 0, 380, 120 }, { 0, -120, 0 }, 0, 0 };
	int breathe = isin((game_frames * 40) & 4095) >> 10;
	ps->dy = breathe;
	switch (p->state) {
	case P_ATTACK:
		if (p->t < 8) { ps->pitch[SOUL_SWORD_ARM] = lerp(380, -1500, p->t, 6); ps->lean = lerp(0, -120, p->t, 6); }
		else { ps->pitch[SOUL_SWORD_ARM] = lerp(-1500, 1300, p->t - 8, 3); ps->yaw[SOUL_SWORD_ARM] = lerp(-300, 300, p->t - 8, 4); ps->lean = lerp(-120, 220, p->t - 8, 3); }
		if (p->t > 14) { ps->pitch[SOUL_SWORD_ARM] = lerp(1300, 380, p->t - 14, 8); ps->lean = lerp(220, 0, p->t - 14, 8); }
		break;
	case P_WALK:
		ps->dy = -(isin(p->t * 2048 / WALK_FRAMES) >> 9);   /* a small bob */
		ps->pitch[SOUL_ARM] = 120 + (isin(p->t * 4096 / WALK_FRAMES) >> 3);
		break;
	case P_DODGE:
		ps->lean = 500; ps->dy = 30 - (p->t - 4) * (p->t - 4) * 2;
		ps->pitch[SOUL_SWORD_ARM] = 900; ps->pitch[SOUL_ARM] = 900;
		break;
	case P_DRINK:
		ps->pitch[SOUL_ARM] = lerp(120, 2300, p->t, 12);
		break;
	case P_STAGGER:
		ps->lean = -300 + ((game_frames & 4) ? 60 : 0);
		break;
	case P_DEAD:
		ps->lean = lerp(0, 1000, p->t, 30); ps->dy = lerp(0, 30, p->t, 30);
		break;
	}
	if (p->blocking) { ps->pitch[SOUL_SWORD_ARM] = 1100; ps->yaw[SOUL_SWORD_ARM] = -700; ps->lean = -60; }
}

static void husk_pose(const Enemy *e, Pose *ps) {
	*ps = (Pose){ { 260, 0, 0 }, { 0, 0, 0 }, 0, 0 };
	ps->dy = isin((game_frames * 24 + (int)(e - enemies) * 900) & 4095) >> 9;
	switch (e->state) {
	case E_MOVE:
		ps->pitch[HUSK_BODY] = 300 + (isin(e->t * 4096 / 36) >> 6);
		ps->pitch[HUSK_RIGHT_ARM] = isin(e->t * 4096 / 36) >> 4;
		ps->pitch[HUSK_LEFT_ARM] = -(isin(e->t * 4096 / 36) >> 4);
		ps->dy = -(isin(e->t * 2048 / 36) >> 8);
		break;
	case E_WINDUP: {
		/* Arms rise over the head, it straightens and leans back. */
		int t = e->t, w = e->windup;
		int arm = lerp(0, -2500, t, w - 8);
		ps->pitch[HUSK_RIGHT_ARM] = arm; ps->pitch[HUSK_LEFT_ARM] = arm;
		ps->pitch[HUSK_BODY] = lerp(260, -150, t, w - 8);
		if (t > w - 15) ps->dy = (game_frames & 2) ? -2 : 2;
		if (e->attack == 2) { ps->yaw[HUSK_RIGHT_ARM] = 600; ps->yaw[HUSK_LEFT_ARM] = -600; }
		break;
	}
	case E_RECOVER: {
		int t = e->t;
		int arm = lerp(700, 0, t - 6, 30);
		ps->pitch[HUSK_RIGHT_ARM] = arm; ps->pitch[HUSK_LEFT_ARM] = arm;
		ps->pitch[HUSK_BODY] = lerp(600, 260, t - 6, 30);
		ps->dy = t < 6 ? 12 : 0;
		break;
	}
	case E_STAGGER:
		ps->pitch[HUSK_BODY] = -350;
		ps->pitch[HUSK_RIGHT_ARM] = -400 + ((game_frames & 8) ? 100 : 0);
		ps->pitch[HUSK_LEFT_ARM] = -500;
		break;
	case E_DYING:
		ps->dy = e->t * 6;
		ps->pitch[HUSK_BODY] = 260 + e->t * 20;
		break;
	}
}

/* ---- Effects from game events --------------------------------------------------------- */

static int enemy_yaw[MAX_ENEMIES];

static void handle_events(void) {
	static const int8_t DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };
	for (int i = 0; i < num_events; i++) {
		const GameEvent *e = &game_events[i];
		int s = 0, c = 4096;   /* kicks go away from the camera */
		switch (e->type) {
		case EV_HIT:
			burst(e->x, e->y, 10, 170, 160, 150, 10, 150);
			kick_x += s * 14 >> 12; kick_z += c * 14 >> 12;
			break;
		case EV_RIPOSTE:
			burst(e->x, e->y, 18, 220, 200, 170, 14, 150);
			kick_x += s * 30 >> 12; kick_z += c * 30 >> 12;
			zoom += 90;
			break;
		case EV_KILL:
			burst(e->x, e->y, 24, 120, 116, 112, 8, 120);
			break;
		case EV_PARRY:
			burst(player.x + DX[player.facing], player.y + DY[player.facing], 16, 255, 230, 140, 16, 140);
			zoom += 70;
			break;
		case EV_HURT:
			burst(e->x, e->y, 12, 160, 30, 30, 9, 120);
			break;
		case EV_BLOCK:
			burst(e->x, e->y, 6, 230, 220, 190, 10, 130);
			break;
		case EV_SLAM:
			burst(e->x, e->y, 14, 110, 106, 100, 12, 10);
			break;
		case EV_DEATH:
			burst(e->x, e->y, 30, 140, 130, 130, 6, 90);
			break;
		}
	}
}

/* ---- The frame ------------------------------------------------------------------------- */

uint8_t *draw_frame(uint32_t *ot_, uint8_t *prim_, uint8_t *end, int frame) {
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

	update_camera(frame);
	handle_events();
	update_particles(frame);

	draw_sky();
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
			if (frame & 1) spawn(x + prand(40) - 20, -60, z + prand(40) - 20, 0, -2, 0, 20, 255, 190, 80, 8, 0);
		}
	}

	/* The husks. */
	for (int i = 0; i < MAX_ENEMIES; i++) {
		const Enemy *e = &enemies[i];
		if (e->state == E_NONE) { enemy_yaw[i] = -1; continue; }
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
		husk_pose(e, &ps);
		int tint = e->hit_t < 5 ? 1 : e->state == E_STAGGER ? 3 : 0;
		int fade = e->state == E_DYING ? 256 - e->t * 8 : 256;
		draw_model(&husk_model, &ps, x, 0, z, enemy_yaw[i], light_at(x, z) + 30, tint, fade < 0 ? 0 : fade);
		shadow(x, z, 80);
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
		draw_model(&soul_model, &ps, soul_x, 0, soul_z, player.facing * 1024, dodge_glow ? 330 : 300, tint, 256);
		shadow(soul_x, soul_z, 56);
		/* The sword's arc as it strikes. */
		if (player.state == P_ATTACK && player.t >= 8 && player.t < 13) {
			static const int8_t DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };
			int ax, az;
			tile_world((player.x + DX[player.facing]) * 16, (player.y + DY[player.facing]) * 16, &ax, &az);
			Proj p;
			project((soul_x + ax) / 2, -110, (soul_z + az) / 2, &p);
			if (p.z > NEAR_Z) glyph(slot(p.z) - 2, G_SLASH, p.x, p.y, 200 * SCREEN_H / p.z, 255, 255, 255, 1);
		}
	}

	draw_particles();
	draw_hud(frame);
	return prim;
}
