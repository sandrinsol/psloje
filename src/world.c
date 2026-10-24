/*
 * City streaming, drawing and collision (see world.h and tools/osm2map.mjs
 * for the MAP.BIN format).
 */
#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxcd.h>
#include <inline_c.h>
#include "world.h"
#include "textures.h"

#define SECTOR      2048
#define MAX_TILES   4096
#define NUM_SLOTS   ((2 * LOAD_RADIUS + 1) * (2 * LOAD_RADIUS + 1) + 2)
#define SLOT_EMPTY  0
#define SLOT_LOAD   1
#define SLOT_READY  2

typedef struct {
	uint16_t nverts, nfaces, nedges, nflat;
	int16_t minx, minz, maxx, maxz, miny, pad;
} TileHeader;

typedef struct {
	uint16_t v[4];
	uint8_t u0, v0, u1, v1;
	uint8_t texlayer;     /* texture id (15 = none) | flat layer << 4 */
	uint8_t r, g, b;
} Face;

typedef struct {
	int16_t x0, z0, x1, z1;
} Edge;

typedef struct {
	int16_t tx, tz;
	uint8_t state;
	TileHeader *h;
	SVECTOR *verts;
	Face *faces;
	Edge *edges;
} Slot;

typedef struct {
	char magic[4];
	uint16_t version, tiles_x, tiles_z, tile_units, header_sectors, max_sectors;
	int32_t spawn_x, spawn_z;
	int16_t spawn_heading;
	uint16_t global_sectors;
	char name[32];
	uint32_t global_sector;
} MapHeader;

#define GLOBAL_BYTES (160 * 1024)   /* must match tools/osm2map.mjs */

Colour fog_colour = { 172, 190, 204 };
Colour light = { 128, 128, 128 };
char map_name[32];
volatile int32_t tiles_loaded, tiles_drawn, faces_drawn, cd_errors;

static MapHeader header;
static uint32_t index_sector[MAX_TILES];
static uint16_t index_count[MAX_TILES];
static int map_lba;
static Slot slots[NUM_SLOTS];
static uint32_t slot_data[NUM_SLOTS][TILE_SLOT_BYTES / 4];
static int reading = -1;   /* slot being read, or -1 */

static uint32_t global_data[GLOBAL_BYTES / 4];
const Place *places;
int num_places;
static const RoadSeg *segs;
static int num_segs;
static const uint16_t *seg_bucket;   /* per tile: first, count */
static const uint16_t *name_offsets;
static const char *name_text;
static int num_names;

static uint16_t tpage;
static uint16_t clut;

/* Per-tile transformed vertices. */
static DVECTOR sxy[MAX_TILE_VERTS + 2];
static int32_t sz[MAX_TILE_VERTS + 2];

/* ---- CD -------------------------------------------------------------------- */

static void cd_start_read(int lba, int sectors, void *buf) {
	CdlLOC loc;
	CdIntToPos(lba, &loc);
	CdControl(CdlSetloc, &loc, 0);
	CdRead(sectors, (uint32_t *)buf, CdlModeSpeed);
}

static int cd_read(int lba, int sectors, void *buf) {
	for (int attempt = 0; attempt < 3; attempt++) {
		cd_start_read(lba, sectors, buf);
		if (CdReadSync(0, 0) >= 0)
			return 1;
		cd_errors++;
	}
	return 0;
}

extern const uint32_t city_tim[];

int world_init(void) {
	static uint32_t sector[SECTOR / 4];
	CdlFILE file;
	TIM_IMAGE tim;

	GetTimInfo(city_tim, &tim);
	LoadImage(tim.prect, tim.paddr);
	LoadImage(tim.crect, tim.caddr);
	DrawSync(0);
	tpage = getTPage(1, 0, CITY_TPAGE_X, CITY_TPAGE_Y);
	clut = getClut(CITY_CLUT_X, CITY_CLUT_Y);

	CdInit();
	if (!CdSearchFile(&file, "\\MAP.BIN;1"))
		return 0;
	map_lba = CdPosToInt(&file.pos);
	if (!cd_read(map_lba, 1, sector))
		return 0;
	memcpy(&header, sector, sizeof(header));
	if (memcmp(header.magic, "VMAP", 4) || header.version < 2 || header.tiles_x * header.tiles_z > MAX_TILES ||
	    header.global_sectors * SECTOR > GLOBAL_BYTES)
		return 0;
	memcpy(map_name, header.name, sizeof(map_name) - 1);

	/* The tile index follows the 64-byte header, over header_sectors sectors. */
	int n = header.tiles_x * header.tiles_z;
	for (int s = 0, t = 0; s < header.header_sectors && t < n; s++) {
		if (s > 0 && !cd_read(map_lba + s, 1, sector))
			return 0;
		const uint8_t *p = (const uint8_t *)sector;
		int start = s == 0 ? 64 : 0;
		for (int off = start; off + 8 <= SECTOR && t < n; off += 8, t++) {
			index_sector[t] = p[off] | p[off + 1] << 8 | p[off + 2] << 16 | p[off + 3] << 24;
			index_count[t] = p[off + 4] | p[off + 5] << 8;
		}
	}
	/* Places, minimap roads and names: read once, kept in RAM. */
	if (header.global_sectors) {
		if (!cd_read(map_lba + header.global_sector, header.global_sectors, global_data))
			return 0;
		const uint16_t *g = (const uint16_t *)global_data;
		num_places = g[0];
		num_segs = g[1];
		num_names = g[2];
		const uint8_t *p = (const uint8_t *)global_data + 8;
		places = (const Place *)p;
		p += num_places * sizeof(Place);
		seg_bucket = (const uint16_t *)p;
		p += g[3] * 4;
		segs = (const RoadSeg *)p;
		p += num_segs * sizeof(RoadSeg);
		name_offsets = (const uint16_t *)p;
		name_text = (const char *)(p + num_names * 2);
	}
	for (int i = 0; i < NUM_SLOTS; i++)
		slots[i].state = SLOT_EMPTY;
	return 1;
}

const char *world_name(int id) {
	if (id < 0 || id >= num_names)
		return "";
	return name_text + name_offsets[id];
}

void world_segments(int32_t x, int32_t z, int radius_tiles, void (*fn)(const RoadSeg *, void *), void *arg) {
	if (!segs)
		return;
	int ctx = x / TILE_UNITS, ctz = z / TILE_UNITS;
	for (int tz = ctz - radius_tiles; tz <= ctz + radius_tiles; tz++) {
		if (tz < 0 || tz >= header.tiles_z) continue;
		for (int tx = ctx - radius_tiles; tx <= ctx + radius_tiles; tx++) {
			if (tx < 0 || tx >= header.tiles_x) continue;
			const uint16_t *b = seg_bucket + (tz * header.tiles_x + tx) * 2;
			for (int i = 0; i < b[1]; i++)
				fn(&segs[b[0] + i], arg);
		}
	}
}

typedef struct { int32_t x, z, best; int name; } NearestArg;

static void nearest_seg(const RoadSeg *sg, void *arg) {
	NearestArg *a = arg;
	if (sg->name == 0xffff) return;
	/* metres, relative to the point */
	int32_t ax = sg->x0 - a->x, az = sg->z0 - a->z, bx = sg->x1 - a->x, bz = sg->z1 - a->z;
	int32_t ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
	int32_t t = l2 ? (-(ax * ex + az * ez)) * 256 / l2 : 0;
	if (t < 0) t = 0;
	if (t > 256) t = 256;
	int32_t cx = ax + (ex * t >> 8), cz = az + (ez * t >> 8);
	int32_t d = cx * cx + cz * cz;
	if (d < a->best) { a->best = d; a->name = sg->name; }
}

const char *world_street_at(int32_t x, int32_t z) {
	NearestArg a = { x / UNIT, z / UNIT, 25 * 25, -1 };   /* within 25 m */
	world_segments(x, z, 1, nearest_seg, &a);
	return a.name < 0 ? "" : world_name(a.name);
}

void world_spawn(int32_t *x, int32_t *z, int *heading) {
	*x = header.spawn_x;
	*z = header.spawn_z;
	*heading = header.spawn_heading;
}

void world_size(int32_t *w, int32_t *h) {
	*w = header.tiles_x * TILE_UNITS;
	*h = header.tiles_z * TILE_UNITS;
}

static void slot_ready(Slot *s) {
	uint8_t *p = (uint8_t *)slot_data[s - slots];
	s->h = (TileHeader *)p;
	s->verts = (SVECTOR *)(p + sizeof(TileHeader));
	s->faces = (Face *)(s->verts + s->h->nverts);
	s->edges = (Edge *)(s->faces + s->h->nfaces);
	s->state = SLOT_READY;
	tiles_loaded++;
}

static int tile_dist(const Slot *s, int tx, int tz) {
	int dx = s->tx - tx, dz = s->tz - tz;
	if (dx < 0) dx = -dx;
	if (dz < 0) dz = -dz;
	return dx > dz ? dx : dz;
}

static int find_slot(int tx, int tz) {
	for (int i = 0; i < NUM_SLOTS; i++)
		if (slots[i].state != SLOT_EMPTY && slots[i].tx == tx && slots[i].tz == tz)
			return i;
	return -1;
}

void world_stream(int32_t x, int32_t z, int wait) {
	int ctx = x / TILE_UNITS, ctz = z / TILE_UNITS;

	do {
		/* Finish the read in flight. */
		if (reading >= 0) {
			int r = CdReadSync(wait ? 0 : 1, 0);
			if (r < 0) {
				cd_errors++;
				slots[reading].state = SLOT_EMPTY;   /* try again later */
				reading = -1;
			} else if (r == 0) {
				slot_ready(&slots[reading]);
				reading = -1;
			} else {
				return;   /* still reading */
			}
		}
		/* Closest missing tile within the radius. */
		int best_tx = 0, best_tz = 0, best_d = 99;
		for (int dz = -LOAD_RADIUS; dz <= LOAD_RADIUS; dz++) {
			for (int dx = -LOAD_RADIUS; dx <= LOAD_RADIUS; dx++) {
				int tx = ctx + dx, tz = ctz + dz;
				if (tx < 0 || tz < 0 || tx >= header.tiles_x || tz >= header.tiles_z)
					continue;
				int d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
				if (d < best_d && find_slot(tx, tz) < 0) {
					best_d = d;
					best_tx = tx;
					best_tz = tz;
				}
			}
		}
		if (best_d == 99)
			return;   /* everything is in */
		/* A free slot, or the one furthest outside the radius. */
		int slot = -1, far = LOAD_RADIUS;
		for (int i = 0; i < NUM_SLOTS; i++) {
			if (slots[i].state == SLOT_EMPTY) { slot = i; break; }
			int d = tile_dist(&slots[i], ctx, ctz);
			if (slots[i].state == SLOT_READY && d > far) { far = d; slot = i; }
		}
		if (slot < 0)
			return;
		Slot *s = &slots[slot];
		int t = best_tz * header.tiles_x + best_tx;
		s->tx = best_tx;
		s->tz = best_tz;
		if (index_count[t] == 0 || index_count[t] * SECTOR > TILE_SLOT_BYTES) {
			/* Empty (or impossible) tile: nothing to read. */
			memset(slot_data[slot], 0, sizeof(TileHeader));
			slot_ready(s);
			continue;
		}
		s->state = SLOT_LOAD;
		reading = slot;
		cd_start_read(map_lba + index_sector[t], index_count[t], slot_data[slot]);
	} while (wait || reading < 0);
}

/* ---- GTE ----------------------------------------------------------------------- */

/* PSn00bSDK's gte_SetRotMatrix() uses $t0-$t2 but declares $12/$13 as
 * clobbered, so the compiler may keep a value in $t1 across it. Behind a
 * real (non-inlined) call those registers are caller-saved and safe. */
__attribute__((noinline)) void gte_load_matrix(const MATRIX *m) {
	gte_SetRotMatrix(m);
	gte_SetTransMatrix(m);
}

/* ---- Camera ------------------------------------------------------------------ */

void camera_update(Camera *cam) {
	int sh = isin(cam->heading), ch = icos(cam->heading);
	int sp = isin(cam->pitch), cp = icos(cam->pitch);
	/* Rows: camera right, down and forward, in world axes. */
	cam->m.m[0][0] = ch;              cam->m.m[0][1] = 0;  cam->m.m[0][2] = -sh;
	cam->m.m[1][0] = -(sh * sp >> 12); cam->m.m[1][1] = cp; cam->m.m[1][2] = -(ch * sp >> 12);
	cam->m.m[2][0] = sh * cp >> 12;   cam->m.m[2][1] = sp; cam->m.m[2][2] = ch * cp >> 12;
	/* The GTE's copy, scaled: same picture, depths GTE_SCALE times bigger. */
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			cam->gm.m[r][c] = cam->m.m[r][c] * GTE_SCALE;
}

/* ---- Drawing ----------------------------------------------------------------- */

static uint8_t *prim, *prim_end;
static uint32_t *ot_base;

static inline int fog_amount(int z) {
	if (z <= FOG_START) return 0;
	if (z >= FOG_END) return 256;
	return (z - FOG_START) * 256 / (FOG_END - FOG_START);
}

static inline void fogged(uint8_t *out, const uint8_t *c, int z) {
	int f = fog_amount(z);
	out[0] = c[0] + (((int)fog_colour.r - c[0]) * f >> 8);
	out[1] = c[1] + (((int)fog_colour.g - c[1]) * f >> 8);
	out[2] = c[2] + (((int)fog_colour.b - c[2]) * f >> 8);
}

/* The colour a face has without its texture: the texture's average tinted
 * by the face colour (128 = unchanged). */
static void base_colour(const Face *f, uint8_t *c) {
	int tex = f->texlayer & 15;
	int r = f->r, g = f->g, b = f->b;
	if (tex < NUM_TEXTURES) {
		r = texture_avg[tex][0] * r >> 7;
		g = texture_avg[tex][1] * g >> 7;
		b = texture_avg[tex][2] * b >> 7;
	}
	r = r * light.r >> 7;
	g = g * light.g >> 7;
	b = b * light.b >> 7;
	c[0] = r > 255 ? 255 : r; c[1] = g > 255 ? 255 : g; c[2] = b > 255 ? 255 : b;
}

static inline int front_facing(DVECTOR a, DVECTOR b, DVECTOR c) {
	return (b.vx - a.vx) * (c.vy - a.vy) - (b.vy - a.vy) * (c.vx - a.vx) > 0;
}

static inline int on_screen(int x, int y) {
	return x > -1000 && x < 1000 && y > -1000 && y < 1000;
}

/* Too big for the GPU (max 1023 x 511): such faces get split. */
static inline int too_big(const DVECTOR *p, int n) {
	int xmin = p[0].vx, xmax = p[0].vx, ymin = p[0].vy, ymax = p[0].vy;
	for (int i = 1; i < n; i++) {
		if (p[i].vx < xmin) xmin = p[i].vx;
		if (p[i].vx > xmax) xmax = p[i].vx;
		if (p[i].vy < ymin) ymin = p[i].vy;
		if (p[i].vy > ymax) ymax = p[i].vy;
	}
	return xmax - xmin > 1000 || ymax - ymin > 500;
}

static void emit(const Face *f, DVECTOR *p, int32_t *z, int u0, int v0, int u1, int v1, int tri, uint32_t *ot) {
	int tex = f->texlayer & 15;
	int zmax = z[0];
	for (int i = 1; i < (tri ? 3 : 4); i++) if (z[i] > zmax) zmax = z[i];

	/* GPU limit: no polygon wider than 1023 or taller than 511. */
	int xmin = p[0].vx, xmax = p[0].vx, ymin = p[0].vy, ymax = p[0].vy;
	for (int i = 1; i < (tri ? 3 : 4); i++) {
		if (p[i].vx < xmin) xmin = p[i].vx;
		if (p[i].vx > xmax) xmax = p[i].vx;
		if (p[i].vy < ymin) ymin = p[i].vy;
		if (p[i].vy > ymax) ymax = p[i].vy;
	}
	if (xmax < 0 || xmin >= 320 || ymax < 0 || ymin >= 240 || xmax - xmin > 1023 || ymax - ymin > 511)
		return;
	if (prim + sizeof(POLY_GT4) > prim_end)
		return;

	if (tex < NUM_TEXTURES && zmax < TEX_DIST && !tri) {
		POLY_FT4 *q = (POLY_FT4 *)prim;
		setPolyFT4(q);
		setXY4(q, p[0].vx, p[0].vy, p[1].vx, p[1].vy, p[2].vx, p[2].vy, p[3].vx, p[3].vy);
		int tu = (tex & 1) * 64, tv = (tex >> 1) * 64;
		setUV4(q, tu + u0, tv + v0, tu + u1, tv + v0, tu + u0, tv + v1, tu + u1, tv + v1);
		setRGB0(q, f->r * light.r >> 7, f->g * light.g >> 7, f->b * light.b >> 7);
		q->tpage = tpage;
		q->clut = clut;
		addPrim(ot, q);
		prim += sizeof(POLY_FT4);
	} else {
		uint8_t c[3], c0[3], c1[3], c2[3];
		base_colour(f, c);
		fogged(c0, c, z[0]);
		fogged(c1, c, z[1]);
		fogged(c2, c, z[2]);
		if (tri) {
			POLY_G3 *q = (POLY_G3 *)prim;
			setPolyG3(q);
			setXY3(q, p[0].vx, p[0].vy, p[1].vx, p[1].vy, p[2].vx, p[2].vy);
			setRGB0(q, c0[0], c0[1], c0[2]);
			setRGB1(q, c1[0], c1[1], c1[2]);
			setRGB2(q, c2[0], c2[1], c2[2]);
			addPrim(ot, q);
			prim += sizeof(POLY_G3);
		} else {
			uint8_t c3[3];
			fogged(c3, c, z[3]);
			POLY_G4 *q = (POLY_G4 *)prim;
			setPolyG4(q);
			setXY4(q, p[0].vx, p[0].vy, p[1].vx, p[1].vy, p[2].vx, p[2].vy, p[3].vx, p[3].vy);
			setRGB0(q, c0[0], c0[1], c0[2]);
			setRGB1(q, c1[0], c1[1], c1[2]);
			setRGB2(q, c2[0], c2[1], c2[2]);
			setRGB3(q, c3[0], c3[1], c3[2]);
			addPrim(ot, q);
			prim += sizeof(POLY_G4);
		}
	}
	faces_drawn++;
}

static inline uint32_t *ot_slot(const Face *f, int flat, int32_t *z, int n) {
	if (flat)
		return ot_base + OT_LEN - 1 - (f->texlayer >> 4);
	int sum = 0;
	for (int i = 0; i < n; i++) sum += z[i];
	int otz = (n == 4 ? sum >> 2 : sum / 3) >> 4;
	if (otz < 1) otz = 1;
	if (otz > OT_LEN - 8) otz = OT_LEN - 8;
	return ot_base + otz;
}

/* A face crossing the near plane: split it (in tile space) until the pieces
 * are in front of the camera. Triangles are quads whose last two corners
 * are the same point. */
static void draw_split(const Face *f, int flat, const SVECTOR *c, int u0, int v0, int u1, int v1, int depth) {
	DVECTOR p[4];
	int32_t z[4];
	VECTOR cv;
	int nearc = 0, farc = 0, all_out = 15;
	for (int i = 0; i < 4; i++) {
		gte_ldv0(&c[i]);
		gte_rtps();
		gte_stsxy(&p[i]);
		gte_stsz(&z[i]);
		gte_stlvnl(&cv);
		if (cv.vz < NEAR_Z) nearc++;
		if (cv.vz > FOG_END) farc++;
		/* Outside the view pyramid (half-space tests: also right for points
		 * behind the camera): left, right, above, below. */
		int out = 0;
		if (cv.vx * 5 < -cv.vz * 4) out |= 1;
		if (cv.vx * 5 > cv.vz * 4) out |= 2;
		if (cv.vy * 5 < -cv.vz * 3) out |= 4;
		if (cv.vy * 5 > cv.vz * 3) out |= 8;
		all_out &= out;
	}
	if (nearc == 4 || farc == 4 || all_out)
		return;
	int ok = nearc == 0;
	for (int i = 0; ok && i < 4; i++) ok = on_screen(p[i].vx, p[i].vy);
	if (ok) ok = !too_big(p, 4);
	if (ok) {
		if (!front_facing(p[0], p[1], p[2]) && !front_facing(p[1], p[3], p[2]))
			return;
		emit(f, p, z, u0, v0, u1, v1, 0, ot_slot(f, flat, z, 4));
		return;
	}
	if (depth >= 5)
		return;
	/* Bilinear split into four: corners 0 1 / 2 3 (Z order). */
	SVECTOR m[9];
	m[0] = c[0]; m[2] = c[1]; m[6] = c[2]; m[8] = c[3];
#define MID(d, a, b) (d).vx = ((a).vx + (b).vx) >> 1, (d).vy = ((a).vy + (b).vy) >> 1, (d).vz = ((a).vz + (b).vz) >> 1
	MID(m[1], c[0], c[1]);
	MID(m[3], c[0], c[2]);
	MID(m[5], c[1], c[3]);
	MID(m[7], c[2], c[3]);
	MID(m[4], m[3], m[5]);
#undef MID
	int um = (u0 + u1) >> 1, vm = (v0 + v1) >> 1;
	SVECTOR q[4];
	static const uint8_t pick[4][4] = { { 0, 1, 3, 4 }, { 1, 2, 4, 5 }, { 3, 4, 6, 7 }, { 4, 5, 7, 8 } };
	for (int k = 0; k < 4; k++) {
		for (int i = 0; i < 4; i++) q[i] = m[pick[k][i]];
		draw_split(f, flat, q, k & 1 ? um : u0, k & 2 ? vm : v0, k & 1 ? u1 : um, k & 2 ? v1 : vm, depth + 1);
	}
}

/* Is any part of the tile's box in front of the camera and inside the view? */
static int tile_visible(const Camera *cam, const Slot *s, int32_t ox, int32_t oz) {
	const TileHeader *h = s->h;
	int left = 0, right = 0, behind = 0, beyond = 0;
	for (int i = 0; i < 8; i++) {
		int32_t x = ox + (i & 1 ? h->maxx : h->minx) - cam->x;
		int32_t y = (i & 2 ? h->miny : 0) - cam->y;
		int32_t z = oz + (i & 4 ? h->maxz : h->minz) - cam->z;
		int32_t cx = (cam->m.m[0][0] * x + cam->m.m[0][1] * y + cam->m.m[0][2] * z) >> 12;
		int32_t cz = (cam->m.m[2][0] * x + cam->m.m[2][1] * y + cam->m.m[2][2] * z) >> 12;
		if (cz * GTE_SCALE < NEAR_Z) behind++;
		if (cz * GTE_SCALE > FOG_END) beyond++;
		/* Screen half width 160 at projection distance H=200: |x| > z * 0.8 is outside. */
		if (cx > cz * 4 / 5 + 32) right++;
		if (cx < -cz * 4 / 5 - 32) left++;
	}
	return behind < 8 && beyond < 8 && left < 8 && right < 8;
}

static void draw_tile(const Camera *cam, const Slot *s) {
	const TileHeader *h = s->h;
	if (!h->nfaces)
		return;
	int32_t ox = s->tx * TILE_UNITS, oz = s->tz * TILE_UNITS;
	if (!tile_visible(cam, s, ox, oz))
		return;
	tiles_drawn++;

	/* translation = R * (tile origin - camera), scaled like the matrix */
	MATRIX m = cam->gm;
	int32_t dx = ox - cam->x, dy = -cam->y, dz = oz - cam->z;
	m.t[0] = (cam->m.m[0][0] * dx + cam->m.m[0][1] * dy + cam->m.m[0][2] * dz) * GTE_SCALE >> 12;
	m.t[1] = (cam->m.m[1][0] * dx + cam->m.m[1][1] * dy + cam->m.m[1][2] * dz) * GTE_SCALE >> 12;
	m.t[2] = (cam->m.m[2][0] * dx + cam->m.m[2][1] * dy + cam->m.m[2][2] * dz) * GTE_SCALE >> 12;
	gte_load_matrix(&m);

	int n = h->nverts;
	const SVECTOR *v = s->verts;
	int i = 0;
	for (; i + 3 <= n; i += 3) {
		gte_ldv3(&v[i], &v[i + 1], &v[i + 2]);
		gte_rtpt();
		gte_stsxy3(&sxy[i], &sxy[i + 1], &sxy[i + 2]);
		gte_stsz3(&sz[i], &sz[i + 1], &sz[i + 2]);
	}
	for (; i < n; i++) {
		gte_ldv0(&v[i]);
		gte_rtps();
		gte_stsxy(&sxy[i]);
		gte_stsz(&sz[i]);
	}

	const Face *f = s->faces;
	for (int k = 0; k < h->nfaces; k++, f++) {
		int tri = f->v[3] == 0xffff;
		int nv = tri ? 3 : 4;
		int flat = k < h->nflat;
		DVECTOR p[4];
		int32_t z[4];
		int nearc = 0, farc = 0;
		for (int j = 0; j < nv; j++) {
			p[j] = sxy[f->v[j]];
			z[j] = sz[f->v[j]];
			if (z[j] < NEAR_Z) nearc++;
			if (z[j] > FOG_END) farc++;
		}
		if (nearc == nv || farc == nv)
			continue;
		int split = nearc > 0;
		for (int j = 0; !split && j < nv; j++) split = !on_screen(p[j].vx, p[j].vy);
		if (!split) split = too_big(p, nv);
		if (split) {
			SVECTOR c[4];
			for (int j = 0; j < 4; j++) c[j] = v[f->v[tri && j == 3 ? 2 : j]];
			draw_split(f, flat, c, f->u0, f->v0, f->u1, f->v1, 0);
			continue;
		}
		if (!front_facing(p[0], p[1], p[2]) && (tri || !front_facing(p[1], p[3], p[2])))
			continue;
		emit(f, p, z, f->u0, f->v0, f->u1, f->v1, tri, ot_slot(f, flat, z, nv));
	}
}

uint8_t *world_draw(const Camera *cam, uint32_t *ot, uint8_t *prims, uint8_t *prims_end) {
	prim = prims;
	prim_end = prims_end;
	ot_base = ot;
	tiles_drawn = 0;
	faces_drawn = 0;
	for (int i = 0; i < NUM_SLOTS; i++)
		if (slots[i].state == SLOT_READY)
			draw_tile(cam, &slots[i]);
	return prim;
}

/* ---- Collision --------------------------------------------------------------- */

static int32_t isqrt(int32_t v) {
	int32_t r = 0, bit = 1 << 30;
	if (v <= 0) return 0;
	while (bit > v) bit >>= 2;
	while (bit) {
		if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return r;
}

int world_collide(int32_t *x, int32_t *z, int32_t radius, int32_t *nx, int32_t *nz) {
	int hit = 0;
	int ctx = (*x >> 4) / TILE_UNITS, ctz = (*z >> 4) / TILE_UNITS;
	*nx = *nz = 0;
	for (int i = 0; i < NUM_SLOTS; i++) {
		Slot *s = &slots[i];
		if (s->state != SLOT_READY || tile_dist(s, ctx, ctz) > 1)
			continue;
		/* Circle centre relative to the tile, in 1/256 m. */
		int32_t px = *x - s->tx * TILE_UNITS * 16, pz = *z - s->tz * TILE_UNITS * 16;
		const Edge *e = s->edges;
		for (int k = 0; k < s->h->nedges; k++, e++) {
			int32_t ax = e->x0 * 16, az = e->z0 * 16, bx = e->x1 * 16, bz = e->z1 * 16;
			/* Quick reject on the edge's box. */
			if (px + radius < (ax < bx ? ax : bx) || px - radius > (ax > bx ? ax : bx) ||
			    pz + radius < (az < bz ? az : bz) || pz - radius > (az > bz ? az : bz))
				continue;
			int32_t ex = bx - ax, ez = bz - az;
			int32_t l2 = ex * ex + ez * ez;
			if (!l2) continue;
			int32_t rx = px - ax, rz = pz - az;
			/* Projection onto the edge, 0..4096. */
			int32_t t = (int32_t)(((int64_t)rx * ex + (int64_t)rz * ez) * 4096 / l2);
			if (t < 0) t = 0;
			if (t > 4096) t = 4096;
			int32_t cx = ax + (ex * t >> 12), cz = az + (ez * t >> 12);
			int32_t dx = px - cx, dz = pz - cz;
			int32_t d2 = dx * dx + dz * dz;
			if (d2 >= radius * radius) continue;
			int32_t d = isqrt(d2);
			int32_t ux, uz;
			if (d > 0) { ux = dx * 4096 / d; uz = dz * 4096 / d; }
			else { ux = -ez * 4096 / isqrt(l2); uz = ex * 4096 / isqrt(l2); }
			int32_t push = radius - d;
			px += ux * push >> 12;
			pz += uz * push >> 12;
			*nx += ux;
			*nz += uz;
			hit = 1;
		}
		*x = px + s->tx * TILE_UNITS * 16;
		*z = pz + s->tz * TILE_UNITS * 16;
	}
	if (hit) {
		int32_t l = isqrt(*nx * *nx + *nz * *nz);
		if (l) { *nx = *nx * 4096 / l; *nz = *nz * 4096 / l; }
	}
	return hit;
}
