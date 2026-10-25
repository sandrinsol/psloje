/*
 * Keyframed animation for the figures: each animation is a few key poses
 * over its length, eased between (smoothstep); changing animation blends
 * from wherever the figure was.
 */
#pragma once
#include <stdint.h>
#include "models.h"

/* A key: when (0..256 through the animation), then each part's pitch and
 * yaw, the model's height (dy, negative = up) and lean. */
typedef struct {
	int16_t t;
	int16_t pitch[MAX_PARTS], yaw[MAX_PARTS];
	int16_t dy, lean;
} Key;

typedef struct {
	const Key *keys;
	uint8_t n;
	uint8_t loop;   /* wraps round; else holds the last key */
} Anim;

enum {
	A_SOUL_IDLE, A_SOUL_WALK, A_SOUL_DODGE, A_SOUL_JUMP, A_SOUL_CHARGE, A_SOUL_ATTACK,
	A_SOUL_HEAVY, A_SOUL_DRINK, A_SOUL_DEAD,
	A_HUSK_IDLE, A_HUSK_MOVE, A_HUSK_WINDUP, A_HUSK_RECOVER, A_HUSK_STAGGER, A_HUSK_SLEEP, A_HUSK_WAKE,
	A_BELL_IDLE, A_BELL_MOVE, A_BELL_RING, A_BELL_RECOVER,
	NUM_ANIMS
};
extern const Anim anims[NUM_ANIMS];

/* The pose t frames into an animation len frames long. */
void anim_sample(int anim, int t, int len, Pose *out);
/* Between two poses: k 0..256. */
void pose_blend(const Pose *a, const Pose *b, int k, Pose *out);
