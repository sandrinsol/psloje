/*
 * GRID SOULS: the trial's rules. Everything runs in 1/60 s frames.
 *
 * The floor is an empty N x N grid (x right, y down, 0..N-1). The player
 * only moves by dodging (Circle) one tile; the D-pad turns. Enemies light
 * up the tiles they're about to hit: faint first, bright just before.
 */
#pragma once
#include <stdint.h>

#define MAX_N        9
#define MAX_ENEMIES  6

/* Directions: also the facing. */
enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

/* Player states. */
enum { P_IDLE, P_DODGE, P_ATTACK, P_DRINK, P_STAGGER, P_DEAD };

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
	int32_t prev_facing, turned_dir; /* the last turn, while its direction is held (-1: none) */
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

/* Floor states. */
enum { F_WAVE_INTRO, F_FIGHT, F_CLEARED, F_DEAD };

void game_start_floor(int keep_drop);
void game_update(const Input *in);
/* 0..256 how bright tile (x, y)'s telegraph is (0 = none, 256 = about to land). */
int game_telegraph(int x, int y);
