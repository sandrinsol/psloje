/*
 * GRID SOULS: the trial's rules. Everything runs in 1/60 s frames.
 *
 * The floor is an empty N x N grid (x right, y down = towards the start,
 * 0..N-1; facing 0 = up the grid). The D-pad walks a tile at a time;
 * Circle (+ a direction) dodges, with invincible frames, for stamina. The
 * soul always faces the nearest enemy along a row or column. Enemies light
 * up the tiles they're about to hit: faint first, bright just before.
 */
#pragma once
#include <stdint.h>

#define MAX_N        9
#define MAX_ENEMIES  6

/* Directions: also the facing. */
enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

/* Player states. */
enum { P_IDLE, P_DODGE, P_ATTACK, P_DRINK, P_STAGGER, P_DEAD, P_WALK };

/* Pad input for one frame. */
typedef struct {
	uint16_t held, pressed;   /* PAD_* bits */
} Input;

typedef struct {
	int32_t x, y;           /* tile (the one being left until halfway through a dodge) */
	int32_t from_x, from_y; /* dodge start, for drawing */
	int32_t facing;
	int32_t state, t;       /* state and frames in it */
	int32_t hp, stamina;    /* 0..HP_MAX, 0..STAMINA_MAX * 16 */
	int32_t stamina_wait;   /* frames before stamina refills */
	int32_t flasks;
	int32_t blocking;
	int32_t parry_t;        /* frames since Square was pressed (parry window) */
	int32_t hurt_t;         /* frames since last hit (flash) */
	int32_t parried_t;      /* frames since the last successful parry */
	int32_t buffered, buffer_t; /* a dodge/attack pressed while busy */
	int32_t buffer_dir;
	int32_t dodge_dir;
} Player;

/* Enemy states. */
enum { E_NONE, E_IDLE, E_MOVE, E_WINDUP, E_RECOVER, E_STAGGER, E_DYING };

typedef struct {
	int32_t state, t;
	int32_t x, y, from_x, from_y;
	int32_t hp;
	int32_t windup, attack;     /* this attack's windup and pattern */
	uint8_t target[MAX_N * MAX_N];  /* tiles the attack will hit */
	int32_t hit_t;              /* frames since hit (flash) */
} Enemy;

/* A walking step, one tile (a dodge is 9 frames). */
#define WALK_FRAMES   20

#define HP_MAX        100
#define STAMINA_MAX   100

extern int32_t grid_n;
extern Player player;
extern Enemy enemies[MAX_ENEMIES];
extern volatile int32_t wave, enemies_alive, absolution, guilt, floor_state, game_frames;
extern volatile int32_t drop_x, drop_y, drop_amount;   /* absolution left on the floor (-1: none) */
extern int32_t message_t;
extern const char *message;
extern int32_t shake_t;
extern int32_t hitstop_t;   /* frames the game stays frozen (hit-stop) */
extern int32_t floor_t;     /* frames in the floor's current state */
extern int32_t parry_wide;  /* assist: a wider parry window */

/* What happened this frame, for sounds and effects. */
enum { EV_TURN, EV_STEP, EV_DODGE, EV_SWING, EV_HIT, EV_RIPOSTE, EV_KILL, EV_BLOCK, EV_PARRY, EV_HURT,
       EV_DEATH, EV_WINDUP, EV_SLAM, EV_HEAL, EV_RECOVER, EV_CLEAR };
typedef struct {
	int16_t type, x, y;   /* tile */
} GameEvent;
#define MAX_EVENTS 16
extern GameEvent game_events[MAX_EVENTS];
extern int32_t num_events;

/* Floor states. */
enum { F_WAVE_INTRO, F_FIGHT, F_CLEARED, F_DEAD };

void game_start_floor(int keep_drop);
void game_update(const Input *in);
/* 0..256 how bright tile (x, y)'s telegraph is (0 = none, 256 = about to land). */
int game_telegraph(int x, int y);
