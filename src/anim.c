/*
 * The figures' key poses (see anim.h). Angles: 4096 = a full turn; pitch
 * positive swings a part forward / down, an arm at -2048 points straight
 * up. Parts hang from their parents, so an arm's angle is on top of the
 * body's.
 *
 * Soul parts: body, head (hood), sword arm, other arm.
 * Husk parts: body, head, right arm, left arm (and the legs, which keep
 * still).
 */
#include <stdint.h>
#include "anim.h"

#define K(t, p0, p1, p2, p3, y0, y1, y2, y3, dy, lean) \
	{ t, { p0, p1, p2, p3 }, { y0, y1, y2, y3 }, dy, lean }
#define N(a) (sizeof(a) / sizeof(a[0]))

/* ---- The soul ------------------------------------------------------------------------ */

/* Standing: breathing, the sword hanging a little forward. */
static const Key SOUL_IDLE[] = {
	K(0,     0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
	K(128,  30,  -50,  430,  160,   0,  60, -150,   0,   2,  20),
	K(256,   0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
};
/* One step (a tile): a bob and the free arm swinging. */
static const Key SOUL_WALK[] = {
	K(0,    40,    0,  380,  220,   0,   0, -120,   0,   0,  60),
	K(128,  60,   40,  300,  -60, -80, -40, -120,   0, -12,  80),
	K(256,  40,    0,  380,  220,   0,   0, -120,   0,   0,  60),
};
/* A low, fast lunge to the side. */
static const Key SOUL_DODGE[] = {
	K(0,   200,  100,  600,  600,   0,   0, -200,   0,  10, 200),
	K(90,  450,  200,  950,  950,   0,   0, -300,   0, -30, 500),
	K(200, 300,  100,  700,  700,   0,   0, -200,   0,   4, 300),
	K(256,  60,    0,  420,  200,   0,   0, -120,   0,   0,  60),
};
/* Crouch, spring up with the arms thrown up, land in a crouch. Off the
 * ground on frames 3..21 of 28. */
static const Key SOUL_JUMP[] = {
	K(0,   100,    0,  380,  120,   0,   0, -120,   0,   0,  80),
	K(27,  220,   80,  520,  420,   0,   0, -150,   0,  16, 160),
	K(70, -100, -120, -300, -700,   0,   0, -200,   0,-150,-100),
	K(110, -60,  -80, -200, -600,   0,   0, -200,   0,-170, -60),
	K(185, 100,    0,  200,  150,   0,   0, -150,   0, -12,  60),
	K(212, 260,  100,  520,  520,   0,   0, -150,   0,  18, 180),
	K(256,   0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
};
/* Drawing the sword back over the shoulder (held at the end). */
static const Key SOUL_CHARGE[] = {
	K(0,     0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
	K(110, -60,  -40,-1400,  300, -80,  60, -350,   0,   6,-100),
	K(256,-120,  -80,-2300,  420,-160, 120, -500,   0,  10,-200),
};
/* Light swing: up, across and down on frame 8 of 22, back. */
static const Key SOUL_ATTACK[] = {
	K(0,     0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
	K(64, -100,  -60,-1500,  300, -60,  40, -300,   0,   0,-120),
	K(93,  250,   80, 1300,    0,  80, -40,  300,   0,   6, 220),
	K(150, 220,   60, 1200,   60,  60, -40,  260,   0,   4, 180),
	K(256,   0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
};
/* Heavy blow: from the charged pose, all the way down on frame 6 of 30. */
static const Key SOUL_HEAVY[] = {
	K(0,  -120,  -80,-2300,  420,-160, 120, -500,   0,  10,-200),
	K(44, -170, -110,-2550,  420,-210, 140, -500,   0,   8,-270),   /* held a moment longer */
	K(64,  350,  120, 1700,  100, 120, -60,  300,   0,  16, 420),   /* lands on frame 8 of 32 */
	K(128, 330,  100, 1650,  100, 100, -60,  280,   0,  14, 380),
	K(256,   0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
};
/* The flask to the lips, the head tipped back. */
static const Key SOUL_DRINK[] = {
	K(0,     0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
	K(70,  -40, -260,  420, 2200,   0,   0, -120, -200,   0, -80),
	K(170, -60, -320,  420, 2300,   0,   0, -120, -200,   0, -90),
	K(256,   0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
};
/* Struck down: a stagger back, then folding forward onto the slab. */
static const Key SOUL_DEAD[] = {
	K(0,     0,    0,  380,  120,   0,   0, -120,   0,   0,   0),
	K(40,  -80, -200,  600,  500,   0,  80, -120,   0,   0,-180),
	K(128, 300,  400, 1000,  900,   0, 120, -120,   0,  24, 600),
	K(256, 420,  500, 1100, 1000,   0, 160, -120,   0,  34, 980),
};

/* ---- Sloth's husk ---------------------------------------------------------------------- */

/* Hunched, the arms hanging straight down (against the body's lean), the
 * head held up to look ahead. */
static const Key HUSK_IDLE[] = {
	K(0,   260, -220, -260, -260,   0,   0,   0,   0,   0,   0),
	K(128, 300, -170, -230, -290,   0, 120,   0,   0,   4,   0),
	K(256, 260, -220, -260, -260,   0,   0,   0,   0,   0,   0),
};
/* A lumbering tile: two heavy steps, swaying, the arms dragging. */
static const Key HUSK_MOVE[] = {
	K(0,   300, -240, -200, -330, -60,  40,   0,   0,   0,   0),
	K(64,  330, -260, -150, -390,-100,  60,   0,   0,  -8,  20),
	K(128, 300, -240, -330, -200,  60, -40,   0,   0,   0,   0),
	K(192, 330, -260, -390, -150, 100, -60,   0,   0,  -8,  20),
	K(256, 300, -240, -200, -330, -60,  40,   0,   0,   0,   0),
};
/* The windup (over however long it is): gathering, then rearing up with the
 * arms over the head, the head thrown back. */
static const Key HUSK_WINDUP[] = {
	K(0,   260, -220, -260, -260,   0,   0,   0,   0,   0,   0),
	K(40,  340, -280, -120, -120,   0,   0,   0,   0,   6,  20),
	K(200,-120,  120,-2350,-2350,   0,   0,   0,   0,  -6, -60),
	K(256,-140,  150,-2450,-2450,   0,   0,   0,   0,  -8, -70),
};
/* After the blow: arms smashed down in front, slowly hauled back up. */
static const Key HUSK_RECOVER[] = {
	K(0,   380, -150,  420,  420,   0,   0,   0,   0,  12,  50),
	K(40,  400, -130,  460,  460,   0,   0,   0,   0,  14,  50),
	K(170, 330, -180,   60,   60,   0,  80,   0,   0,   4,  20),
	K(256, 260, -220, -260, -260,   0,   0,   0,   0,   0,   0),
};
/* Broken: reeling back, arms flung up, wobbling. */
static const Key HUSK_STAGGER[] = {
	K(0,  -180,  150, -350, -450,   0,   0,   0,   0,   0, -80),
	K(64, -230,  190, -450, -380,  60, 120,   0,   0,  -2,-110),
	K(128,-190,  140, -330, -480,   0,  40,   0,   0,   0, -90),
	K(192,-220,  180, -430, -400, -60,-100,   0,   0,  -2,-105),
	K(256,-180,  150, -350, -450,   0,   0,   0,   0,   0, -80),
};

/* Asleep: slumped forward over its knees, arms hanging, breathing slowly. */
static const Key HUSK_SLEEP[] = {
	K(0,   880, -150, -420, -460,   0,  60,   0,   0,  26,   0),
	K(128, 840, -130, -400, -440,   0,  60,   0,   0,  22,   0),
	K(256, 880, -150, -420, -460,   0,  60,   0,   0,  26,   0),
};
/* Waking: rearing up with a roar, arms flung up, then down into its stance. */
static const Key HUSK_WAKE[] = {
	K(0,   880, -150, -420, -460,   0,  60,   0,   0,  26,   0),
	K(100, -80,  320,-1700,-1700,   0,   0,   0,   0,  -6, -90),
	K(170, -60,  300,-1650,-1650,   0,   0,   0,   0,  -6, -80),
	K(256, 260, -220, -260, -260,   0,   0,   0,   0,   0,   0),
};

/* ---- The bell-ringer -------------------------------------------------------------------- */
/* Parts: body, hood, bell arm, other arm. */

/* Waiting, swaying, the bell held low. */
static const Key BELL_IDLE[] = {
	K(0,    60,  -60,  220,  120,   0,   0, -100,   0,   0,   0),
	K(128,  90,  -40,  260,  150,  40, 120, -120,   0,   3,  10),
	K(256,  60,  -60,  220,  120,   0,   0, -100,   0,   0,   0),
};
/* Gliding a tile along its edge. */
static const Key BELL_MOVE[] = {
	K(0,   120,  -80,  300,  160,   0,   0, -100,   0,   0,  50),
	K(128, 140,  -70,  360,  190,   0,  60, -100,   0,  -6,  60),
	K(256, 120,  -80,  300,  160,   0,   0, -100,   0,   0,  50),
};
/* Ringing: the bell held out and swung side to side (one toll a cycle). */
static const Key BELL_RING[] = {
	K(0,    40, -140,  900,  250,   0,   0, -500,   0,   0, -40),
	K(128,  40, -140, 1000,  250,   0,   0,  500,   0,  -2, -40),
	K(256,  40, -140,  900,  250,   0,   0, -500,   0,   0, -40),
};
/* After: the bell lowered, bowed. */
static const Key BELL_RECOVER[] = {
	K(0,   260,  120,  500,  200,   0,   0,    0,   0,   4,  60),
	K(256,  60,  -60,  220,  120,   0,   0, -100,   0,   0,   0),
};

const Anim anims[NUM_ANIMS] = {
	[A_SOUL_IDLE] = { SOUL_IDLE, N(SOUL_IDLE), 1 },
	[A_SOUL_WALK] = { SOUL_WALK, N(SOUL_WALK), 1 },
	[A_SOUL_DODGE] = { SOUL_DODGE, N(SOUL_DODGE), 0 },
	[A_SOUL_JUMP] = { SOUL_JUMP, N(SOUL_JUMP), 0 },
	[A_SOUL_CHARGE] = { SOUL_CHARGE, N(SOUL_CHARGE), 0 },
	[A_SOUL_ATTACK] = { SOUL_ATTACK, N(SOUL_ATTACK), 0 },
	[A_SOUL_HEAVY] = { SOUL_HEAVY, N(SOUL_HEAVY), 0 },
	[A_SOUL_DRINK] = { SOUL_DRINK, N(SOUL_DRINK), 0 },
	[A_SOUL_DEAD] = { SOUL_DEAD, N(SOUL_DEAD), 0 },
	[A_HUSK_IDLE] = { HUSK_IDLE, N(HUSK_IDLE), 1 },
	[A_HUSK_MOVE] = { HUSK_MOVE, N(HUSK_MOVE), 1 },
	[A_HUSK_WINDUP] = { HUSK_WINDUP, N(HUSK_WINDUP), 0 },
	[A_HUSK_RECOVER] = { HUSK_RECOVER, N(HUSK_RECOVER), 0 },
	[A_HUSK_STAGGER] = { HUSK_STAGGER, N(HUSK_STAGGER), 1 },
	[A_HUSK_SLEEP] = { HUSK_SLEEP, N(HUSK_SLEEP), 1 },
	[A_HUSK_WAKE] = { HUSK_WAKE, N(HUSK_WAKE), 0 },
	[A_BELL_IDLE] = { BELL_IDLE, N(BELL_IDLE), 1 },
	[A_BELL_MOVE] = { BELL_MOVE, N(BELL_MOVE), 1 },
	[A_BELL_RING] = { BELL_RING, N(BELL_RING), 1 },
	[A_BELL_RECOVER] = { BELL_RECOVER, N(BELL_RECOVER), 0 },
};

/* Smoothstep, k 0..256. */
static int ease(int k) {
	return k * k * (768 - 2 * k) >> 16;
}

static int mix(int a, int b, int k) {
	return a + (b - a) * k / 256;
}

void pose_blend(const Pose *a, const Pose *b, int k, Pose *out) {
	for (int i = 0; i < MAX_PARTS; i++) {
		out->pitch[i] = mix(a->pitch[i], b->pitch[i], k);
		out->yaw[i] = mix(a->yaw[i], b->yaw[i], k);
	}
	out->dy = mix(a->dy, b->dy, k);
	out->lean = mix(a->lean, b->lean, k);
}

void anim_sample(int anim, int t, int len, Pose *out) {
	const Anim *a = &anims[anim];
	if (len < 1) len = 1;
	if (a->loop) t %= len;
	if (t < 0) t = 0;
	int at = t >= len ? 256 : t * 256 / len;
	int i = 0;
	while (i + 1 < a->n && a->keys[i + 1].t <= at) i++;
	const Key *k0 = &a->keys[i], *k1 = i + 1 < a->n ? &a->keys[i + 1] : k0;
	int span = k1->t - k0->t, k = span > 0 ? ease((at - k0->t) * 256 / span) : 0;
	for (int p = 0; p < MAX_PARTS; p++) {
		out->pitch[p] = mix(k0->pitch[p], k1->pitch[p], k);
		out->yaw[p] = mix(k0->yaw[p], k1->yaw[p], k);
	}
	out->dy = mix(k0->dy, k1->dy, k);
	out->lean = mix(k0->lean, k1->lean, k);
}
