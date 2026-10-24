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
#include "route.h"
#include "textures.h"

#define SECTOR      2048
#define MAX_TILES   16384
#define NUM_NEAR    ((2 * LOAD_RADIUS + 1) * (2 * LOAD_RADIUS + 1) + 2)
#define NUM_FAR     ((2 * FAR_RADIUS + 1) * (2 * FAR_RADIUS + 1) - (2 * LOAD_RADIUS + 1) * (2 * LOAD_RADIUS + 1) + 8)
#define SLOT_EMPTY  0
#define SLOT_LOAD   1
#define SLOT_READY  2

typedef struct {
	uint16_t nverts, nfaces, nedges, nflat;
	int16_t minx, minz, maxx, maxz, miny, flags;   /* flags 1: height grid after the edges */
	uint16_t ngroups, nsegs, nflatverts, pad;   /* vertices: the flat faces' first, then each group's */
} TileHeader;

/* One building: culled as a whole, fine faces near, coarse ones far. */
typedef struct {
	int16_t cx, cz, r, top, bottom;   /* tile-local units; top/bottom are Y (negative = up) */
	uint16_t first, nfine, ncoarse;   /* faces; coarse follow the fine (0: use the fine) */
	uint16_t vfirst, vcount;          /* its vertices */
	uint16_t nroof, pad;              /* the last nroof faces (fine and coarse) are the roof */
} Group;

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
	int16_t *heights;   /* 9 x 9 ground Y every 8 m, or 0 */
	Group *groups;
	RoadSeg *segs;
	uint32_t *data;
} Slot;

typedef struct {
	char magic[4];
	uint16_t version, tiles_x, tiles_z, tile_units, header_sectors, graph_sectors;
	int32_t spawn_x, spawn_z;
	int16_t spawn_heading;
	uint16_t global_sectors;
	char name[32];
	uint32_t global_sector;
} MapHeader;

#define GLOBAL_BYTES (64 * 1024)   /* places and names; must match tools/osm2map.mjs */

Colour fog_colour = { 172, 190, 204 };
Colour light = { 128, 128, 128 };
char map_name[32];
volatile int32_t tiles_loaded, tiles_drawn, faces_drawn, cd_errors;
volatile int32_t stat_faces_seen, stat_groups_seen, stat_groups_drawn, stat_far_tiles, stat_verts;

static MapHeader header;
/* Per tile: first sector << 10 | far sectors << 5 | near sectors (the far
 * blob, then the near one). */
static uint32_t tile_index[MAX_TILES];
static int map_lba;
/* Near: everything, within LOAD_RADIUS tiles. Far: building boxes and coarse
 * ground, out to FAR_RADIUS tiles (the view reaches ~200 m). */
static Slot near_slots[NUM_NEAR], far_slots[NUM_FAR];
static uint32_t near_data[NUM_NEAR][TILE_SLOT_BYTES / 4];
static uint32_t far_data[NUM_FAR][FAR_SLOT_BYTES / 4];
static Slot *reading;   /* slot being read, or 0 */

static uint32_t global_data[GLOBAL_BYTES / 4];
const Place *places;
int num_places;
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
	if (memcmp(header.magic, "VMAP", 4) || header.version != 4 || header.tiles_x * header.tiles_z > MAX_TILES ||
	    header.global_sectors * SECTOR > GLOBAL_BYTES)
		return 0;
	memcpy(map_name, header.name, sizeof(map_name) - 1);

	/* The tile index follows the 64-byte header, over header_sectors sectors. */
	int n = header.tiles_x * header.tiles_z;
	for (int s = 0, t = 0; s < header.header_sectors && t < n; s++) {
		if (s > 0 && !cd_read(map_lba + s, 1, sector))
			return 0;
		for (int i = s == 0 ? 16 : 0; i < SECTOR / 4 && t < n; i++, t++)
			tile_index[t] = sector[i];
	}
	/* Places and names: read once, kept in RAM. */
	if (header.global_sectors) {
		if (!cd_read(map_lba + header.global_sector, header.global_sectors, global_data))
			return 0;
		const uint16_t *g = (const uint16_t *)global_data;
		num_places = g[0];
		num_names = g[1];
		const uint8_t *p = (const uint8_t *)global_data + 8;
		places = (const Place *)p;
		p += num_places * sizeof(Place);
		name_offsets = (const uint16_t *)p;
		name_text = (const char *)(p + num_names * 2);
	}
	/* The road graph (routes, the big map) follows them. */
	if (header.graph_sectors && header.graph_sectors * SECTOR <= GRAPH_BYTES) {
		if (!cd_read(map_lba + header.global_sector + header.global_sectors, header.graph_sectors, graph_data))
			return 0;
		route_init(header.graph_sectors * SECTOR);
	}
	for (int i = 0; i < NUM_NEAR; i++) {
		near_slots[i].state = SLOT_EMPTY;
		near_slots[i].data = near_data[i];
	}
	for (int i = 0; i < NUM_FAR; i++) {
		far_slots[i].state = SLOT_EMPTY;
		far_slots[i].data = far_data[i];
	}
	return 1;
}

const char *world_name(int id) {
	if (id < 0 || id >= num_names)
		return "";
	return name_text + name_offsets[id];
}

static int tile_dist(const Slot *s, int tx, int tz) {
	int dx = s->tx - tx, dz = s->tz - tz;
	if (dx < 0) dx = -dx;
	if (dz < 0) dz = -dz;
	return dx > dz ? dx : dz;
}

static Slot *find_slot(Slot *slots, int n, int tx, int tz) {
	for (int i = 0; i < n; i++)
		if (slots[i].state != SLOT_EMPTY && slots[i].tx == tx && slots[i].tz == tz)
			return &slots[i];
	return 0;
}

static Slot *near_ready(int tx, int tz) {
	Slot *s = find_slot(near_slots, NUM_NEAR, tx, tz);
	return s && s->state == SLOT_READY ? s : 0;
}

void world_segments(int32_t x, int32_t z, int radius_tiles, void (*fn)(const RoadSeg *, void *), void *arg) {
	int ctx = x / TILE_UNITS, ctz = z / TILE_UNITS;
	for (int i = 0; i < NUM_NEAR; i++) {
		Slot *s = &near_slots[i];
		if (s->state != SLOT_READY || tile_dist(s, ctx, ctz) > radius_tiles)
			continue;
		for (int k = 0; k < s->h->nsegs; k++)
			fn(&s->segs[k], arg);
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
	uint8_t *p = (uint8_t *)s->data;
	s->h = (TileHeader *)p;
	s->verts = (SVECTOR *)(p + sizeof(TileHeader));
	s->faces = (Face *)(s->verts + s->h->nverts);
	s->edges = (Edge *)(s->faces + s->h->nfaces);
	s->heights = s->h->flags & 1 ? (int16_t *)(s->edges + s->h->nedges) : 0;
	s->groups = (Group *)(s->heights ? s->heights + 81 : (int16_t *)(s->edges + s->h->nedges));
	s->segs = (RoadSeg *)(s->groups + s->h->ngroups);
	s->state = SLOT_READY;
	tiles_loaded++;
}

/* The closest tile in the ring min_d..max_d (Chebyshev) around (ctx, ctz)
 * that has no slot yet in `slots` (and, for far ones, no near copy). */
static int missing(Slot *slots, int n, int ctx, int ctz, int min_d, int max_d, int *otx, int *otz) {
	int best = 1 << 30;
	for (int dz = -max_d; dz <= max_d; dz++) {
		for (int dx = -max_d; dx <= max_d; dx++) {
			int adx = dx < 0 ? -dx : dx, adz = dz < 0 ? -dz : dz;
			if ((adx > adz ? adx : adz) < min_d)
				continue;
			int tx = ctx + dx, tz = ctz + dz;
			if (tx < 0 || tz < 0 || tx >= header.tiles_x || tz >= header.tiles_z)
				continue;
			int d = dx * dx + dz * dz;
			if (d >= best || find_slot(slots, n, tx, tz) || (slots == far_slots && near_ready(tx, tz)))
				continue;
			best = d;
			*otx = tx;
			*otz = tz;
		}
	}
	return best < (1 << 30);
}

/* A free slot, else the ready one furthest beyond keep_d (far slots whose
 * tile now has a near copy are free too), else 0. */
static Slot *free_slot(Slot *slots, int n, int ctx, int ctz, int keep_d) {
	Slot *pick = 0;
	int far = keep_d;
	for (int i = 0; i < n; i++) {
		Slot *s = &slots[i];
		if (s->state == SLOT_EMPTY) return s;
		if (s->state != SLOT_READY) continue;
		if (slots == far_slots && near_ready(s->tx, s->tz)) return s;
		int d = tile_dist(s, ctx, ctz);
		if (d > far) { far = d; pick = s; }
	}
	return pick;
}

void world_stream(int32_t x, int32_t z, int wait) {
	static int last_ctx = -1, last_ctz = -1, complete;
	int ctx = x / TILE_UNITS, ctz = z / TILE_UNITS;
	/* Nothing to do while everything around this tile is in. */
	if (ctx != last_ctx || ctz != last_ctz) {
		last_ctx = ctx;
		last_ctz = ctz;
		complete = 0;
	}
	if (complete && !reading)
		return;

	for (;;) {
		/* Finish the read in flight. */
		if (reading) {
			int r = CdReadSync(wait ? 0 : 1, 0);
			if (r > 0)
				return;   /* still reading */
			if (r < 0) {
				cd_errors++;
				reading->state = SLOT_EMPTY;   /* try again later */
			} else {
				slot_ready(reading);
			}
			reading = 0;
		}
		/* Near tiles first (closest first), then the far ring. Waiting (at the
		 * start) only covers the near ones. */
		int tx, tz, is_near = 1;
		if (!missing(near_slots, NUM_NEAR, ctx, ctz, 0, LOAD_RADIUS, &tx, &tz)) {
			if (wait)
				return;
			if (!missing(far_slots, NUM_FAR, ctx, ctz, LOAD_RADIUS + 1, FAR_RADIUS, &tx, &tz)) {
				complete = 1;
				return;
			}
			is_near = 0;
		}
		Slot *s = is_near ? free_slot(near_slots, NUM_NEAR, ctx, ctz, LOAD_RADIUS)
		                  : free_slot(far_slots, NUM_FAR, ctx, ctz, FAR_RADIUS);
		if (!s)
			return;
		uint32_t e = tile_index[tz * header.tiles_x + tx];
		int first = e >> 10, nfar = (e >> 5) & 31, nnear = e & 31;
		int lba = map_lba + first + (is_near ? nfar : 0), count = is_near ? nnear : nfar;
		s->tx = tx;
		s->tz = tz;
		if (!count || count * SECTOR > (is_near ? TILE_SLOT_BYTES : FAR_SLOT_BYTES)) {
			memset(s->data, 0, sizeof(TileHeader));   /* nothing to read */
			slot_ready(s);
			continue;
		}
		s->state = SLOT_LOAD;
		reading = s;
		cd_start_read(lba, count, s->data);
		if (!wait)
			return;   /* one read per frame */
	}
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
/* Screen-space size (larger of width and height). */
static inline int extent(const DVECTOR *p, int n) {
	int xmin = p[0].vx, xmax = p[0].vx, ymin = p[0].vy, ymax = p[0].vy;
	for (int i = 1; i < n; i++) {
		if (p[i].vx < xmin) xmin = p[i].vx;
		if (p[i].vx > xmax) xmax = p[i].vx;
		if (p[i].vy < ymin) ymin = p[i].vy;
		if (p[i].vy > ymax) ymax = p[i].vy;
	}
	return xmax - xmin > ymax - ymin ? xmax - xmin : ymax - ymin;
}

/* Textured faces close to the camera are split until they are this small on
 * screen: the PS1 maps textures without perspective, so big ones warp. */
#define WARP_DIST   ZM(30)
#define WARP_SIZE   120

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

/* smooth: Gouraud fog per corner (the ground, where steps would show); else
 * one fog value per face (buildings: cheaper, and small on screen). */
static void emit(const Face *f, DVECTOR *p, int32_t *z, int u0, int v0, int u1, int v1, int tri, uint32_t *ot, int smooth) {
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
		setUV4(q, u0, v0, u1, v0, u0, v1, u1, v1);   /* the texture window picks the texture */
		setRGB0(q, f->r * light.r >> 7, f->g * light.g >> 7, f->b * light.b >> 7);
		q->tpage = tpage;
		q->clut = clut;
		addPrim(ot, q);
		prim += sizeof(POLY_FT4);
		/* Texture window: this face's 64 x 64 texture repeats across it (UVs
		 * up to 255). Added after the face, so drawn just before it. */
		DR_TWIN *w = (DR_TWIN *)prim;
		RECT win = { tu >> 3, tv >> 3, 24, 24 };   /* offset, mask: 8-pixel units */
		setTexWindow(w, &win);
		addPrim(ot, w);
		prim += sizeof(DR_TWIN);
	} else {
		uint8_t c[3], c0[3], c1[3], c2[3];
		base_colour(f, c);
		if (!smooth) {
			int zavg = tri ? (z[0] + z[1] + z[2]) / 3 : (z[0] + z[1] + z[2] + z[3]) >> 2;
			fogged(c0, c, zavg);
			if (tri) {
				POLY_F3 *q = (POLY_F3 *)prim;
				setPolyF3(q);
				setXY3(q, p[0].vx, p[0].vy, p[1].vx, p[1].vy, p[2].vx, p[2].vy);
				setRGB0(q, c0[0], c0[1], c0[2]);
				addPrim(ot, q);
				prim += sizeof(POLY_F3);
			} else {
				POLY_F4 *q = (POLY_F4 *)prim;
				setPolyF4(q);
				setXY4(q, p[0].vx, p[0].vy, p[1].vx, p[1].vy, p[2].vx, p[2].vy, p[3].vx, p[3].vy);
				setRGB0(q, c0[0], c0[1], c0[2]);
				addPrim(ot, q);
				prim += sizeof(POLY_F4);
			}
			faces_drawn++;
			return;
		}
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

/* Depth-sorted, also the ground (hills hide what's behind them). Ground faces
 * sort by their farthest corner so they go under what stands on them; areas,
 * footways and roads by their middle, a step nearer per layer so they go on
 * top of the ground. */
static inline uint32_t *ot_slot(const Face *f, int flat, int32_t *z, int n) {
	int sum = 0, zmax = 0;
	for (int i = 0; i < n; i++) {
		sum += z[i];
		if (z[i] > zmax) zmax = z[i];
	}
	int avg = n == 4 ? sum >> 2 : sum / 3;
	int layer = f->texlayer >> 4;
	int otz = flat ? ((layer == 0 ? zmax : avg) >> 4) + 3 - layer : avg >> 4;
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
	if (ok && depth < 2 && (f->texlayer & 15) < NUM_TEXTURES && z[0] < WARP_DIST && extent(p, 4) > WARP_SIZE)
		ok = 0;   /* close and big: split to keep the texture from warping */
	if (ok) {
		if (!front_facing(p[0], p[1], p[2]) && !front_facing(p[1], p[3], p[2]))
			return;
		emit(f, p, z, u0, v0, u1, v1, 0, ot_slot(f, flat, z, 4), flat);
		return;
	}
	if (depth >= 4)
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

/* Vertices first .. first + n into the screen-space cache. */
static void transform(const SVECTOR *v, int first, int n) {
	stat_verts += n;
	int i = first, end = first + n;
	for (; i + 3 <= end; i += 3) {
		gte_ldv3(&v[i], &v[i + 1], &v[i + 2]);
		gte_rtpt();
		gte_stsxy3(&sxy[i], &sxy[i + 1], &sxy[i + 2]);
		gte_stsz3(&sz[i], &sz[i + 1], &sz[i + 2]);
	}
	for (; i < end; i++) {
		gte_ldv0(&v[i]);
		gte_rtps();
		gte_stsxy(&sxy[i]);
		gte_stsz(&sz[i]);
	}
}

static void draw_faces(const Face *f, int n, const SVECTOR *v, int flat) {
	stat_faces_seen += n;
	for (int k = 0; k < n; k++, f++) {
		/* One pass, cheapest rejections first: every read is a trip to main
		 * RAM on the PS1 (no data cache). */
		int tri = f->v[3] == 0xffff;
		int i0 = f->v[0], i1 = f->v[1], i2 = f->v[2], i3 = tri ? i2 : f->v[3];
		int32_t z[4] = { sz[i0], sz[i1], sz[i2], sz[i3] };
		int nearc = (z[0] < NEAR_Z) + (z[1] < NEAR_Z) + (z[2] < NEAR_Z) + (z[3] < NEAR_Z);
		if (nearc == 4 || (z[0] > FOG_END && z[1] > FOG_END && z[2] > FOG_END && z[3] > FOG_END))
			continue;
		if (nearc) {
			SVECTOR c[4] = { v[i0], v[i1], v[i2], v[i3] };
			draw_split(f, flat, c, f->u0, f->v0, f->u1, f->v1, 0);
			continue;
		}
		DVECTOR p[4] = { sxy[i0], sxy[i1], sxy[i2], sxy[i3] };
		/* Facing away: the back of walls and roofs, slopes beyond a hilltop. */
		if (!front_facing(p[0], p[1], p[2]) && (tri || !front_facing(p[1], p[3], p[2])))
			continue;
		int xmin = p[0].vx, xmax = p[0].vx, ymin = p[0].vy, ymax = p[0].vy;
		for (int j = 1; j < 4; j++) {
			if (p[j].vx < xmin) xmin = p[j].vx;
			if (p[j].vx > xmax) xmax = p[j].vx;
			if (p[j].vy < ymin) ymin = p[j].vy;
			if (p[j].vy > ymax) ymax = p[j].vy;
		}
		if (xmax < 0 || xmin >= 320 || ymax < 0 || ymin >= 240)
			continue;   /* off screen */
		int w = xmax - xmin, h = ymax - ymin;
		if (xmin <= -1000 || xmax >= 1000 || ymin <= -1000 || ymax >= 1000 || w > 1000 || h > 500 ||
		    (!tri && (f->texlayer & 15) < NUM_TEXTURES && z[0] < WARP_DIST && (w > WARP_SIZE || h > WARP_SIZE))) {
			/* Too big for the GPU, or close and big enough for the texture to warp. */
			SVECTOR c[4] = { v[i0], v[i1], v[i2], v[i3] };
			draw_split(f, flat, c, f->u0, f->v0, f->u1, f->v1, 0);
			continue;
		}
		emit(f, p, z, f->u0, f->v0, f->u1, f->v1, tri, ot_slot(f, flat, z, tri ? 3 : 4), flat);
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

	const SVECTOR *v = s->verts;
	transform(v, 0, h->nflatverts);
	draw_faces(s->faces, h->nflat, v, 1);

	/* Buildings: one bounding-sphere test each (camera space, unscaled), then
	 * fine faces if any part is within texture distance, else coarse ones. */
	const Group *g = s->groups;
	stat_groups_seen += h->ngroups;
	if (!s->heights) stat_far_tiles++;
	for (int i = 0; i < h->ngroups; i++, g++) {
		int32_t x = ox + g->cx - cam->x, y = ((g->top + g->bottom) >> 1) - cam->y, z = oz + g->cz - cam->z;
		int32_t r = g->r + ((g->bottom - g->top) >> 1);   /* sphere around the box, roughly */
		int32_t cz = (cam->m.m[2][0] * x + cam->m.m[2][1] * y + cam->m.m[2][2] * z) >> 12;
		if (cz + r < NEAR_Z / GTE_SCALE || (cz - r) * GTE_SCALE > FOG_END)
			continue;
		int32_t cx = (cam->m.m[0][0] * x + cam->m.m[0][1] * y + cam->m.m[0][2] * z) >> 12;
		/* Outside the sides of the view (half-width 0.8, sphere: r * sqrt(1 + 0.64)). */
		if (cx * 5 > cz * 4 + r * 7 || -cx * 5 > cz * 4 + r * 7)
			continue;
		stat_groups_drawn++;
		transform(v, g->vfirst, g->vcount);   /* only buildings we draw */
		/* The roof only shows from above it (e.g. from the hills). */
		int roof = cam->y < g->top ? 0 : g->nroof;
		if ((cz - r) * GTE_SCALE < TEX_DIST || !g->ncoarse)
			draw_faces(s->faces + g->first, g->nfine - roof, v, 0);
		else
			draw_faces(s->faces + g->first + g->nfine, g->ncoarse - roof, v, 0);
	}
}

uint8_t *world_draw(const Camera *cam, uint32_t *ot, uint8_t *prims, uint8_t *prims_end) {
	prim = prims;
	prim_end = prims_end;
	ot_base = ot;
	tiles_drawn = 0;
	faces_drawn = 0;
	stat_faces_seen = stat_groups_seen = stat_groups_drawn = stat_far_tiles = stat_verts = 0;
	for (int i = 0; i < NUM_NEAR; i++)
		if (near_slots[i].state == SLOT_READY)
			draw_tile(cam, &near_slots[i]);
	/* Far tiles, unless the detailed copy is in (looked up in a small grid). */
	uint8_t has_near[2 * FAR_RADIUS + 1][2 * FAR_RADIUS + 1] = { { 0 } };
	int ctx = cam->x / TILE_UNITS, ctz = cam->z / TILE_UNITS;
	for (int i = 0; i < NUM_NEAR; i++) {
		int dx = near_slots[i].tx - ctx + FAR_RADIUS, dz = near_slots[i].tz - ctz + FAR_RADIUS;
		if (near_slots[i].state == SLOT_READY && dx >= 0 && dz >= 0 && dx <= 2 * FAR_RADIUS && dz <= 2 * FAR_RADIUS)
			has_near[dz][dx] = 1;
	}
	for (int i = 0; i < NUM_FAR; i++) {
		Slot *s = &far_slots[i];
		int dx = s->tx - ctx + FAR_RADIUS, dz = s->tz - ctz + FAR_RADIUS;
		if (s->state != SLOT_READY || dx < 0 || dz < 0 || dx > 2 * FAR_RADIUS || dz > 2 * FAR_RADIUS || has_near[dz][dx])
			continue;
		draw_tile(cam, s);
	}
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

int32_t world_ground_y(int32_t x, int32_t z) {
	if (x < 0 || z < 0)
		return 0;
	int tx = x / TILE_UNITS, tz = z / TILE_UNITS;
	Slot *s = near_ready(tx, tz);
	if (!s || !s->heights)
		return 0;
	/* 8 m = 128 units per grid step; bilinear. */
	int32_t lx = x - tx * TILE_UNITS, lz = z - tz * TILE_UNITS;
	int gx = lx >> 7, gz = lz >> 7, fx = lx & 127, fz = lz & 127;
	if (gx > 7) gx = 7, fx = 128;
	if (gz > 7) gz = 7, fz = 128;
	const int16_t *h = s->heights + gz * 9 + gx;
	int32_t a = h[0] + ((h[1] - h[0]) * fx >> 7);
	int32_t b = h[9] + ((h[10] - h[9]) * fx >> 7);
	return a + ((b - a) * fz >> 7);
}

int world_collide(int32_t *x, int32_t *z, int32_t radius, int32_t *nx, int32_t *nz) {
	int hit = 0;
	int ctx = (*x >> 4) / TILE_UNITS, ctz = (*z >> 4) / TILE_UNITS;
	*nx = *nz = 0;
	for (int i = 0; i < NUM_NEAR; i++) {
		Slot *s = &near_slots[i];
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
