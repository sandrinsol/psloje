/*
 * The big map (see bigmap.h). Everything goes in the ordering table's
 * front slot, so it's drawn in the reverse of the order it's added: the
 * labels and markers first, the land last.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include "bigmap.h"
#include "route.h"

#define W 320
#define H 240
#define MIN_SHIFT 1      /* 2 m a pixel */
#define MAX_SHIFT 5      /* 32 m a pixel: most of the city */
#define PAN_PX    4      /* per frame with the D-pad held */
#define MAX_EDGES 16384

volatile int32_t bigmap_open;
static int shift = 3;            /* metres per pixel = 1 << shift */
static int32_t view_x, view_z;   /* metres, the centre of the screen */
static uint8_t drawn[MAX_EDGES];
static uint8_t stamp;
static int blink;

void bigmap_show(const Car *car) {
	view_x = CAR_TO_WORLD(car->x) / UNIT;
	view_z = CAR_TO_WORLD(car->z) / UNIT;
	bigmap_open = 1;
}

void bigmap_hide(void) {
	bigmap_open = 0;
}

void bigmap_input(uint16_t held, uint16_t pressed) {
	if ((pressed & PAD_L1) && shift > MIN_SHIFT) shift--;
	if ((pressed & PAD_R1) && shift < MAX_SHIFT) shift++;
	int step = PAN_PX << shift;
	if (held & PAD_LEFT) view_x -= step;
	if (held & PAD_RIGHT) view_x += step;
	if (held & PAD_UP) view_z += step;
	if (held & PAD_DOWN) view_z -= step;
	int32_t w, h;
	world_size(&w, &h);
	w /= UNIT;
	h /= UNIT;
	if (view_x < 0) view_x = 0;
	if (view_z < 0) view_z = 0;
	if (view_x > w) view_x = w;
	if (view_z > h) view_z = h;
}

static inline int sx(int32_t x) { return W / 2 + ((x - view_x) >> shift); }
static inline int sy(int32_t z) { return H / 2 - ((z - view_z) >> shift); }

static uint8_t *tile(uint32_t *ot, uint8_t *prim, int x, int y, int w, int h, int r, int g, int b) {
	TILE *t = (TILE *)prim;
	setTile(t);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, r, g, b);
	addPrim(ot, t);
	return prim + sizeof(TILE);
}

/* Liang-Barsky clip to the screen. */
static int clip(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
	if (*x0 >= 0 && *x0 < W && *y0 >= 0 && *y0 < H && *x1 >= 0 && *x1 < W && *y1 >= 0 && *y1 < H)
		return 1;
	int32_t dx = *x1 - *x0, dy = *y1 - *y0, t0 = 0, t1 = 4096;
	int32_t p[4] = { -dx, dx, -dy, dy };
	int32_t q[4] = { *x0, W - 1 - *x0, *y0, H - 1 - *y0 };
	for (int i = 0; i < 4; i++) {
		if (p[i] == 0) {
			if (q[i] < 0) return 0;
			continue;
		}
		int32_t t = q[i] * 4096 / p[i];
		if (p[i] < 0) { if (t > t1) return 0; if (t > t0) t0 = t; }
		else { if (t < t0) return 0; if (t < t1) t1 = t; }
	}
	int32_t ax = *x0 + (dx * t0 >> 12), ay = *y0 + (dy * t0 >> 12);
	*x1 = *x0 + (dx * t1 >> 12);
	*y1 = *y0 + (dy * t1 >> 12);
	*x0 = ax;
	*y0 = ay;
	return 1;
}

static uint8_t *line(uint32_t *ot, uint8_t *prim, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int r, int g, int b) {
	if (!clip(&x0, &y0, &x1, &y1))
		return prim;
	LINE_F2 *l = (LINE_F2 *)prim;
	setLineF2(l);
	setXY2(l, x0, y0, x1, y1);
	setRGB0(l, r, g, b);
	addPrim(ot, l);
	return prim + sizeof(LINE_F2);
}

/* The route, 3 pixels wide. */
static uint8_t *route(uint32_t *ot, uint8_t *prim, uint8_t *end, int r, int g, int b) {
	for (int i = 0; i + 1 < route_count && prim < end; i++) {
		int32_t x0 = sx(route_points[i].x), y0 = sy(route_points[i].z);
		int32_t x1 = sx(route_points[i + 1].x), y1 = sy(route_points[i + 1].z);
		for (int k = -1; k <= 1; k++) {
			if (x1 - x0 > y1 - y0 || x0 - x1 > y1 - y0) prim = line(ot, prim, x0, y0 + k, x1, y1 + k, r, g, b);   /* flat: widen up/down */
			else prim = line(ot, prim, x0 + k, y0, x1 + k, y1, r, g, b);
		}
	}
	return prim;
}

static const uint8_t road_rgb[3][3] = { { 150, 150, 150 }, { 215, 200, 140 }, { 245, 205, 90 } };

/* The roads, as lines in map pixels ((metres >> shift), z up), worked out
 * for the screen plus half a screen around it and only again when the
 * zoom changes or the view moves out of that: projecting every street
 * every frame is too slow zoomed out. */
#define CACHE_LINES 8192
static int16_t cache[CACHE_LINES][4];
static uint8_t cache_cls[CACHE_LINES];
static int cache_n, cache_shift = -1;
static int32_t cache_x, cache_z;   /* metres: the view it was made for */

static void build_cache(void) {
	if (!++stamp) {
		memset(drawn, 0, sizeof(drawn));
		stamp = 1;
	}
	cache_n = 0;
	cache_shift = shift;
	cache_x = view_x;
	cache_z = view_z;
	int32_t half_w = W << shift, half_h = H << shift;   /* a screen each way */
	int c0 = (view_x - half_w) / graph.cell_m, c1 = (view_x + half_w) / graph.cell_m;
	int r0 = (view_z - half_h) / graph.cell_m, r1 = (view_z + half_h) / graph.cell_m;
	if (c0 < 0) c0 = 0;
	if (r0 < 0) r0 = 0;
	if (c1 >= graph.grid_w) c1 = graph.grid_w - 1;
	if (r1 >= graph.grid_h) r1 = graph.grid_h - 1;
	for (int gz = r0; gz <= r1; gz++)
		for (int gx = c0; gx <= c1; gx++) {
			int c = gz * graph.grid_w + gx;
			for (uint32_t k = graph.cell_first[c]; k < graph.cell_first[c + 1]; k++) {
				int ei = graph.cell_edges[k];
				if (ei >= MAX_EDGES || drawn[ei] == stamp) continue;
				drawn[ei] = stamp;
				const RoadEdge *e = &graph.edges[ei];
				int cls = e->flags & ROAD_CLASS;
				if ((shift >= 5 && cls == 0) || (shift >= 4 && (e->flags & ROAD_SERVICE)))
					continue;   /* zoomed out: the bigger roads only */
				MapPoint p = edge_point(e, 0);
				int lx = p.x >> shift, ly = p.z >> shift;
				for (int i = 1; i <= e->npoints + 1; i++) {
					p = edge_point(e, i);
					int x = p.x >> shift, y = p.z >> shift;
					int d = (x > lx ? x - lx : lx - x) + (y > ly ? y - ly : ly - y);
					if (d < 2 && (i <= e->npoints || !d)) continue;
					if (cache_n >= CACHE_LINES) return;
					int16_t *l = cache[cache_n];
					l[0] = lx; l[1] = ly; l[2] = x; l[3] = y;
					cache_cls[cache_n++] = cls;
					lx = x;
					ly = y;
				}
			}
		}
}

static uint8_t *roads(uint32_t *ot, uint8_t *prim, uint8_t *end) {
	int32_t dx = view_x - cache_x, dz = view_z - cache_z;
	if (shift != cache_shift || dx > (W / 2 << shift) || -dx > (W / 2 << shift) || dz > (H / 2 << shift) || -dz > (H / 2 << shift))
		build_cache();
	/* Screen = map pixels - origin (y flipped). */
	int32_t ox = (view_x >> shift) - W / 2, oy = (view_z >> shift) + H / 2;
	for (int i = 0; i < cache_n && prim < end; i++) {
		const int16_t *l = cache[i];
		int32_t x0 = l[0] - ox, x1 = l[2] - ox, y0 = oy - l[1], y1 = oy - l[3];
		if ((x0 < 0 && x1 < 0) || (x0 >= W && x1 >= W) || (y0 < 0 && y1 < 0) || (y0 >= H && y1 >= H))
			continue;
		const uint8_t *rgb = road_rgb[cache_cls[i]];
		prim = line(ot, prim, x0, y0, x1, y1, rgb[0], rgb[1], rgb[2]);
		if (cache_cls[i] == 2 && shift <= 3) prim = line(ot, prim, x0 + 1, y0, x1 + 1, y1, rgb[0], rgb[1], rgb[2]);
	}
	return prim;
}

/* The sea: the bitmap turned into runs once (row by row), then a tile per
 * run on screen. */
#define MAX_RUNS 8192
#define MAX_SEA_ROWS 1024
static uint16_t run_first[MAX_SEA_ROWS + 1];
static uint16_t runs[MAX_RUNS][2];   /* first cell, end cell */
static int runs_made;

static void make_runs(void) {
	int n = 0;
	for (int j = 0; j < graph.sea_h && j < MAX_SEA_ROWS; j++) {
		const uint8_t *row = graph.sea + j * graph.sea_row_bytes;
		run_first[j] = n;
		for (int i = 0; i < graph.sea_w && n < MAX_RUNS; i++) {
			if (!(row[i >> 3] & (1 << (i & 7)))) continue;
			int start = i;
			while (i + 1 < graph.sea_w && (row[(i + 1) >> 3] & (1 << ((i + 1) & 7)))) i++;
			runs[n][0] = start;
			runs[n][1] = i + 1;
			n++;
		}
	}
	run_first[graph.sea_h < MAX_SEA_ROWS ? graph.sea_h : MAX_SEA_ROWS] = n;
	runs_made = 1;
}

static uint8_t *sea(uint32_t *ot, uint8_t *prim, uint8_t *end) {
	if (!runs_made)
		make_runs();
	int cell = graph.sea_cell;
	int32_t half_h = (H / 2) << shift;
	int j0 = (view_z - half_h) / cell, j1 = (view_z + half_h) / cell + 1;
	int rows = graph.sea_h < MAX_SEA_ROWS ? graph.sea_h : MAX_SEA_ROWS;
	if (j0 < 0) j0 = 0;
	if (j1 > rows) j1 = rows;
	for (int j = j0; j < j1 && prim < end; j++) {
		int y0 = sy((j + 1) * cell), y1 = sy(j * cell);
		if (y0 < 0) y0 = 0;
		if (y1 > H) y1 = H;
		if (y1 <= y0) continue;   /* zoomed out: rows thinner than a pixel */
		for (int k = run_first[j]; k < run_first[j + 1]; k++) {
			int x0 = sx(runs[k][0] * cell), x1 = sx(runs[k][1] * cell);
			if (x0 < 0) x0 = 0;
			if (x1 > W) x1 = W;
			if (x1 > x0) prim = tile(ot, prim, x0, y0, x1 - x0, y1 - y0, 34, 62, 104);
		}
	}
	return prim;
}

static uint8_t *triangle(uint32_t *ot, uint8_t *prim, int x0, int y0, int x1, int y1, int x2, int y2, int r, int g, int b) {
	POLY_F3 *t = (POLY_F3 *)prim;
	setPolyF3(t);
	setXY3(t, x0, y0, x1, y1, x2, y2);
	setRGB0(t, r, g, b);
	addPrim(ot, t);
	return prim + sizeof(POLY_F3);
}

uint8_t *bigmap_draw(const Car *car, const Place *target, int pickup, uint32_t *ot, uint8_t *prim, uint8_t *end) {
	char text[48];
	int r = pickup ? 60 : 255, g = pickup ? 230 : 120, b = pickup ? 90 : 20;
	blink++;

	/* Labels (on top), with a dark band behind them. */
	sprintf(text, "VLORA   %d M A PIXEL", 1 << shift);
	prim = FntSort(ot, prim, 8, 6, text);
	prim = FntSort(ot, prim, 8, 226, "L1/R1 ZOOM  D-PAD MOVE  TRIANGLE CLOSE");
	if (target) {
		sprintf(text, "%s %s", pickup ? "PASSENGER AT" : "TO", world_name(target->name));
		prim = FntSort(ot, prim, 8, 18, text);
		if (route_count) {
			sprintf(text, "%d.%d KM", route_metres / 1000, route_metres / 100 % 10);
			prim = FntSort(ot, prim, 264, 6, text);
		}
	}

	/* The taxi: an arrow with a dark edge. */
	int32_t cx = sx(CAR_TO_WORLD(car->x) / UNIT), cy = sy(CAR_TO_WORLD(car->z) / UNIT);
	int h = car->heading >> 8, s = isin(h), c = icos(h);
	for (int k = 0; k < 2; k++) {
		int sz = k ? 8 : 6;   /* the dark one is added second: drawn below */
		prim = triangle(ot, prim, cx + (s * sz >> 12), cy - (c * sz >> 12),
		                cx + ((-s * sz - c * sz) * 3 / 4 >> 12), cy + ((c * sz - s * sz) * 3 / 4 >> 12),
		                cx + ((-s * sz + c * sz) * 3 / 4 >> 12), cy + ((c * sz + s * sz) * 3 / 4 >> 12),
		                k ? 0 : 255, k ? 0 : 230, k ? 0 : 60);
	}
	/* The target, blinking, clamped to the screen edge. */
	if (target) {
		int32_t tx = sx(target->x / UNIT), ty = sy(target->z / UNIT);
		if (tx < 4) tx = 4;
		if (tx > W - 5) tx = W - 5;
		if (ty < 30) ty = 30;
		if (ty > H - 20) ty = H - 20;
		if (blink & 16) prim = tile(ot, prim, tx - 3, ty - 3, 7, 7, r, g, b);
		prim = tile(ot, prim, tx - 4, ty - 4, 9, 9, 255, 255, 255);
	}
	prim = tile(ot, prim, 0, 0, W, 30, 12, 14, 20);
	prim = tile(ot, prim, 0, 222, W, 18, 12, 14, 20);
	if (target)
		prim = route(ot, prim, end, r, g, b);
	prim = roads(ot, prim, end);
	prim = sea(ot, prim, end - 2048);

	/* The land inside the map, dark grey outside it. */
	int32_t w, hgt;
	world_size(&w, &hgt);
	int32_t x0 = sx(0), x1 = sx(w / UNIT), y0 = sy(hgt / UNIT), y1 = sy(0);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > W) x1 = W;
	if (y1 > H) y1 = H;
	if (x1 > x0 && y1 > y0)
		prim = tile(ot, prim, x0, y0, x1 - x0, y1 - y0, 58, 66, 60);
	prim = tile(ot, prim, 0, 0, W, H, 24, 26, 32);
	/* No texture window for the font (drawn first: added last). */
	DR_TWIN *tw = (DR_TWIN *)prim;
	RECT none = { 0, 0, 0, 0 };
	setTexWindow(tw, &none);
	addPrim(ot, tw);
	return prim + sizeof(DR_TWIN);
}
