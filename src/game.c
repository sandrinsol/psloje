/*
 * GRID SOULS: the trial's rules (see game.h). Floor 1, Sloth: a 9 x 9 grid,
 * three waves of husks: slow, heavy, hard-hitting, every blow telegraphed.
 */
#include <stdint.h>
#include <string.h>
#include <psxpad.h>
#include "game.h"

/* Player timing (frames) and costs. Stamina is kept x16 for slow refills. */
#define DODGE_COST      (25 * 16)
#define ATTACK_WINDUP   8      /* sword: hits on frame 8 */
#define ATTACK_FRAMES   22
#define ATTACK_COST     (12 * 16)
#define ATTACK_DAMAGE   20
#define HEAVY_WINDUP    8      /* charged: hits on frame 8 of the swing (a weightier release) */
#define HEAVY_FRAMES    32
#define HEAVY_COST      (30 * 16)
#define HEAVY_DAMAGE    50     /* and it breaks the husk (staggers it) */
#define RIPOSTE_FACTOR  3      /* any hit on a broken husk */
#define JUMP_COST       (20 * 16)
#define STAMINA_REGEN   13     /* /16 a frame: ~49 a second */
#define STAMINA_WAIT    15
#define DRINK_FRAMES    45
#define DRINK_HEAL_AT   30
#define FLASK_HEAL      40
#define FLASKS          3
#define BUFFER_FRAMES   12     /* a press while busy is kept this long */
#define PERFECT_FRAMES  6      /* a dodge this late before a blow lands: perfect */
#define PERFECT_STAMINA (30 * 16)
#define WAVE_HEAL       25     /* health back as a wave falls */
#define CRUMBLE_FRAMES  110    /* the edge cracks this long before it falls */
#define BREATHER        220    /* between waves: the edge falls, then the next rise */
#define LIGHT_CANCEL    (ATTACK_WINDUP + 6)   /* a swing's recovery can be cut by a dodge or jump from here */
#define HEAVY_CANCEL    (HEAVY_WINDUP + 12)

/* Sloth's husk. */
#define HUSK_HP         60
#define HUSK_DAMAGE     30
#define HUSK_IDLE       50
#define HUSK_MOVE       36
#define HUSK_RECOVER    45
#define HUSK_STAGGER    60
#define HUSK_DYING      30
#define HUSK_REWARD     40
#define WAKE_RANGE      2      /* a sleeper wakes when you come this near (or strike it) */
#define WAKE_FRAMES     45
#define SLEEP_FACTOR    2      /* a blow on a sleeper */
#define BELL_HP         40
#define BELL_IDLE       40
#define BELL_MOVE       30
#define BELL_WINDUP     80     /* four tolls */
#define BELL_RECOVER    50

/* The Unfinished. */
#define BOSS_DAMAGE     35
#define BOSS_IDLE       44     /* between blows; faster below half its life */
#define BOSS_IDLE_ANGRY 26
#define BOSS_MOVE       40
#define BOSS_RECOVER    40
#define BOSS_REWARD     200
#define BRIGHT_FRAMES   15     /* the last frames of a windup: bright */

enum { ATK_SLAM, ATK_LINE, ATK_CROSS, ATK_RING, ATK_UNDONE, ATK_UNDONE2, ATK_TOMORROW, ATK_TOMORROW2, ATK_ROPE,
       ATK_UNRAVEL, ATK_UNRAVEL2, ATK_LOOSE, ATK_LOOSE2 };

int32_t grid_n = 9;
int32_t area_x0, area_y0, area_x1 = 8, area_y1 = 8;
static int16_t slab_t[MAX_N][MAX_N];   /* see game_slab() */
static int fell;                        /* the last death was a fall */
static int fall_x, fall_y;              /* the last solid tile before it */
Player player;
Enemy enemies[MAX_ENEMIES];
volatile int32_t wave, enemies_alive, absolution, guilt, floor_state, game_frames;
volatile int32_t drop_x = -1, drop_y = -1, drop_amount;
int32_t message_t;
const char *message;
int32_t shake_t;
int32_t hitstop_t;
GameEvent game_events[MAX_EVENTS];
int32_t num_events;
int32_t floor_t;   /* frames in the floor's current state */
int32_t boss = -1;
Stats stats;
uint32_t trials;

/* A trial met for the first time: kept, and announced. */
static void emit(int type, int x, int y);
static void trial(int id) {
	if (trials & (1u << id)) return;
	trials |= 1u << id;
	emit(EV_TRIAL, id, 0);
}

static uint16_t held_pressed;   /* buttons pressed during a hit-stop */

static void emit(int type, int x, int y) {
	if (num_events < MAX_EVENTS) {
		game_events[num_events].type = type;
		game_events[num_events].x = x;
		game_events[num_events].y = y;
		game_events[num_events].v = 0;
		num_events++;
	}
}

static void hitstop(int frames) {
	if (frames > hitstop_t) hitstop_t = frames;
}

static const int8_t DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };

static uint32_t rng = 0x1234567;
static int rnd(int n) {
	rng = rng * 1103515245 + 12345;
	return (rng >> 16) % n;
}

static void say(const char *text, int frames) {
	message = text;
	message_t = frames;
}

/* On a slab still standing (crumbling ones too, until they fall). */
static int inside(int x, int y) {
	return x >= area_x0 && y >= area_y0 && x <= area_x1 && y <= area_y1 && slab_t[y][x] <= 0;
}

int game_fell(void) { return fell; }
void game_death_tile(int *x, int *y) { *x = fell ? fall_x : player.x; *y = fell ? fall_y : player.y; }

int game_slab(int x, int y) {
	if (x < 0 || y < 0 || x >= MAX_N || y >= MAX_N) return 9999;
	if (slab_t[y][x] == 0 && !(x >= area_x0 && y >= area_y0 && x <= area_x1 && y <= area_y1)) return 9999;
	return slab_t[y][x];
}

static int enemy_at(int x, int y) {
	for (int i = 0; i < MAX_ENEMIES; i++) {
		const Enemy *e = &enemies[i];
		if (e->state != E_NONE && e->state != E_DYING && e->x == x && e->y == y)
			return i;
	}
	return -1;
}

/* ---- Floor -------------------------------------------------------------------- */

/* Each wave: up to 3 of { kind, x, y }, on the floor as it is by then. */
#define NUM_WAVES 3
static const int8_t WAVES[NUM_WAVES][3][3] = {
	{ { K_HUSK, 4, 1 }, { -1 }, { -1 } },                           /* 9 x 9 */
	{ { K_HUSK, 1, 2 }, { K_SLEEPER, 6, 4 }, { -1 } },              /* 8 x 8: x 0..7, y 1..8 */
	{ { K_HUSK, 2, 1 }, { K_HUSK, 6, 1 }, { K_BELL, 1, 4 } },       /* 7 x 7: x 1..7, y 1..7 */
};
/* After each wave, the edges that fall (top, right, bottom, left) and the
 * floor left: the boss's is 6 x 6. */
static const int8_t SHRINK[NUM_WAVES][4] = {
	{ 0, 1, 7, 8 },   /* x0, y0, x1, y1: the top row and right column go */
	{ 1, 1, 7, 7 },   /* the bottom row and left column */
	{ 1, 2, 6, 7 },   /* the top row and right column again */
};
static int next_x0, next_y0, next_x1, next_y1;

/* The edge outside the next area cracks, then falls in turn. */
static void start_crumble(int w) {
	next_x0 = SHRINK[w][0]; next_y0 = SHRINK[w][1]; next_x1 = SHRINK[w][2]; next_y1 = SHRINK[w][3];
	for (int y = area_y0; y <= area_y1; y++)
		for (int x = area_x0; x <= area_x1; x++)
			if (x < next_x0 || x > next_x1 || y < next_y0 || y > next_y1)
				slab_t[y][x] = -(CRUMBLE_FRAMES + (x * 7 + y * 13) % 24);
	emit(EV_CRUMBLE, 0, 0);
}

/* Once a frame: crumbling slabs fall; the soul on one falls with it. */
static void start_fall(int from_x, int from_y, int to_x, int to_y);
static void player_tile(int *x, int *y);
static void update_slabs(void) {
	for (int y = 0; y < MAX_N; y++)
		for (int x = 0; x < MAX_N; x++) {
			int16_t *s = &slab_t[y][x];
			if (*s < 0 && ++*s == 0) {
				*s = 1;
				if (shake_t < 4) shake_t = 4;   /* each one, a thud */
				int px, py;
				player_tile(&px, &py);
				if (px == x && py == y && player.state != P_DEAD && player.state != P_FALL && !(player.state == P_JUMP && player.t < AIR_LAST))
					start_fall(x, y, x, y);
				if (drop_x == x && drop_y == y) {   /* what it left: onto the floor still standing */
					drop_x = drop_x < next_x0 ? next_x0 : drop_x > next_x1 ? next_x1 : drop_x;
					drop_y = drop_y < next_y0 ? next_y0 : drop_y > next_y1 ? next_y1 : drop_y;
				}
			} else if (*s > 0 && *s < 999) ++*s;
		}
}

void game_start_floor(int keep_drop) {
	grid_n = 9;
	if (!keep_drop) memset(&stats, 0, sizeof(stats));   /* a new trial */
	memset(&player, 0, sizeof(player));
	player.x = player.from_x = 4;
	player.y = player.from_y = 8;
	player.facing = DIR_UP;
	player.hp = HP_MAX;
	player.stamina = STAMINA_MAX * 16;
	player.flasks = FLASKS;
	player.hurt_t = 999;
	player.lock = -1;
	area_x0 = area_y0 = 0;
	area_x1 = area_y1 = grid_n - 1;
	memset(slab_t, 0, sizeof(slab_t));
	fell = 0;
	memset(enemies, 0, sizeof(enemies));
	if (!keep_drop) {
		drop_x = drop_y = -1;
		drop_amount = 0;
	}
	wave = 0;
	enemies_alive = 0;
	boss = -1;
	floor_state = F_WAVE_INTRO;
	floor_t = 0;
	say("FLOOR I  -  SLOTH", 120);
}

void game_title_scene(void) {
	game_start_floor(0);
	player.x = player.from_x = 1;   /* to the left of the menu */
	player.y = player.from_y = 6;
	player.facing = DIR_DOWN;
	drop_amount = 0;
	drop_x = drop_y = -1;
	message = 0;
	floor_state = F_FIGHT;   /* (not an entrance: no title card) */
	floor_t = 200;
}

void game_title_tick(void) {
	game_frames++;
}

static void spawn_wave(void) {
	int n = 0;
	for (int i = 0; i < 3; i++) {
		const int8_t *w = WAVES[wave][i];
		if (w[0] < 0) continue;
		Enemy *e = &enemies[n];
		memset(e, 0, sizeof(*e));
		e->kind = w[0];
		e->state = E_RISE;   /* out of the slabs, then awake (or asleep) */
		e->t = -n * 25;      /* not all at once */
		e->x = e->from_x = w[1];
		e->y = e->from_y = w[2];
		if (e->x == player.x && e->y == player.y) e->x = e->from_x = e->x + (e->x < (area_x0 + area_x1) / 2 ? 1 : -1);
		e->hp = e->kind == K_BELL ? BELL_HP : HUSK_HP;
		e->hit_t = 999;
		n++;
	}
	enemies_alive = n;
}

/* After the last wave: the Unfinished rises in the middle of the floor. */
static void spawn_boss(void) {
	Enemy *e = &enemies[0];
	memset(e, 0, sizeof(*e));
	e->kind = K_UNFINISHED;
	e->state = E_SLEEP;   /* still: it doesn't wake by itself */
	e->x = e->from_x = 4;
	e->y = e->from_y = 4;   /* in the middle of what's left (6 x 6) */
	if (player.x == 4 && player.y == 4) e->x = e->from_x = 3;
	e->hp = BOSS_HP;
	e->hit_t = 999;
	boss = 0;
	enemies_alive = 1;
	floor_state = F_BOSS_INTRO;
	floor_t = 0;
}

/* ---- Player ------------------------------------------------------------------- */

/* The tile the player can be hit on: on a dodge, each tile it passes in
 * turn; on a walk, the one left until halfway. */
static void player_tile(int *x, int *y) {
	if (player.state == P_DODGE) {
		int dx = player.x - player.from_x, dy = player.y - player.from_y;
		int n = dx ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
		int k = (n * player.t + DODGE_FRAMES / 2) / DODGE_FRAMES;   /* tiles covered so far */
		if (k > n) k = n;
		*x = player.from_x + (n ? dx / n * k : 0);
		*y = player.from_y + (n ? dy / n * k : 0);
	} else if (player.state == P_WALK && player.t < WALK_FRAMES / 2) {
		*x = player.from_x;
		*y = player.from_y;
	} else {
		*x = player.x;
		*y = player.y;
	}
}

static void die(void) {
	stats.deaths++;
	fell = player.state == P_FALL;
	player.state = P_DEAD;
	player.t = 0;
	floor_state = F_DEAD;
	floor_t = 0;
	shake_t = 20;
	say(fell ? "LOST TO THE VOID" : "JUDGED", 0);
	emit(EV_DEATH, fell ? fall_x : player.x, fell ? fall_y : player.y);
}

/* Off the edge (or the slab gone from under it): falling, then gone. The
 * soul glides from (from) towards (to) and drops. */
static void start_fall(int from_x, int from_y, int to_x, int to_y) {
	fall_x = from_x;
	fall_y = from_y;
	player.from_x = from_x;
	player.from_y = from_y;
	player.x = to_x;
	player.y = to_y;
	player.state = P_FALL;
	player.t = 0;
	player.buffered = 0;
	emit(EV_FALL, from_x, from_y);
}

/* An attack from (ex, ey) lands on the player's tile. */
static void player_hit(int ex, int ey, int damage) {
	if (player.state == P_DEAD || player.state == P_FALL)
		return;
	if (player.state == P_DODGE && player.t >= IFRAME_FIRST && player.t <= IFRAME_LAST)
		return;   /* rolled through it */
	if (player.state == P_JUMP && player.t >= AIR_FIRST && player.t <= AIR_LAST) {
		stats.jumped++;
		if (stats.jumped >= TRIAL_AIR_COUNT) trial(TRIAL_AIR);
		return;   /* jumped over it */
	}
	int px, py;
	player_tile(&px, &py);
	player.hp -= damage;
	stats.hurt += damage;
	player.hurt_t = 0;
	shake_t = 10;
	hitstop(5);
	emit(EV_HURT, px, py);
	if (player.state == P_DRINK || player.state == P_CHARGE) {
		player.state = P_IDLE;   /* the flask is spilled, the blow lost */
		player.t = 0;
	}
	if (player.hp <= 0) {
		player.hp = 0;
		die();
	}
}

static int dir_of(uint16_t bits) {
	if (bits & PAD_UP) return DIR_UP;
	if (bits & PAD_RIGHT) return DIR_RIGHT;
	if (bits & PAD_DOWN) return DIR_DOWN;
	if (bits & PAD_LEFT) return DIR_LEFT;
	return -1;
}

void game_player_tile(int *x, int *y) { player_tile(x, y); }

/* Two tiles, over an enemy if need be (one if the second is taken); off
 * the edge, it lands on nothing and falls. */
static int try_dodge(int dir) {
	if (player.stamina <= 0) { emit(EV_TIRED, player.x, player.y); return 0; }
	int nx = -99, ny = -99;
	for (int d = DODGE_TILES; d >= 1 && nx == -99; d--) {
		int tx = player.x + DX[dir] * d, ty = player.y + DY[dir] * d;
		if (enemy_at(tx, ty) < 0) { nx = tx; ny = ty; }
	}
	if (nx == -99)
		return 0;
	player.from_x = player.x;
	player.from_y = player.y;
	player.x = nx;
	player.y = ny;
	player.dodge_dir = dir;
	player.state = P_DODGE;
	player.t = 0;
	player.stamina -= DODGE_COST;
	player.stamina_wait = STAMINA_WAIT;
	emit(EV_DODGE, nx, ny);
	return 1;
}

static int try_walk(int dir) {
	int nx = player.x + DX[dir], ny = player.y + DY[dir];
	if (!inside(nx, ny)) {   /* a step onto nothing */
		start_fall(player.x, player.y, nx, ny);
		return 1;
	}
	if (enemy_at(nx, ny) >= 0)
		return 0;
	player.from_x = player.x;
	player.from_y = player.y;
	player.x = nx;
	player.y = ny;
	player.state = P_WALK;
	player.t = 0;
	emit(EV_STEP, nx, ny);
	return 1;
}

static int can_face(int i) {
	return i >= 0 && i < MAX_ENEMIES && enemies[i].state != E_NONE && enemies[i].state != E_DYING;
}

/* Face the enemy locked on (R2), else the nearest, along a row or a column;
 * exactly diagonal: as before. */
static void face_nearest(void) {
	int best = 1 << 30, bdx = 0, bdy = 0;
	if (!can_face(player.lock)) player.lock = -1;
	for (int i = 0; i < MAX_ENEMIES; i++) {
		const Enemy *e = &enemies[i];
		if (!can_face(i) || (player.lock >= 0 && i != player.lock)) continue;
		int dx = e->x - player.x, dy = e->y - player.y;
		int d = dx * dx + dy * dy;
		if (d < best) { best = d; bdx = dx; bdy = dy; }
	}
	if (best == 1 << 30) return;
	int adx = bdx < 0 ? -bdx : bdx, ady = bdy < 0 ? -bdy : bdy;
	if (adx > ady) player.facing = bdx > 0 ? DIR_RIGHT : DIR_LEFT;
	else if (ady > adx) player.facing = bdy > 0 ? DIR_DOWN : DIR_UP;
}

/* The swing itself: light, or heavy after a full charge. */
static int swing(int heavy) {
	int cost = heavy ? HEAVY_COST : ATTACK_COST;
	if (player.stamina <= 0) { emit(EV_TIRED, player.x, player.y); return 0; }
	player.state = P_ATTACK;
	player.heavy = heavy;
	player.t = 0;
	player.stamina -= cost;
	player.stamina_wait = STAMINA_WAIT;
	emit(EV_SWING, player.x, player.y);
	return 1;
}

static int try_jump(void) {
	if (player.stamina <= 0) { emit(EV_TIRED, player.x, player.y); return 0; }
	player.state = P_JUMP;
	player.t = 0;
	player.stamina -= JUMP_COST;
	player.stamina_wait = STAMINA_WAIT;
	emit(EV_JUMP, player.x, player.y);
	return 1;
}

static void sword_hits(void) {
	int tx = player.x + DX[player.facing], ty = player.y + DY[player.facing];
	int i = inside(tx, ty) ? enemy_at(tx, ty) : -1;
	if (i < 0)
		return;
	Enemy *e = &enemies[i];
	if (e->state == E_RISE)
		return;   /* still ash */
	int heavy = player.heavy, riposte = e->state == E_STAGGER, asleep = e->state == E_SLEEP;
	int damage = (heavy ? HEAVY_DAMAGE : ATTACK_DAMAGE) * (riposte ? RIPOSTE_FACTOR : asleep ? SLEEP_FACTOR : 1);
	e->hp -= damage;
	e->hit_t = 0;
	shake_t = riposte ? 12 : heavy ? 14 : 3;
	hitstop(riposte ? 12 : heavy ? 13 : 4);
	emit(riposte ? EV_RIPOSTE : heavy ? EV_HEAVY : EV_HIT, tx, ty);
	if (num_events) game_events[num_events - 1].v = damage;
	if (riposte) { stats.ripostes++; trial(TRIAL_RIPOSTE); }
	if (e->hp <= 0) {
		e->state = E_DYING;
		e->t = 0;
		absolution += e->kind == K_UNFINISHED ? BOSS_REWARD : HUSK_REWARD;
		enemies_alive--;
		emit(EV_KILL, tx, ty);
		stats.kills++;
		if (e->kind == K_SLEEPER && asleep) trial(TRIAL_SLEEP);
		if (e->kind == K_BELL && e->attack != ATK_RING) trial(TRIAL_BELL);   /* before it ever tolled */
		if (e->kind == K_UNFINISHED) {
			trial(TRIAL_BOSS);
			if (!stats.deaths) trial(TRIAL_UNBROKEN);
			if (!stats.flasks) trial(TRIAL_DRY);
			if (stats.frames <= TRIAL_SWIFT_TIME) trial(TRIAL_SWIFT);
			if (!stats.hurt) trial(TRIAL_UNTOUCHED);
		}
	} else if (asleep) {
		e->state = E_WAKE;      /* a rude awakening */
		e->t = 0;
		emit(EV_WAKE, tx, ty);
	} else if (riposte) {
		e->state = E_IDLE;      /* the opening is spent */
		e->t = 0;
	} else if (heavy) {
		e->state = E_STAGGER;   /* broken: open to a riposte */
		e->t = 0;
		emit(EV_BROKEN, tx, ty);
	}
}

enum { BUF_NONE, BUF_DODGE, BUF_ATTACK, BUF_JUMP };

static void update_player(const Input *in) {
	Player *p = &player;
	p->t++;
	if (p->hurt_t < 999) p->hurt_t++;
	if (p->buffer_t > 0 && --p->buffer_t == 0) p->buffered = BUF_NONE;
	if (p->state == P_DEAD)
		return;
	if (p->state == P_FALL) {
		if (p->t >= FALL_FRAMES) die();
		return;
	}
	/* Nothing under it any more (walking onto a slab as it went, landing a
	 * jump where one was): it falls. A dodge is checked as it lands. */
	if (p->state != P_DODGE && !(p->state == P_JUMP && p->t <= AIR_LAST) && !inside(p->x, p->y)) {
		start_fall(p->from_x, p->from_y, p->x, p->y);
		return;
	}

	/* R2: face the next enemy (round to the nearest again). */
	if (in->pressed & PAD_R2) {
		int i = p->lock;
		do i = i + 1 >= MAX_ENEMIES ? -1 : i + 1; while (i >= 0 && !can_face(i));
		p->lock = i;
		emit(EV_LOCK, i, 0);
	}

	/* The D-pad walks (screen directions: Up = away from the camera). */
	int held_dir = dir_of(in->held);

	/* Wanted this frame (or buffered while busy). */
	if (in->pressed & PAD_CIRCLE) {
		p->buffered = BUF_DODGE;
		p->buffer_t = BUFFER_FRAMES;
		p->buffer_dir = held_dir;   /* a direction held: dodge that way, else away from the enemy */
	}
	if (in->pressed & PAD_SQUARE) {
		p->buffered = BUF_ATTACK;
		p->buffer_t = BUFFER_FRAMES;
	}
	if (in->pressed & PAD_CROSS) {
		p->buffered = BUF_JUMP;
		p->buffer_t = BUFFER_FRAMES;
	}

	switch (p->state) {
	case P_DODGE:
		if (p->t >= DODGE_FRAMES) {
			if (!inside(p->x, p->y)) {   /* landed on nothing */
				int fx = p->from_x, fy = p->from_y;
				start_fall(p->x, p->y, p->x, p->y);
				fall_x = fx;
				fall_y = fy;
				return;
			}
			p->state = P_IDLE; p->t = 0; p->from_x = p->x; p->from_y = p->y;
		}
		break;
	case P_WALK:
		if (p->t >= WALK_FRAMES) { p->state = P_IDLE; p->t = 0; p->from_x = p->x; p->from_y = p->y; }
		break;
	case P_CHARGE:
		/* Let go early: an ordinary swing, already that far along. Held to
		 * the end: the heavy blow, on release. */
		if (p->t == CHARGE_FULL) emit(EV_CHARGED, p->x, p->y);
		if (!(in->held & PAD_SQUARE)) {
			int t = p->t;
			if (t >= CHARGE_FULL) swing(1);
			else if (swing(0)) p->t = t < ATTACK_WINDUP - 1 ? t : ATTACK_WINDUP - 1;
			else { p->state = P_IDLE; p->t = 0; }
		}
		break;
	case P_ATTACK:
		if (p->t == (p->heavy ? HEAVY_WINDUP : ATTACK_WINDUP)) sword_hits();
		if (p->t >= (p->heavy ? HEAVY_FRAMES : ATTACK_FRAMES)) { p->state = P_IDLE; p->t = 0; }
		break;
	case P_JUMP:
		if (p->t == AIR_LAST + 1) emit(EV_LAND, p->x, p->y);
		if (p->t >= JUMP_FRAMES) { p->state = P_IDLE; p->t = 0; }
		break;
	case P_DRINK:
		if (p->t == DRINK_HEAL_AT) { p->hp = p->hp + FLASK_HEAL > HP_MAX ? HP_MAX : p->hp + FLASK_HEAL; emit(EV_HEAL, p->x, p->y); }
		if (p->t >= DRINK_FRAMES) { p->state = P_IDLE; p->t = 0; }
		break;
	}

	/* Cancels: a dodge or a jump cuts a charge or the end of a swing; any
	 * action cuts a walk (back to the tile it left, if not halfway yet). */
	if (p->buffered != BUF_NONE) {
		int cancel = 0;
		if (p->state == P_WALK) cancel = 1;
		else if (p->state == P_CHARGE) cancel = p->buffered != BUF_ATTACK;
		else if (p->state == P_ATTACK) cancel = p->buffered != BUF_ATTACK && p->t >= (p->heavy ? HEAVY_CANCEL : LIGHT_CANCEL);
		if (cancel) {
			if (p->state == P_WALK && p->t < WALK_FRAMES / 2) { p->x = p->from_x; p->y = p->from_y; }
			p->from_x = p->x;
			p->from_y = p->y;
			p->state = P_IDLE;
			p->t = 0;
		}
	}

	if (p->state == P_IDLE) {
		if (p->buffered == BUF_DODGE) {
			try_dodge(p->buffer_dir >= 0 ? p->buffer_dir : (p->facing + 2) & 3);
			p->buffered = BUF_NONE;
		} else if (p->buffered == BUF_ATTACK) {
			/* Square starts the charge; let go at once and it's a plain swing. */
			if (p->stamina > 0) { p->state = P_CHARGE; p->t = 0; }
			else emit(EV_TIRED, p->x, p->y);
			p->buffered = BUF_NONE;
		} else if (p->buffered == BUF_JUMP) {
			try_jump();
			p->buffered = BUF_NONE;
		} else if ((in->pressed & PAD_TRIANGLE) && p->flasks > 0) {
			p->flasks--;
			stats.flasks++;
			p->state = P_DRINK;
			p->t = 0;
		} else if (held_dir >= 0) {
			try_walk(held_dir);
		}
	}
	/* A charge started this frame sees the button already let go: swing. */
	if (p->state == P_CHARGE && p->t == 0 && !(in->held & PAD_SQUARE)) {
		if (!swing(0)) p->state = P_IDLE;
	}

	/* Always facing the nearest enemy, except mid-swing and mid-dodge (it
	 * turns once it lands, past an enemy it went over). */
	if (p->state != P_ATTACK && p->state != P_DODGE) face_nearest();

	/* Stamina refills after a short pause. */
	if (p->stamina_wait > 0) {
		p->stamina_wait--;
	} else if (p->state != P_ATTACK && p->state != P_DODGE && p->state != P_JUMP && p->state != P_CHARGE) {
		p->stamina += STAMINA_REGEN;
		if (p->stamina > STAMINA_MAX * 16) p->stamina = STAMINA_MAX * 16;
	}

	/* Absolution left here last time. */
	if (drop_amount > 0 && p->state != P_DODGE && p->state != P_WALK && p->x == drop_x && p->y == drop_y) {
		absolution += drop_amount;
		drop_amount = 0;
		drop_x = drop_y = -1;
		say("ABSOLUTION RECOVERED", 90);
		emit(EV_RECOVER, p->x, p->y);
	}
}

/* ---- Enemies ------------------------------------------------------------------- */

static void mark(Enemy *e, int x, int y) {
	if (inside(x, y)) e->target[y * MAX_N + x] = 1;
}

static void start_attack(Enemy *e, int kind) {
	int px, py;
	player_tile(&px, &py);
	memset(e->target, 0, sizeof(e->target));
	e->attack = kind;
	switch (kind) {
	case ATK_SLAM:   /* the 8 tiles around it */
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++)
				if (dx || dy) mark(e, e->x + dx, e->y + dy);
		e->windup = 55;
		break;
	case ATK_LINE: { /* 4 tiles towards the player */
		int dx = px > e->x ? 1 : px < e->x ? -1 : 0, dy = py > e->y ? 1 : py < e->y ? -1 : 0;
		for (int k = 1; k <= 4; k++) mark(e, e->x + dx * k, e->y + dy * k);
		e->windup = 65;
		break;
	}
	/* The Unfinished's blows come in two halves: never quite done. */
	case ATK_UNDONE:    /* the ring around it... */
	case ATK_UNDONE2: { /* ...then the ring around that */
		int r = kind == ATK_UNDONE ? 1 : 2;
		for (int dy = -r; dy <= r; dy++)
			for (int dx = -r; dx <= r; dx++)
				if ((dx < 0 ? -dx : dx) == r || (dy < 0 ? -dy : dy) == r) mark(e, e->x + dx, e->y + dy);
		e->windup = kind == ATK_UNDONE ? 50 : 24;
		break;
	}
	case ATK_TOMORROW:  /* every other row, yours first... */
	case ATK_TOMORROW2: /* ...and the rest tomorrow */
		for (int y = 0; y < grid_n; y++)
			if (((y ^ py) & 1) == (kind == ATK_TOMORROW2))
				for (int x = 0; x < grid_n; x++)
					if (x != e->x || y != e->y) mark(e, x, y);
		e->windup = kind == ATK_TOMORROW ? 70 : 40;
		break;
	case ATK_ROPE: {    /* a rope lashed out to the edge */
		int dx = px - e->x, dy = py - e->y, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
		int sx = adx >= ady ? (dx > 0 ? 1 : -1) : 0, sy = adx >= ady ? 0 : (dy > 0 ? 1 : -1);
		for (int k = 1; k < grid_n; k++) mark(e, e->x + sx * k, e->y + sy * k);
		e->windup = e->hp < BOSS_HP / 2 ? 32 : 44;
		break;
	}
	case ATK_UNRAVEL:   /* both diagonals out to the edge... */
	case ATK_UNRAVEL2:  /* ...then its row and column, coming apart */
		for (int k = 1; k < grid_n; k++)
			for (int s = 0; s < 4; s++) {
				int sx = kind == ATK_UNRAVEL ? (s & 1 ? 1 : -1) : DX[s], sy = kind == ATK_UNRAVEL ? (s & 2 ? 1 : -1) : DY[s];
				mark(e, e->x + sx * k, e->y + sy * k);
			}
		e->windup = kind == ATK_UNRAVEL ? 56 : 26;
		break;
	case ATK_LOOSE:     /* a loose thread where you stand, and around... */
	case ATK_LOOSE2:    /* ...and again, wherever you went */
		mark(e, px, py);
		for (int s = 0; s < 4; s++) mark(e, px + DX[s], py + DY[s]);
		e->windup = kind == ATK_LOOSE ? 46 : 34;
		break;
	case ATK_RING: { /* the bell: across the whole floor from its edge */
		int dx = e->x == area_x0 ? 1 : e->x == area_x1 ? -1 : 0, dy = dx ? 0 : e->y == area_y0 ? 1 : -1;
		for (int k = 1; k < grid_n; k++) mark(e, e->x + dx * k, e->y + dy * k);
		e->windup = BELL_WINDUP;
		break;
	}
	case ATK_CROSS:  /* its whole row and column */
		for (int k = 0; k < grid_n; k++) {
			if (k != e->x) mark(e, k, e->y);
			if (k != e->y) mark(e, e->x, k);
		}
		e->windup = 85;
		break;
	}
	e->state = E_WINDUP;
	e->t = 0;
	emit(EV_WINDUP, e->x, e->y);
}

static void update_enemy(Enemy *e) {
	if (e->state == E_NONE)
		return;
	e->t++;
	if (e->hit_t < 999) e->hit_t++;
	int px, py;
	player_tile(&px, &py);
	switch (e->state) {
	case E_RISE:
		if (e->t == 1) emit(EV_SPAWN, e->x, e->y);
		if (e->t >= RISE_FRAMES) { e->state = e->kind == K_SLEEPER ? E_SLEEP : E_IDLE; e->t = 0; }
		break;
	case E_SLEEP: {
		if (e->kind == K_UNFINISHED) break;   /* waits for its entrance to end */
		int dx = px - e->x, dy = py - e->y;
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if ((dx > dy ? dx : dy) <= WAKE_RANGE && player.state != P_DEAD) {
			e->state = E_WAKE;
			e->t = 0;
			shake_t = shake_t > 6 ? shake_t : 6;
			emit(EV_WAKE, e->x, e->y);
		}
		break;
	}
	case E_WAKE:
		if (e->t >= WAKE_FRAMES) { e->state = E_IDLE; e->t = HUSK_IDLE; }   /* and straight at you */
		break;
	case E_IDLE: {
		if (player.state == P_DEAD)
			break;
		if (e->kind == K_UNFINISHED) {
			if (e->t < (e->hp < BOSS_HP / 2 ? BOSS_IDLE_ANGRY : BOSS_IDLE)) break;
			int dx = px - e->x, dy = py - e->y, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
			int cheb = adx > ady ? adx : ady;
			int r = rnd(6);
			if (cheb <= 2 && r < 3) start_attack(e, ATK_UNDONE);
			else if (cheb <= 2 && r < 5) start_attack(e, ATK_UNRAVEL);
			else if ((dx == 0 || dy == 0) && rnd(5) < 3) start_attack(e, ATK_ROPE);
			else if (r < 2) start_attack(e, ATK_LOOSE);
			else if (cheb > 4 && rnd(2)) {
				/* Drag itself a tile closer. */
				int nx = e->x + (adx >= ady ? (dx > 0 ? 1 : -1) : 0), ny = e->y + (adx >= ady ? 0 : (dy > 0 ? 1 : -1));
				e->t = 0;
				if (inside(nx, ny) && enemy_at(nx, ny) < 0 && !(nx == px && ny == py)) {
					e->from_x = e->x; e->from_y = e->y;
					e->x = nx; e->y = ny;
					e->state = E_MOVE;
				}
			} else start_attack(e, ATK_TOMORROW);
			break;
		}
		if (e->kind == K_BELL) {
			/* Keeps to its edge: lines up with the soul, then rings. */
			if (e->t < BELL_IDLE) break;
			int side = e->x == area_x0 || e->x == area_x1;   /* on a side edge: rings rows */
			int want = side ? py : px, at = side ? e->y : e->x;
			if (want == at) { start_attack(e, ATK_RING); break; }
			int nx = e->x + (side ? 0 : want > at ? 1 : -1), ny = e->y + (side ? (want > at ? 1 : -1) : 0);
			e->t = 0;
			if (!inside(nx, ny) || enemy_at(nx, ny) >= 0 || (nx == player.x && ny == player.y) || (nx == px && ny == py)) break;
			e->from_x = e->x; e->from_y = e->y;
			e->x = nx; e->y = ny;
			e->state = E_MOVE;
			break;
		}
		if (e->t < HUSK_IDLE)
			break;
		int dx = px - e->x, dy = py - e->y, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
		int cheb = adx > ady ? adx : ady;
		if (cheb == 1) {
			start_attack(e, ATK_SLAM);
		} else if ((dx == 0 || dy == 0) && adx + ady <= 4) {
			start_attack(e, rnd(4) ? ATK_LINE : ATK_CROSS);
		} else if (cheb <= 4 && rnd(5) == 0) {
			start_attack(e, ATK_CROSS);
		} else {
			/* Lumber one tile closer, along the longer way first. */
			int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
			int tries[2][2] = { { adx >= ady ? sx : 0, adx >= ady ? 0 : sy }, { adx >= ady ? 0 : sx, adx >= ady ? sy : 0 } };
			e->t = 0;
			for (int k = 0; k < 2; k++) {
				int nx = e->x + tries[k][0], ny = e->y + tries[k][1];
				if ((nx == e->x && ny == e->y) || !inside(nx, ny) || enemy_at(nx, ny) >= 0 || (nx == player.x && ny == player.y) || (nx == px && ny == py))
					continue;
				e->from_x = e->x;
				e->from_y = e->y;
				e->x = nx;
				e->y = ny;
				e->state = E_MOVE;
				break;
			}
		}
		break;
	}
	case E_MOVE:
		if (e->t >= (e->kind == K_BELL ? BELL_MOVE : e->kind == K_UNFINISHED ? BOSS_MOVE : HUSK_MOVE)) { e->state = E_IDLE; e->t = 0; e->from_x = e->x; e->from_y = e->y; }
		break;
	case E_WINDUP:
		if (e->attack == ATK_RING && e->t % 20 == 1) emit(EV_BELL, e->x, e->y);   /* each toll */
		if (e->t >= e->windup) {
			player_tile(&px, &py);
			/* Dodged at the last moment from where it lands: time slows, breath back. */
			if (player.state == P_DODGE && player.t <= PERFECT_FRAMES && inside(player.from_x, player.from_y) &&
			    e->target[player.from_y * MAX_N + player.from_x]) {
				player.stamina = player.stamina + PERFECT_STAMINA > STAMINA_MAX * 16 ? STAMINA_MAX * 16 : player.stamina + PERFECT_STAMINA;
				emit(EV_PERFECT, player.from_x, player.from_y);
			}
			if (px >= 0 && py >= 0 && px < MAX_N && py < MAX_N && e->target[py * MAX_N + px])
				player_hit(e->x, e->y, e->kind == K_UNFINISHED ? BOSS_DAMAGE : HUSK_DAMAGE);
			shake_t = shake_t > 3 ? shake_t : 3;
			emit(EV_SLAM, e->x, e->y);
			e->state = E_RECOVER;
			e->t = 0;
			/* The second half follows at once. */
			if (e->attack == ATK_UNDONE) start_attack(e, ATK_UNDONE2);
			else if (e->attack == ATK_TOMORROW) start_attack(e, ATK_TOMORROW2);
			else if (e->attack == ATK_UNRAVEL) start_attack(e, ATK_UNRAVEL2);
			else if (e->attack == ATK_LOOSE) start_attack(e, ATK_LOOSE2);
		}
		break;
	case E_RECOVER:
		if (e->t >= (e->kind == K_BELL ? BELL_RECOVER : e->kind == K_UNFINISHED ? BOSS_RECOVER : HUSK_RECOVER + rnd(20))) { e->state = E_IDLE; e->t = 0; }
		break;
	case E_STAGGER:
		if (e->t >= HUSK_STAGGER) { e->state = E_IDLE; e->t = 0; }
		break;
	case E_DYING:
		if (e->t >= HUSK_DYING) e->state = E_NONE;
		break;
	}
}

int game_telegraph(int x, int y) {
	int best = 0;
	for (int i = 0; i < MAX_ENEMIES; i++) {
		const Enemy *e = &enemies[i];
		if (e->state != E_WINDUP || !e->target[y * MAX_N + x])
			continue;
		int v = e->t >= e->windup - BRIGHT_FRAMES ? 256 : 70 + (e->t * 60 / e->windup);
		if (v > best) best = v;
	}
	return best;
}

/* ---- The frame ---------------------------------------------------------------- */

void game_update(const Input *in) {
	num_events = 0;
	if (hitstop_t > 0) {
		/* Frozen for a moment on a heavy hit: inputs wait. */
		hitstop_t--;
		held_pressed |= in->pressed;
		return;
	}
	Input now = *in;
	now.pressed |= held_pressed;
	held_pressed = 0;
	in = &now;
	game_frames++;
	if (floor_state != F_DEAD && floor_state != F_CLEARED) stats.frames++;
	floor_t++;
	if (message_t > 0) message_t--;
	if (shake_t > 0) shake_t--;

	switch (floor_state) {
	case F_WAVE_INTRO:
		if (wave == 0) {
			if (floor_t >= 90) { spawn_wave(); floor_state = F_FIGHT; floor_t = 0; }
			break;
		}
		/* A breather: a little health back, the edge cracks and falls, then
		 * the next wave (or the boss) rises. */
		if (floor_t == 1) {
			start_crumble(wave - 1);
			player.hp = player.hp + WAVE_HEAL > HP_MAX ? HP_MAX : player.hp + WAVE_HEAL;
			emit(EV_HEAL, player.x, player.y);
			say("THE EDGE CRUMBLES", 100);
		}
		if (floor_t == CRUMBLE_FRAMES + 40) {   /* all fallen: the floor as it is now */
			const int8_t *a = SHRINK[wave - 1 < NUM_WAVES ? wave - 1 : NUM_WAVES - 1];
			area_x0 = a[0]; area_y0 = a[1]; area_x1 = a[2]; area_y1 = a[3];
		}
		if (floor_t == CRUMBLE_FRAMES + 50 && wave < NUM_WAVES) say(wave == 1 ? "WAVE II" : "WAVE III", 90);
		if (floor_t >= BREATHER) {
			if (wave < NUM_WAVES) { spawn_wave(); floor_state = F_FIGHT; floor_t = 0; }
			else spawn_boss();
		}
		break;
	case F_FIGHT:
		if (enemies_alive <= 0) {
			int dying = 0;
			for (int i = 0; i < MAX_ENEMIES; i++) dying |= enemies[i].state == E_DYING;
			if (!dying) {
				floor_t = 0;
				if (boss >= 0) { floor_state = F_CLEARED; say("THE FLOOR FALLS SILENT", 0); emit(EV_CLEAR, 0, 0); }
				else { wave++; floor_state = F_WAVE_INTRO; }
			}
		}
		break;
	case F_BOSS_INTRO:
		/* Ash rises into its shape, its name is carved; Cross skips ahead. */
		if (floor_t == 160) emit(EV_BOSS, enemies[boss].x, enemies[boss].y);
		if (floor_t > 30 && floor_t < 160 && (in->pressed & (PAD_CROSS | PAD_START))) floor_t = 159;
		if (floor_t >= BOSS_INTRO) {
			enemies[boss].state = E_IDLE;
			enemies[boss].t = 0;
			floor_state = F_FIGHT;
			floor_t = 0;
		}
		static const Input still = { 0, 0 };
		in = &still;   /* the soul waits */
		break;
	case F_DEAD:
		if (floor_t > 90 && (in->pressed & PAD_CROSS)) {
			/* Rise again: what you carried stays where you fell; what you
			 * left last time is gone. */
			int x = fell ? fall_x : player.x, y = fell ? fall_y : player.y, amount = absolution;
			guilt++;
			absolution = 0;
			drop_amount = amount;
			drop_x = amount ? x : -1;
			drop_y = amount ? y : -1;
			game_start_floor(1);
			emit(EV_RISE, player.x, player.y);
			static const Input none = { 0, 0 };
			in = &none;   /* the press that rose isn't a jump too */
		}
		break;
	}

	update_slabs();
	update_player(in);
	for (int i = 0; i < MAX_ENEMIES; i++) update_enemy(&enemies[i]);
}
