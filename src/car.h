/*
 * The taxi: semi-arcade driving physics and its model.
 *
 * Position and velocity are in 1/65536 m (velocity per 1/60 s tick); heading
 * is 4096 per turn << 8 (0 = north, clockwise).
 */
#pragma once
#include <stdint.h>
#include "world.h"

typedef struct {
	int32_t x, z;        /* 1/65536 m */
	int32_t vx, vz;      /* 1/65536 m per tick */
	int32_t heading;     /* 4096 per turn << 8 */
	int32_t steer;       /* -4096 .. 4096 */
	int32_t speed;       /* forward speed, 1/65536 m per tick (negative = reversing) */
	int32_t bumps;       /* wall hits, for damage later */
	int32_t y;           /* ground height, world units (negative = up) */
	int32_t pitch;       /* nose up, 4096 per turn */
	int32_t throttle;    /* 0 .. 4096: the pedal, pressed gradually */
} Car;

typedef struct {
	int gas, brake, handbrake;  /* 0 / 1 */
	int steer;                  /* -4096 .. 4096 */
	int analog;                 /* steer came from a stick: no ramping */
} CarInput;

#define CAR_M          65536            /* car units per metre */
#define CAR_TO_WORLD(v) ((v) >> 12)     /* -> 1/16 m */

void car_reset(Car *car, int32_t world_x, int32_t world_z, int heading);
void car_tick(Car *car, const CarInput *in);
int car_kmh(const Car *car);
/* Puts the chase camera behind the car. snap = jump there instead of easing. */
void car_camera(const Car *car, Camera *cam, int snap);
uint8_t *car_draw(const Car *car, const Camera *cam, uint32_t *ot, uint8_t *prims);
