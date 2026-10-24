/*
 * The figures, built from boxes, prisms and cones at start-up (see models.h).
 *
 * Faces are listed as seen from outside: quads top-left, top-right,
 * bottom-left, bottom-right; triangles clockwise.
 */
#include <stdint.h>
#include <psxgte.h>
#include "models.h"

Model soul_model, husk_model, head_statue, hand_statue, pillar_statue;

static Vec3 vpool[1200];
static MFace fpool[700];
static int nv, nf;
static Part *part;
static int scale = 4096;   /* this model's size, 4096 = as written */

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

static void begin(Model *m, int px, int py, int pz) {
	px = px * scale >> 12; py = py * scale >> 12; pz = pz * scale >> 12;
	part = &m->parts[m->nparts++];
	part->verts = &vpool[nv];
	part->faces = &fpool[nf];
	part->nverts = part->nfaces = 0;
	part->px = px; part->py = py; part->pz = pz;
}

static int vert(int x, int y, int z) {
	Vec3 *v = &vpool[nv++];
	v->x = x * scale >> 12; v->y = y * scale >> 12; v->z = z * scale >> 12;
	return part->nverts++;
}

static void face(int a, int b, int c, int d, int r, int g, int bl, int glow) {
	MFace *f = &fpool[nf++];
	f->v[0] = a; f->v[1] = b; f->v[2] = c; f->v[3] = d < 0 ? 255 : d;
	f->r = r; f->g = g; f->b = bl; f->glow = glow;
	/* Outward normal: (BL - TL) x (TR - TL), seen from outside. */
	const Vec3 *p = part->verts;
	int ux = p[b].x - p[a].x, uy = p[b].y - p[a].y, uz = p[b].z - p[a].z;
	int vx = p[c].x - p[a].x, vy = p[c].y - p[a].y, vz = p[c].z - p[a].z;
	int nx = vy * uz - vz * uy, ny = vz * ux - vx * uz, nz = vx * uy - vy * ux;
	while (nx > 30000 || nx < -30000 || ny > 30000 || ny < -30000 || nz > 30000 || nz < -30000) { nx >>= 1; ny >>= 1; nz >>= 1; }
	int len = isqrt(nx * nx + ny * ny + nz * nz);
	if (!len) len = 1;
	f->nx = nx * 4096 / len; f->ny = ny * 4096 / len; f->nz = nz * 4096 / len;
	part->nfaces++;
}

#define RGB(c) (c)[0], (c)[1], (c)[2]

/* A box: x0 < x1, y0 < y1 (y0 is the top), z0 < z1 (z1 the front). */
static void box(int x0, int x1, int y0, int y1, int z0, int z1, const uint8_t *c) {
	int a = vert(x0, y0, z0), b = vert(x1, y0, z0), cc = vert(x1, y0, z1), d = vert(x0, y0, z1);
	int e = vert(x0, y1, z0), f = vert(x1, y1, z0), g = vert(x1, y1, z1), h = vert(x0, y1, z1);
	face(cc, d, g, h, RGB(c), 0);   /* front (+z): seen from the front, +x is on the left */
	face(a, b, e, f, RGB(c), 0);    /* back */
	face(b, cc, f, g, RGB(c), 0);   /* right (+x) */
	face(d, a, h, e, RGB(c), 0);    /* left */
	face(d, cc, a, b, RGB(c), 0);   /* top */
	face(e, f, h, g, RGB(c), 0);    /* bottom */
}

/* An n-sided prism around (cx, cz), radius r0 at the bottom (y0) and r1 at
 * the top (y1 < y0); r1 = 0 makes a cone. */
static void prism(int n, int cx, int cz, int r0, int r1, int y0, int y1, const uint8_t *c, int top, int bottom) {
	int lo[12], hi[12];
	for (int i = 0; i < n; i++) {
		int a = i * 4096 / n, s = isin(a), co = icos(a);
		lo[i] = vert(cx + (r0 * s >> 12), y0, cz + (r0 * co >> 12));
		hi[i] = r1 ? vert(cx + (r1 * s >> 12), y1, cz + (r1 * co >> 12)) : -1;
	}
	int apex = r1 ? -1 : vert(cx, y1, cz);
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		if (r1) face(hi[j], hi[i], lo[j], lo[i], RGB(c), 0);
		else face(apex, lo[i], lo[j], -1, RGB(c), 0);
	}
	if (top && r1) {
		int m = vert(cx, y1, cz);
		for (int i = 0; i < n; i++) face(m, hi[i], hi[(i + 1) % n], -1, RGB(c), 0);
	}
	if (bottom) {
		int m = vert(cx, y0, cz);
		for (int i = 0; i < n; i++) face(m, lo[(i + 1) % n], lo[i], -1, RGB(c), 0);
	}
}

/* A flat quad facing +z (for eyes and the face's darkness). */
static void plate(int x0, int x1, int y0, int y1, int z, const uint8_t *c, int glow) {
	int a = vert(x1, y0, z), b = vert(x0, y0, z), cc = vert(x1, y1, z), d = vert(x0, y1, z);
	face(a, b, cc, d, RGB(c), glow);
}

void models_init(void) {
	/* The soul: a hooded figure in a ragged grey robe, faint light in its
	 * face, a sword in the right hand. About 180 units tall. */
	static const uint8_t robe[] = { 150, 144, 140 }, robe2[] = { 172, 164, 156 }, hood[] = { 128, 122, 122 };
	static const uint8_t dark[] = { 8, 6, 10 }, eyes[] = { 170, 200, 255 }, skin[] = { 120, 110, 104 };
	static const uint8_t blade[] = { 200, 206, 214 }, hilt[] = { 130, 96, 52 };
	Model *m = &soul_model;
	scale = 7373;   /* x 1.8: big enough to read from the camera */
	begin(m, 0, 0, 0);
	prism(6, 0, 0, 52, 28, 0, -112, robe, 0, 1);
	prism(6, 0, 0, 28, 20, -112, -140, robe2, 1, 0);
	prism(5, 0, 2, 24, 7, -136, -182, hood, 0, 0);
	plate(-11, 11, -166, -144, 20, dark, 0);
	plate(-8, -3, -160, -156, 21, eyes, 1);
	plate(3, 8, -160, -156, 21, eyes, 1);
	begin(m, 26, -132, 0);   /* sword arm, from the right shoulder */
	box(20, 32, -134, -84, -7, 7, robe2);
	box(21, 31, -86, -76, -6, 6, skin);
	box(18, 34, -78, -72, -8, 8, hilt);
	box(23, 29, -72, 30, -3, 3, blade);
	begin(m, -26, -132, 0);  /* left arm */
	box(-32, -20, -134, -84, -7, 7, robe2);
	box(-31, -21, -86, -76, -6, 6, skin);

	/* Sloth's husk: a hunched grey giant, arms hanging to the floor, a small
	 * head sunk in front of its shoulders. About 230 units. */
	static const uint8_t ash[] = { 128, 122, 116 }, ash2[] = { 104, 98, 94 }, ash3[] = { 150, 144, 136 };
	static const uint8_t ember[] = { 230, 170, 80 };
	m = &husk_model;
	scale = 6554;   /* x 1.6 */
	begin(m, 0, -90, 0);
	box(-46, -14, -92, 0, -22, 20, ash2);
	box(14, 46, -92, 0, -22, 20, ash2);
	box(-62, 62, -206, -88, -52, 48, ash);
	box(-24, 24, -218, -176, 34, 80, ash3);
	plate(-16, -6, -202, -196, 81, ember, 1);
	plate(6, 16, -202, -196, 81, ember, 1);
	begin(m, 66, -196, 0);   /* right arm */
	box(60, 90, -200, -40, -18, 18, ash);
	box(56, 96, -44, -6, -26, 26, ash2);
	begin(m, -66, -196, 0);  /* left arm */
	box(-90, -60, -200, -40, -18, 18, ash);
	box(-96, -56, -44, -6, -26, 26, ash2);

	/* Sunken statues in the ash around the floor: a giant head, a raised
	 * hand, a broken column (units: about 4x the figures). */
	static const uint8_t stone[] = { 112, 108, 104 }, stone2[] = { 88, 84, 82 }, hollow[] = { 24, 22, 24 };
	m = &head_statue;
	scale = 4096;
	begin(m, 0, 0, 0);
	box(-260, 260, -700, 0, -240, 240, stone);
	box(-280, 280, -560, -500, 200, 280, stone2);   /* brow */
	prism(4, 0, 290, 70, 0, -360, -500, stone2, 0, 0);
	plate(-200, -70, -480, -400, 241, hollow, 0);
	plate(70, 200, -480, -400, 241, hollow, 0);
	plate(-120, 120, -260, -220, 241, hollow, 0);
	m = &hand_statue;
	begin(m, 0, 0, 0);
	box(-60, 60, -300, 400, -50, 50, stone2);       /* wrist and forearm, rising */
	box(-110, 110, -560, -300, -60, 60, stone);     /* palm */
	for (int i = 0; i < 4; i++) box(-104 + i * 54, -62 + i * 54, -800 + (i == 0 || i == 3) * 80, -560, -40, 40, stone);
	box(110, 170, -500, -360, -40, 40, stone);      /* thumb */
	m = &pillar_statue;
	begin(m, 0, 0, 0);
	prism(8, 0, 0, 120, 110, 400, -900, stone, 1, 0);
	prism(8, 0, 0, 150, 150, -900, -980, stone2, 1, 1);
}
