/*
 * Low-poly figures made in code (boxes, prisms, cones) and posed by turning
 * their parts: the soul and Sloth's husk, and the sunken statues around
 * the floor.
 *
 * Model space: units (a tile is 256), x right, y down (negative = up), z
 * forward (the way the figure faces), feet at y = 0.
 */
#pragma once
#include <stdint.h>

#define MAX_PARTS 5

typedef struct {
	int16_t x, y, z;
} Vec3;

typedef struct {
	uint8_t v[4];            /* v[3] = 255: triangle */
	uint8_t r, g, b;         /* textured: a tint, 128 = the texture as is */
	uint8_t glow;            /* not shaded by the light (eyes) */
	int8_t mat;              /* MAT_* (figtex.h), MAT_FLAT: plain colour */
	uint8_t smooth;          /* lit by the vertex normals (rounded) */
	int16_t nx, ny, nz;      /* outward normal, 4096 = 1 */
} MFace;

typedef struct {
	const Vec3 *verts;
	const Vec3 *normals;     /* per vertex, averaged over its smooth faces */
	const MFace *faces;
	int nverts, nfaces;
	int16_t px, py, pz;      /* pivot */
	int8_t parent;           /* part it hangs from (turns with it), -1: none */
} Part;

typedef struct {
	Part parts[MAX_PARTS];
	int nparts;
} Model;

/* A pose: each part turned about its pivot, pitch (about x, positive =
 * forward/down) then yaw, on top of its parent's turn; and the whole model
 * moved. */
typedef struct {
	int16_t pitch[MAX_PARTS], yaw[MAX_PARTS];
	int16_t dy;               /* whole model up/down */
	int16_t lean;             /* whole model pitch */
} Pose;

enum { SOUL_BODY, SOUL_HEAD, SOUL_SWORD_ARM, SOUL_ARM };
enum { HUSK_BODY, HUSK_HEAD, HUSK_RIGHT_ARM, HUSK_LEFT_ARM, HUSK_LEGS };
enum { BELL_BODY, BELL_HEAD, BELL_ARM, BELL_OTHER_ARM };

extern Model soul_model, husk_model, bell_model, unfinished_model, head_statue, hand_statue, pillar_statue;
/* The Unfinished's parts are in the husk's order (it moves like one). */

void models_init(void);
