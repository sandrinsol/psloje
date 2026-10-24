/*
 * The taxi job (see game.h).
 *
 * Fares: a passenger waits at a place from the map (shown by a green
 * column); stop next to them to let them in. Drive to the destination
 * (orange column, arrow at the top of the screen, dot on the minimap) and
 * stop there. The meter runs like a Vlora taxi: a starting fare plus a price
 * per kilometre, in Lek. Arriving quickly without hitting anything earns a
 * tip; every wall hit during the ride takes some of it away.
 *
 * The clock runs 1 game minute per real second, from 07:00; the light and
 * the fog colour follow the time of day.
 */
#include <stdint.h>
#include <stdio.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include "game.h"

#define START_FARE      300      /* Lek */
#define PER_KM          120      /* Lek per km */
#define STOP_DIST       (9 * UNIT)
#define STOP_KMH        12
#define BOARD_TICKS     45       /* passenger getting in / out */
#define PAID_TICKS      240

volatile int32_t game_minutes = 7 * 60;
volatile int32_t money = 1000;
volatile int32_t fare_state, meter, fares_done, last_fare, last_tip;
volatile int32_t fare_from = -1, fare_to = -1;

static int minute_ticks, state_ticks, board_ticks;
static int32_t ride_units;          /* distance driven with the passenger, 1/16 m */
static int32_t ride_ticks, ride_bumps, ride_expect;
static int32_t last_x, last_z;
static uint32_t rng = 0x5eed1234;

static uint32_t rnd(void) {
	rng = rng * 1103515245 + 12345;
	return rng >> 8;
}

static int32_t dist_units(int32_t ax, int32_t az, int32_t bx, int32_t bz) {
	int32_t dx = (ax - bx) >> 4, dz = (az - bz) >> 4;   /* metres */
	int32_t d2 = dx * dx + dz * dz, r = 0, bit = 1 << 30;
	while (bit > d2) bit >>= 2;
	while (bit) {
		if (d2 >= r + bit) { d2 -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return r * UNIT;
}

/* ---- Day and night ------------------------------------------------------------- */

typedef struct {
	int16_t minute;
	uint8_t light[3], fog[3];
} Key;

static const Key keys[] = {
	{ 0,    { 52, 58, 92 },    { 14, 18, 34 } },
	{ 300,  { 52, 58, 92 },    { 14, 18, 34 } },
	{ 360,  { 112, 100, 100 }, { 150, 122, 112 } },
	{ 450,  { 128, 128, 128 }, { 172, 190, 204 } },
	{ 1080, { 128, 128, 128 }, { 172, 190, 204 } },
	{ 1170, { 128, 104, 92 },  { 204, 142, 108 } },
	{ 1230, { 52, 58, 92 },    { 14, 18, 34 } },
	{ 1440, { 52, 58, 92 },    { 14, 18, 34 } },
};

static void update_light(void) {
	int m = game_minutes % 1440, i = 0;
	while (keys[i + 1].minute <= m) i++;
	const Key *a = &keys[i], *b = &keys[i + 1];
	int t = (m - a->minute) * 256 / (b->minute - a->minute);
	light.r = a->light[0] + ((b->light[0] - a->light[0]) * t >> 8);
	light.g = a->light[1] + ((b->light[1] - a->light[1]) * t >> 8);
	light.b = a->light[2] + ((b->light[2] - a->light[2]) * t >> 8);
	fog_colour.r = a->fog[0] + ((b->fog[0] - a->fog[0]) * t >> 8);
	fog_colour.g = a->fog[1] + ((b->fog[1] - a->fog[1]) * t >> 8);
	fog_colour.b = a->fog[2] + ((b->fog[2] - a->fog[2]) * t >> 8);
}

static int is_dark(void) {
	return light.r < 100;
}

/* ---- Fares ------------------------------------------------------------------------ */

/* A random destination-type place between min and max units from (x, z), or -1. */
static int pick_place(int32_t x, int32_t z, int32_t min, int32_t max, int not) {
	int n = 0, pick = -1;
	for (int i = 0; i < num_places; i++) {
		if (i == not || places[i].kind != PLACE_DESTINATION) continue;
		int32_t d = dist_units(x, z, places[i].x, places[i].z);
		if (d < min || d > max) continue;
		/* Reservoir sampling: each candidate equally likely. */
		if (rnd() % ++n == 0) pick = i;
	}
	return pick;
}

static void new_fare(const Car *car) {
	int32_t x = CAR_TO_WORLD(car->x), z = CAR_TO_WORLD(car->z);
	int from = pick_place(x, z, 60 * UNIT, 600 * UNIT, -1);
	if (from < 0) from = pick_place(x, z, 30 * UNIT, 1 << 30, -1);
	if (from < 0) return;   /* no places on this map */
	int to = pick_place(places[from].x, places[from].z, 200 * UNIT, 1500 * UNIT, from);
	if (to < 0) to = pick_place(places[from].x, places[from].z, 50 * UNIT, 1 << 30, from);
	if (to < 0) return;
	fare_from = from;
	fare_to = to;
	fare_state = FARE_PICKUP;
	board_ticks = 0;
}

static int stopped_at(const Car *car, const Place *p) {
	return car_kmh(car) <= STOP_KMH &&
	       dist_units(CAR_TO_WORLD(car->x), CAR_TO_WORLD(car->z), p->x, p->z) < STOP_DIST;
}

void game_init(void) {
	fare_state = FARE_NONE;
	state_ticks = 60;
	update_light();
}

void game_update(const Car *car, int ticks) {
	int32_t x = CAR_TO_WORLD(car->x), z = CAR_TO_WORLD(car->z);
	for (int t = 0; t < ticks; t++) {
		if (++minute_ticks >= 60) {
			minute_ticks = 0;
			game_minutes = (game_minutes + 1) % 1440;
		}
		switch (fare_state) {
		case FARE_NONE:
			if (--state_ticks <= 0) {
				new_fare(car);
				state_ticks = 60;   /* try again in a second if there was none */
			}
			break;
		case FARE_PICKUP:
			if (stopped_at(car, &places[fare_from])) {
				if (++board_ticks >= BOARD_TICKS) {
					fare_state = FARE_RIDE;
					meter = START_FARE;
					ride_units = ride_ticks = 0;
					ride_bumps = car->bumps;
					/* Expected time: straight-line distance at ~25 km/h, plus 20 s. */
					ride_expect = dist_units(places[fare_from].x, places[fare_from].z, places[fare_to].x, places[fare_to].z) / UNIT * 60 * 36 / 250 + 1200;
					board_ticks = 0;
				}
			} else {
				board_ticks = 0;
			}
			break;
		case FARE_RIDE:
			ride_ticks++;
			if (stopped_at(car, &places[fare_to])) {
				if (++board_ticks >= BOARD_TICKS) {
					int bumps = car->bumps - ride_bumps;
					int tip = 0;
					if (ride_ticks < ride_expect)
						tip = meter * 20 / 100 * (ride_expect - ride_ticks) / ride_expect + meter / 10;
					tip -= bumps * meter / 20;
					if (tip < 0) tip = 0;
					tip = tip / 10 * 10;
					last_fare = meter;
					last_tip = tip;
					money += meter + tip;
					fares_done++;
					fare_state = FARE_PAID;
					state_ticks = PAID_TICKS;
				}
			} else {
				board_ticks = 0;
			}
			break;
		case FARE_PAID:
			if (--state_ticks <= 0) {
				fare_state = FARE_NONE;
				state_ticks = 90;
			}
			break;
		}
	}
	if (fare_state == FARE_RIDE) {
		int32_t dx = x - last_x, dz = z - last_z;
		if (dx < 0) dx = -dx;
		if (dz < 0) dz = -dz;
		/* Octagonal distance approximation, good to a few percent. */
		ride_units += dx > dz ? dx + dz * 3 / 8 : dz + dx * 3 / 8;
		meter = START_FARE + ride_units / UNIT * PER_KM / 1000 / 10 * 10;
	}
	last_x = x;
	last_z = z;
	update_light();
}

/* ---- World markers ------------------------------------------------------------- */

/* World point (units) to screen; returns camera depth (scaled), <= 0 if behind. */
static int32_t project(const Camera *cam, int32_t x, int32_t y, int32_t z, DVECTOR *out) {
	int32_t dx = x - cam->x, dy = y - cam->y, dz = z - cam->z;
	int32_t cx = (cam->m.m[0][0] * dx + cam->m.m[0][1] * dy + cam->m.m[0][2] * dz) >> 12;
	int32_t cy = (cam->m.m[1][0] * dx + cam->m.m[1][1] * dy + cam->m.m[1][2] * dz) >> 12;
	int32_t cz = (cam->m.m[2][0] * dx + cam->m.m[2][1] * dy + cam->m.m[2][2] * dz) >> 12;
	if (cz < UNIT)
		return 0;
	int32_t sx = 160 + cx * 200 / cz, sy = 120 + cy * 200 / cz;
	if (sx < -1000) sx = -1000;
	if (sx > 1000) sx = 1000;
	if (sy < -1000) sy = -1000;
	if (sy > 1000) sy = 1000;
	out->vx = sx;
	out->vy = sy;
	return cz;
}

static uint8_t *tpage_abr(uint32_t *ot, uint8_t *prim, int abr) {
	DR_TPAGE *tp = (DR_TPAGE *)prim;
	setDrawTPage(tp, 0, 1, getTPage(0, abr, 0, 0));
	addPrim(ot, tp);
	return prim + sizeof(DR_TPAGE);
}

static inline uint32_t *depth_slot(uint32_t *ot, int32_t cz) {
	int otz = cz * GTE_SCALE >> 4;
	if (otz < 2) otz = 2;
	if (otz > OT_LEN - 8) otz = OT_LEN - 8;
	return ot + otz;
}

/* A see-through column 1.6 m wide and 12 m tall over a place. */
static uint8_t *beacon(const Camera *cam, const Place *p, int r, int g, int b, uint32_t *ot, uint8_t *prim) {
	DVECTOR lo, hi;
	int32_t gy = world_ground_y(p->x, p->z);
	int32_t cz = project(cam, p->x, gy, p->z, &lo);
	if (!cz || !project(cam, p->x, gy - 12 * UNIT, p->z, &hi))
		return prim;
	int w = 13 * 200 / cz + 1;   /* 0.8 m each side */
	if (lo.vx + w < 0 || lo.vx - w > 320 || lo.vy < 0 || hi.vy > 240)
		return prim;   /* off screen */
	uint32_t *slot = depth_slot(ot, cz);
	POLY_G4 *q = (POLY_G4 *)prim;
	setPolyG4(q);
	setSemiTrans(q, 1);
	setXY4(q, hi.vx - w, hi.vy, hi.vx + w, hi.vy, lo.vx - w, lo.vy, lo.vx + w, lo.vy);
	setRGB0(q, 0, 0, 0);
	setRGB1(q, 0, 0, 0);
	setRGB2(q, r, g, b);
	setRGB3(q, r, g, b);
	addPrim(slot, q);
	prim += sizeof(POLY_G4);
	return tpage_abr(slot, prim, 1);   /* additive: glows, day and night */
}

/* The waiting passenger: a person-sized figure next to the kerb. */
static uint8_t *passenger(const Camera *cam, const Place *p, uint32_t *ot, uint8_t *prim) {
	DVECTOR feet, head;
	int32_t gy = world_ground_y(p->x, p->z);
	int32_t cz = project(cam, p->x, gy, p->z, &feet);
	if (!cz || !project(cam, p->x, gy - 28, p->z, &head))   /* 1.75 m */
		return prim;
	int w = 4 * 200 / cz + 1;   /* 0.25 m each side */
	if (feet.vx + w < 0 || feet.vx - w > 320 || feet.vy < 0 || head.vy > 240)
		return prim;   /* off screen */
	int neck = head.vy + (feet.vy - head.vy) / 6;
	uint32_t *slot = depth_slot(ot, cz);
	static const uint8_t parts[3][3] = { { 60, 60, 70 }, { 200, 60, 60 }, { 220, 180, 150 } };
	int y[4] = { feet.vy, feet.vy - (feet.vy - head.vy) * 4 / 9, neck, head.vy };
	for (int i = 0; i < 3; i++) {
		TILE *t = (TILE *)prim;
		int pw = i == 2 ? w * 2 / 3 + 1 : w;
		setTile(t);
		setXY0(t, feet.vx - pw, y[i + 1]);
		setWH(t, pw * 2, y[i] - y[i + 1] + 1);
		setRGB0(t, parts[i][0] * light.r >> 7, parts[i][1] * light.g >> 7, parts[i][2] * light.b >> 7);
		addPrim(slot, t);
		prim += sizeof(TILE);
	}
	return prim;
}

/* Headlight glow on the road ahead at night (additive, drawn over the ground). */
static uint8_t *headlights(const Car *car, const Camera *cam, uint32_t *ot, uint8_t *prim) {
	int h = car->heading >> 8;
	int32_t fx = isin(h), fz = icos(h), rx = fz, rz = -fx;
	int32_t x = CAR_TO_WORLD(car->x), z = CAR_TO_WORLD(car->z);
	static const int16_t shape[4][2] = { { -100, 720 }, { 100, 720 }, { -18, 40 }, { 18, 40 } };  /* 1/16 m: side, ahead (45 m) */
	DVECTOR p[4];
	int32_t near = 1 << 30;
	for (int i = 0; i < 4; i++) {
		int32_t px = x + ((fx * shape[i][1] + rx * shape[i][0]) >> 12);
		int32_t pz = z + ((fz * shape[i][1] + rz * shape[i][0]) >> 12);
		int32_t cz = project(cam, px, world_ground_y(px, pz) - 1, pz, &p[i]);
		if (!cz)
			return prim;
		if (cz < near) near = cz;
	}
	/* Just in front of the ground and road under its near end. */
	uint32_t *slot = depth_slot(ot, near) - 1;
	POLY_G4 *q = (POLY_G4 *)prim;
	setPolyG4(q);
	setSemiTrans(q, 1);
	setXY4(q, p[0].vx, p[0].vy, p[1].vx, p[1].vy, p[2].vx, p[2].vy, p[3].vx, p[3].vy);
	setRGB0(q, 10, 9, 6);
	setRGB1(q, 10, 9, 6);
	setRGB2(q, 110, 102, 72);
	setRGB3(q, 110, 102, 72);
	addPrim(slot, q);
	prim += sizeof(POLY_G4);
	return tpage_abr(slot, prim, 1);
}

uint8_t *game_draw_world(const Car *car, const Camera *cam, uint32_t *ot, uint8_t *prim) {
	if (fare_state == FARE_PICKUP) {
		prim = beacon(cam, &places[fare_from], 40, 160, 60, ot, prim);
		prim = passenger(cam, &places[fare_from], ot, prim);
	} else if (fare_state == FARE_RIDE) {
		prim = beacon(cam, &places[fare_to], 170, 100, 20, ot, prim);
	}
	if (is_dark())
		prim = headlights(car, cam, ot, prim);
	return prim;
}

/* ---- HUD ----------------------------------------------------------------------------- */

#define MAP_X     244
#define MAP_Y     138
#define MAP_SIZE  72
#define MAP_SCALE 3     /* metres per pixel */

typedef struct {
	uint32_t *ot;
	uint8_t *prim;
	int32_t cx, cz;     /* car, metres */
	int32_t s, c;       /* camera heading sin/cos */
} MapArg;

/* Metres -> minimap pixels (heading up). */
static void map_point(const MapArg *a, int32_t x, int32_t z, int32_t *px, int32_t *py) {
	int32_t dx = x - a->cx, dz = z - a->cz;
	int32_t right = (dx * a->c - dz * a->s) >> 12, fwd = (dx * a->s + dz * a->c) >> 12;
	*px = MAP_X + MAP_SIZE / 2 + right / MAP_SCALE;
	*py = MAP_Y + MAP_SIZE / 2 - fwd / MAP_SCALE;
}

/* Liang-Barsky clip of a line to the minimap box. */
static int clip(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
	int32_t dx = *x1 - *x0, dy = *y1 - *y0, t0 = 0, t1 = 4096;
	int32_t p[4] = { -dx, dx, -dy, dy };
	int32_t q[4] = { *x0 - MAP_X, MAP_X + MAP_SIZE - 1 - *x0, *y0 - MAP_Y, MAP_Y + MAP_SIZE - 1 - *y0 };
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

/* The minimap's lines, worked out every other frame and drawn every frame. */
#define MAP_LINES 600
static int16_t map_lines[MAP_LINES][4];
static uint8_t map_cls[MAP_LINES];
static int num_map_lines;

static void map_segment(const RoadSeg *sg, void *arg) {
	MapArg *a = arg;
	int32_t x0, y0, x1, y1;
	if (num_map_lines >= MAP_LINES)
		return;
	map_point(a, sg->x0, sg->z0, &x0, &y0);
	map_point(a, sg->x1, sg->z1, &x1, &y1);
	if (!clip(&x0, &y0, &x1, &y1))
		return;
	int16_t *l = map_lines[num_map_lines];
	l[0] = x0; l[1] = y0; l[2] = x1; l[3] = y1;
	map_cls[num_map_lines++] = sg->cls;
}

static uint8_t *dark_box(uint32_t *ot, uint8_t *prim, int x, int y, int w, int h) {
	TILE *t = (TILE *)prim;
	setTile(t);
	setSemiTrans(t, 1);
	setXY0(t, x, y);
	setWH(t, w, h);
	setRGB0(t, 16, 20, 28);
	addPrim(ot, t);
	return prim + sizeof(TILE);
}

static uint8_t *triangle(uint32_t *ot, uint8_t *prim, int x0, int y0, int x1, int y1, int x2, int y2, int r, int g, int b) {
	POLY_F3 *t = (POLY_F3 *)prim;
	setPolyF3(t);
	setXY3(t, x0, y0, x1, y1, x2, y2);
	setRGB0(t, r, g, b);
	addPrim(ot, t);
	return prim + sizeof(POLY_F3);
}

static uint8_t *minimap(const Car *car, const Camera *cam, uint32_t *ot, uint8_t *prim, const Place *target) {
	MapArg a = { ot, prim, CAR_TO_WORLD(car->x) / UNIT, CAR_TO_WORLD(car->z) / UNIT, isin(cam->heading), icos(cam->heading) };
	int mx = MAP_X + MAP_SIZE / 2, my = MAP_Y + MAP_SIZE / 2;
	/* Car marker (on top), target dot (clamped to the edge), roads. */
	int h = ((car->heading >> 8) - cam->heading) & 4095;
	int s = isin(h), c = icos(h);
	a.prim = triangle(ot, a.prim, mx + (s * 4 >> 12), my - (c * 4 >> 12), mx + ((-s * 3 - c * 3) >> 12), my + ((c * 3 - s * 3) >> 12),
	                  mx + ((-s * 3 + c * 3) >> 12), my + ((c * 3 + s * 3) >> 12), 255, 230, 60);
	if (target) {
		int32_t tx, ty;
		map_point(&a, target->x / UNIT, target->z / UNIT, &tx, &ty);
		if (tx < MAP_X + 2) tx = MAP_X + 2;
		if (tx > MAP_X + MAP_SIZE - 5) tx = MAP_X + MAP_SIZE - 5;
		if (ty < MAP_Y + 2) ty = MAP_Y + 2;
		if (ty > MAP_Y + MAP_SIZE - 5) ty = MAP_Y + MAP_SIZE - 5;
		TILE *t = (TILE *)a.prim;
		setTile(t);
		setXY0(t, tx - 1, ty - 1);
		setWH(t, 4, 4);
		if (fare_state == FARE_PICKUP) setRGB0(t, 60, 230, 90);
		else setRGB0(t, 255, 150, 30);
		addPrim(ot, t);
		a.prim += sizeof(TILE);
	}
	static int frame;
	if (!(frame++ & 1)) {
		num_map_lines = 0;
		world_segments(CAR_TO_WORLD(car->x), CAR_TO_WORLD(car->z), 2, map_segment, &a);
	}
	static const uint8_t col[3][3] = { { 150, 150, 150 }, { 220, 200, 120 }, { 240, 170, 80 } };
	for (int i = 0; i < num_map_lines; i++) {
		LINE_F2 *l = (LINE_F2 *)a.prim;
		setLineF2(l);
		setXY2(l, map_lines[i][0], map_lines[i][1], map_lines[i][2], map_lines[i][3]);
		setRGB0(l, col[map_cls[i]][0], col[map_cls[i]][1], col[map_cls[i]][2]);
		addPrim(ot, l);
		a.prim += sizeof(LINE_F2);
	}
	return dark_box(ot, a.prim, MAP_X, MAP_Y, MAP_SIZE, MAP_SIZE);
}

/* Arrow at the top of the screen pointing to the target (relative to the view). */
static uint8_t *arrow(const Car *car, const Camera *cam, const Place *target, uint32_t *ot, uint8_t *prim) {
	int32_t dx = (target->x - CAR_TO_WORLD(car->x)) >> 4, dz = (target->z - CAR_TO_WORLD(car->z)) >> 4;
	int32_t s = isin(cam->heading), c = icos(cam->heading);
	int32_t right = (dx * c - dz * s) >> 12, fwd = (dx * s + dz * c) >> 12;
	int32_t len = (right < 0 ? -right : right) + (fwd < 0 ? -fwd : fwd);
	if (!len) return prim;
	int32_t ux = right * 4096 / len, uy = -fwd * 4096 / len;   /* screen direction, ~unit */
	int cx = 160, cy = 40;
	int tipx = cx + (ux * 12 >> 12), tipy = cy + (uy * 12 >> 12);
	int bx = cx - (ux * 6 >> 12), by = cy - (uy * 6 >> 12);
	int px = -uy * 7 >> 12, py = ux * 7 >> 12;
	int r = fare_state == FARE_PICKUP ? 60 : 255, g = fare_state == FARE_PICKUP ? 230 : 150, b = fare_state == FARE_PICKUP ? 90 : 30;
	return triangle(ot, prim, tipx, tipy, bx + px, by + py, bx - px, by - py, r, g, b);
}

uint8_t *game_draw_hud(const Car *car, const Camera *cam, uint32_t *ot, uint8_t *prim) {
	char line[64];
	const Place *target = fare_state == FARE_PICKUP ? &places[fare_from] : fare_state == FARE_RIDE ? &places[fare_to] : 0;

	sprintf(line, "%02d:%02d  LEK %d", game_minutes / 60, game_minutes % 60, money);
	prim = FntSort(ot, prim, 8, 8, line);
	sprintf(line, "%3d KM/H", car_kmh(car));
	prim = FntSort(ot, prim, 248, 8, line);

	switch (fare_state) {
	case FARE_NONE:
		prim = FntSort(ot, prim, 8, 20, "WAITING FOR A CALL...");
		break;
	case FARE_PICKUP:
		sprintf(line, "PICK UP AT %s", world_name(places[fare_from].name));
		prim = FntSort(ot, prim, 8, 20, line);
		break;
	case FARE_RIDE:
		sprintf(line, "TO %s", world_name(places[fare_to].name));
		prim = FntSort(ot, prim, 8, 20, line);
		sprintf(line, "METER %d", meter);
		prim = FntSort(ot, prim, 8, 32, line);
		break;
	case FARE_PAID:
		sprintf(line, "PAID %d + TIP %d LEK", last_fare, last_tip);
		prim = FntSort(ot, prim, 8, 20, line);
		break;
	}
	if (board_ticks > 0 && (fare_state == FARE_PICKUP || fare_state == FARE_RIDE))
		prim = FntSort(ot, prim, 104, 100, fare_state == FARE_PICKUP ? "PASSENGER GETTING IN" : "PASSENGER GETTING OUT");

	const char *street = world_street_at(CAR_TO_WORLD(car->x), CAR_TO_WORLD(car->z));
	prim = FntSort(ot, prim, 8, 224, street);

	if (target)
		prim = arrow(car, cam, target, ot, prim);
	prim = minimap(car, cam, ot, prim, target);
	prim = dark_box(ot, prim, 0, 4, 320, fare_state == FARE_RIDE ? 40 : 28);
	prim = dark_box(ot, prim, 0, 220, 236, 16);
	/* Semi-transparency mode for the boxes: 50%. */
	prim = tpage_abr(ot, prim, 0);
	/* No texture window for the font (the city's faces set one). */
	DR_TWIN *w = (DR_TWIN *)prim;
	RECT none = { 0, 0, 0, 0 };
	setTexWindow(w, &none);
	addPrim(ot, w);
	return prim + sizeof(DR_TWIN);
}
