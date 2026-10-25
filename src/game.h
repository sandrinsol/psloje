/*
 * GRID SOULS: the trial's rules. Everything runs in 1/60 s frames.
 *
 * The floor is an empty N x N grid (x right, y down = towards the start,
 * 0..N-1; facing 0 = up the grid). The D-pad walks a tile at a time;
 * Circle (+ a direction) dodges, with invincible frames, for stamina; Cross
 * jumps in place, over the blows that hit the ground; Square attacks, held
 * it charges a heavy blow, which breaks a husk (staggers it): the next hit on
 * a broken husk is a riposte, triple damage. The soul always faces the nearest enemy along a
 * row or column. Enemies light up the tiles they're about to hit: faint
 * first, bright just before.
 */
#pragma once
#include <stdint.h>

#define MAX_N        9
#define MAX_ENEMIES  6

/* Directions: also the facing. */
enum { DIR_UP, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

/* Player states. */
enum { P_IDLE, P_DODGE, P_ATTACK, P_DRINK, P_CHARGE, P_DEAD, P_WALK, P_JUMP, P_FALL };

/* Pad input for one frame. */
typedef struct {
	uint16_t held, pressed;   /* PAD_* bits */
} Input;

typedef struct {
	int32_t x, y;           /* tile (a dodge's destination from its start) */
	int32_t from_x, from_y; /* dodge start, for drawing */
	int32_t facing;
	int32_t state, t;       /* state and frames in it */
	int32_t hp, stamina;    /* 0..HP_MAX, 0..STAMINA_MAX * 16 */
	int32_t stamina_wait;   /* frames before stamina refills */
	int32_t flasks;
	int32_t heavy;          /* this swing is a charged, heavy one */
	int32_t hurt_t;         /* frames since last hit (flash) */
	int32_t buffered, buffer_t; /* a dodge/attack/jump pressed while busy */
	int32_t buffer_dir;
	int32_t dodge_dir;
	int32_t lock;           /* the enemy faced (R2 picks the next), -1: the nearest */
} Player;

/* Enemy states. */
enum { E_NONE, E_IDLE, E_MOVE, E_WINDUP, E_RECOVER, E_STAGGER, E_DYING, E_SLEEP, E_WAKE, E_RISE };
#define RISE_FRAMES  50   /* an enemy rising out of the slabs (ash into shape) */
/* Kinds: Sloth's husk; a husk asleep until you come near; the bell-ringer,
 * who keeps to the floor's edge and rings a whole row or column down. */
enum { K_HUSK, K_SLEEPER, K_BELL, K_UNFINISHED };
#define BOSS_HP      400
#define BOSS_INTRO   300   /* frames of the boss's entrance */

typedef struct {
	int32_t state, t;
	int32_t x, y, from_x, from_y;
	int32_t hp;
	int32_t windup, attack;     /* this attack's windup and pattern */
	uint8_t target[MAX_N * MAX_N];  /* tiles the attack will hit */
	int32_t hit_t;              /* frames since hit (flash) */
	int32_t kind;               /* K_* */
} Enemy;

/* A walking step, one tile (a dodge is 9 frames). */
#define WALK_FRAMES   20
#define DODGE_FRAMES  12   /* a dodge: two tiles (over an enemy too) */
#define DODGE_TILES   2
#define IFRAME_FIRST  2    /* invincible frames 2..9 of the dodge */
#define IFRAME_LAST   9
/* A jump: off the ground (attacks on the floor miss) from AIR_FIRST to
 * AIR_LAST. */
#define JUMP_FRAMES   28
#define AIR_FIRST     3
#define AIR_LAST      21
/* Square held this long: the heavy blow is ready. */
#define CHARGE_FULL   30

/* Walked or dodged off the edge: falling, then gone (a death). */
#define FALL_FRAMES   55

#define HP_MAX        100
#define STAMINA_MAX   100

extern int32_t grid_n;
/* The part of the grid still standing: after each wave an edge row and
 * column crumble and fall (9 x 9, 8 x 8, 7 x 7, the boss on 6 x 6). */
extern int32_t area_x0, area_y0, area_x1, area_y1;   /* inclusive */
extern Player player;
extern Enemy enemies[MAX_ENEMIES];
extern volatile int32_t wave, enemies_alive, absolution, guilt, floor_state, game_frames;
extern volatile int32_t drop_x, drop_y, drop_amount;   /* absolution left on the floor (-1: none) */
extern int32_t message_t;
extern const char *message;
extern int32_t shake_t;
extern int32_t hitstop_t;   /* frames the game stays frozen (hit-stop) */
extern int32_t floor_t;     /* frames in the floor's current state */

/* What happened this frame, for sounds and effects. */
enum { EV_TURN, EV_STEP, EV_DODGE, EV_SWING, EV_HIT, EV_HEAVY, EV_KILL, EV_JUMP, EV_CHARGED, EV_HURT,
       EV_DEATH, EV_WINDUP, EV_SLAM, EV_HEAL, EV_RECOVER, EV_CLEAR, EV_LAND, EV_RIPOSTE, EV_RISE, EV_WAKE, EV_BELL, EV_BOSS, EV_TRIAL, EV_FALL, EV_CRUMBLE, EV_PERFECT, EV_SPAWN, EV_LOCK };   /* EV_TRIAL: x = the trial met */   /* EV_BOSS: the boss's name is carved */
typedef struct {
	int16_t type, x, y;   /* tile */
} GameEvent;
#define MAX_EVENTS 16
extern GameEvent game_events[MAX_EVENTS];
extern int32_t num_events;

/* Floor states. */
enum { F_WAVE_INTRO, F_FIGHT, F_CLEARED, F_DEAD, F_BOSS_INTRO };
extern int32_t boss;   /* the boss's enemy slot, -1: none (yet) */

/* This trial's numbers (from NEW TRIAL; dying doesn't reset them). */
typedef struct {
	int32_t frames;     /* time on the floor, not counting the death screen */
	int32_t deaths, kills, ripostes, jumped, hurt, flasks;
} Stats;
extern Stats stats;

/* Trials: feats, met once and kept (on the memory card). */
enum {
	TRIAL_RIPOSTE, TRIAL_SLEEP, TRIAL_BELL, TRIAL_AIR, TRIAL_BOSS,
	TRIAL_UNBROKEN, TRIAL_DRY, TRIAL_SWIFT, TRIAL_UNTOUCHED, NUM_TRIALS
};
#define TRIAL_AIR_COUNT  10            /* blows jumped over in one trial */
#define TRIAL_SWIFT_TIME (5 * 60 * 60) /* frames */
extern uint32_t trials;   /* bits met so far (set from the save) */

void game_start_floor(int keep_drop);
void game_update(const Input *in);
/* 0..256 how bright tile (x, y)'s telegraph is (0 = none, 256 = about to land). */
int game_telegraph(int x, int y);
/* A slab's state: 0 standing, < 0 crumbling (frames until it falls),
 * > 0 falling (frames since), or gone once out of the area and settled. */
int game_slab(int x, int y);
/* The last death was a fall (nothing left to draw of the soul). */
int game_fell(void);
/* The tile the soul is on now (passing over others while it dodges). */
void game_player_tile(int *x, int *y);
