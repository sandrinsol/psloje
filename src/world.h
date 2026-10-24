/*
 * The city: tiles streamed from MAP.BIN on the CD (made by tools/osm2map.mjs),
 * drawn with the GTE, and the collision edges of its buildings.
 *
 * Units: 1 world unit = 1/16 m. X = east, Z = north, Y = down.
 */
#pragma once
#include <stdint.h>
#include <psxgte.h>

#define UNIT             16          /* world units per metre */
#define TILE_UNITS       1024        /* 64 m */
#define MAX_TILE_VERTS   2400        /* must match tools/osm2map.mjs */
#define TILE_SLOT_BYTES  (40 * 1024) /* must be >= the biggest tile */
#define LOAD_RADIUS      2           /* tiles kept around the car: 5 x 5 */

#define OT_LEN           2048
/* The GTE gets the camera matrix scaled by GTE_SCALE: the picture is the same
 * but depths (SZ) are GTE_SCALE times bigger. Needed because the GTE's divide
 * overflows when SZ < H/2 (H = 200): points closer than that are squashed.
 * Depth constants below are in those scaled units. */
#define GTE_SCALE        7
#define ZM(metres)       ((metres) * UNIT * GTE_SCALE)
#define NEAR_Z           104         /* > H/2: about 0.93 m */
#define TEX_DIST         ZM(50)      /* textured closer than this */
#define FOG_START        ZM(55)
#define FOG_END          ZM(120)

typedef struct {
	int32_t x, y, z;   /* world units */
	int heading;       /* 4096 = full turn, 0 = north, clockwise */
	int pitch;         /* positive = looking down */
	MATRIX m;          /* world -> camera rotation (set by camera_update) */
	MATRIX gm;         /* the same, times GTE_SCALE, for the GTE */
} Camera;

typedef struct {
	uint8_t r, g, b;
} Colour;

/* A place passengers go to (on the nearest road), from OSM. */
typedef struct {
	int32_t x, z;       /* world units */
	uint16_t name;
	uint8_t kind;       /* PLACE_* */
	uint8_t pad;
} Place;
#define PLACE_DESTINATION 0
#define PLACE_FUEL        1
#define PLACE_REPAIR      2

/* A piece (<= 32 m) of drivable road, for the minimap and street names. */
typedef struct {
	int16_t x0, z0, x1, z1;   /* metres */
	uint16_t name;            /* 0xffff = none */
	uint8_t cls;              /* 0 minor, 1 secondary/tertiary, 2 primary+ */
	uint8_t pad;
} RoadSeg;

extern const Place *places;
extern int num_places;

extern Colour fog_colour;
extern Colour light;       /* scene light, 128 = full daylight (day/night) */
extern char map_name[32];
extern volatile int32_t tiles_loaded, tiles_drawn, faces_drawn, cd_errors;

/* Reads the map header from the CD; returns 0 if MAP.BIN is missing. */
int world_init(void);
/* Name by id (upper case ASCII), "" if none. */
const char *world_name(int id);
/* Name of the street nearest to (x, z) within 25 m, or "". */
const char *world_street_at(int32_t x, int32_t z);
/* Calls fn for every road piece in the tiles within radius_tiles of (x, z). */
void world_segments(int32_t x, int32_t z, int radius_tiles, void (*fn)(const RoadSeg *, void *), void *arg);
/* Spawn point from the map (world units, heading 0..4095). */
void world_spawn(int32_t *x, int32_t *z, int *heading);
/* Map extent in world units. */
void world_size(int32_t *w, int32_t *h);
/* Keeps the tiles around (x, z) loaded: one CD read at a time, non-blocking.
 * With wait != 0 it blocks until all of them are in (start of a level). */
void world_stream(int32_t x, int32_t z, int wait);
void camera_update(Camera *cam);
/* Loads a rotation + translation into the GTE (see world.c for why not the macros). */
void gte_load_matrix(const MATRIX *m);
/* Adds the visible city to the ordering table. Returns the new prim pointer. */
uint8_t *world_draw(const Camera *cam, uint32_t *ot, uint8_t *prims, uint8_t *prims_end);
/* Ground Y (world units, negative = up) at (x, z), from the loaded tiles. */
int32_t world_ground_y(int32_t x, int32_t z);
/* Pushes a circle (centre in 1/256 m, radius in 1/256 m) out of building
 * walls. Returns 1 if it touched one; *nx, *nz = the push direction (4096 = 1). */
int world_collide(int32_t *x, int32_t *z, int32_t radius, int32_t *nx, int32_t *nz);
