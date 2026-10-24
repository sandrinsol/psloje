/*
 * GRID SOULS: the trial's rules (see game.h). Floor 1, Sloth: a 9 x 9 grid,
 * three waves of husks: slow, heavy, hard-hitting, every blow telegraphed.
 */
#include <stdint.h>
#include <string.h>
#include <psxpad.h>
#include "game.h"

/* Player timing (frames) and costs. Stamina is kept x16 for slow refills. */
#define STEP_FRAMES     9      /* a dodge: one tile */
#define IFRAME_FIRST    2      /* invincible frames 2..7 of the dodge */
#define IFRAME_LAST     7
#define DODGE_COST      (25 * 16)
#define ATTACK_WINDUP   8      /* sword: hits on frame 8 */
#define ATTACK_FRAMES   22
#define ATTACK_COST     (12 * 16)
#define ATTACK_DAMAGE   20
#define RIPOSTE_FACTOR  3      /* hitting a staggered enemy */
#define BLOCK_COST      (22 * 16)
#define PARRY_WINDOW    6      /* Square pressed this many frames or less before the hit */
#define PARRY_WIDE      10     /* with the assist option */
#define GUARD_BREAK     40
#define STAMINA_REGEN   13     /* /16 a frame: ~49 a second */
#define STAMINA_WAIT    15
#define BLOCK_REGEN     4
#define DRINK_FRAMES    45
#define DRINK_HEAL_AT   30
#define FLASK_HEAL      40
#define FLASKS          3
#define BUFFER_FRAMES   10

/* Sloth's husk. */
#define HUSK_HP         60
#define HUSK_DAMAGE     30
#define HUSK_IDLE       50
#define HUSK_MOVE       36
#define HUSK_RECOVER    45
#define HUSK_STAGGER    90
#define HUSK_DYING      30
#define HUSK_REWARD     40
#define BRIGHT_FRAMES   15     /* the last frames of a windup: bright */

enum { ATK_SLAM, ATK_LINE, ATK_CROSS };

int32_t grid_n = 9;
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
int32_t parry_wide;

static uint16_t held_pressed;   /* buttons pressed during a hit-stop */

static void emit(int type, int x, int y) {
	if (num_events < MAX_EVENTS) {
		game_events[num_events].type = type;
		game_events[num_events].x = x;
		game_events[num_events].y = y;
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

static int inside(int x, int y) {
	return x >= 0 && y >= 0 && x < grid_n && y < grid_n;
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

static const int WAVES[] = { 1, 2, 3 };
#define NUM_WAVES 3
static const int8_t SPAWNS[][2] = { { 4, 1 }, { 1, 2 }, { 7, 2 }, { 2, 0 }, { 6, 0 }, { 4, 3 } };

void game_start_floor(int keep_drop) {
	grid_n = 9;
	memset(&player, 0, sizeof(player));
	player.x = player.from_x = 4;
	player.y = player.from_y = 8;
	player.facing = DIR_UP;
	player.hp = HP_MAX;
	player.stamina = STAMINA_MAX * 16;
	player.flasks = FLASKS;
	player.parry_t = 999;
	player.hurt_t = 999;
	player.parried_t = 999;
	memset(enemies, 0, sizeof(enemies));
	if (!keep_drop) {
		drop_x = drop_y = -1;
		drop_amount = 0;
	}
	wave = 0;
	enemies_alive = 0;
	floor_state = F_WAVE_INTRO;
	floor_t = 0;
	say("FLOOR I  -  SLOTH", 120);
}

static void spawn_wave(void) {
	int n = WAVES[wave];
	for (int i = 0; i < n && i < MAX_ENEMIES; i++) {
		Enemy *e = &enemies[i];
		memset(e, 0, sizeof(*e));
		e->state = E_IDLE;
		e->t = -i * 25;   /* not all at once */
		e->x = e->from_x = SPAWNS[i][0];
		e->y = e->from_y = SPAWNS[i][1];
		e->hp = HUSK_HP;
		e->hit_t = 999;
	}
	enemies_alive = n;
}

/* ---- Player ------------------------------------------------------------------- */

/* The tile the player can be hit on: the dodge's destination from halfway. */
static void player_tile(int *x, int *y) {
	if ((player.state == P_DODGE && player.t < STEP_FRAMES / 2) || (player.state == P_WALK && player.t < WALK_FRAMES / 2)) {
		*x = player.from_x;
		*y = player.from_y;
	} else {
		*x = player.x;
		*y = player.y;
	}
}

static void die(void) {
	player.state = P_DEAD;
	player.t = 0;
	floor_state = F_DEAD;
	floor_t = 0;
	shake_t = 20;
	say("JUDGED", 0);
	emit(EV_DEATH, player.x, player.y);
}

/* An attack from (ex, ey) lands on the player's tile. Returns 1 if parried. */
static int player_hit(int ex, int ey, int damage) {
	if (player.state == P_DEAD)
		return 0;
	if (player.state == P_DODGE && player.t >= IFRAME_FIRST && player.t <= IFRAME_LAST)
		return 0;   /* rolled through it */
	int px, py;
	player_tile(&px, &py);
	int dx = ex - px, dy = ey - py, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
	int facing_it = (adx >= ady && player.facing == (dx > 0 ? DIR_RIGHT : DIR_LEFT)) ||
	                (ady >= adx && player.facing == (dy > 0 ? DIR_DOWN : DIR_UP));
	int ready = player.state == P_IDLE;
	if (ready && player.parry_t <= (parry_wide ? PARRY_WIDE : PARRY_WINDOW) && facing_it) {
		player.parry_t = 999;
		player.parried_t = 0;
		shake_t = 6;
		hitstop(6);
		emit(EV_PARRY, ex, ey);
		return 1;
	}
	if (ready && player.blocking) {
		player.stamina -= BLOCK_COST;
		player.stamina_wait = STAMINA_WAIT;
		shake_t = 4;
		emit(EV_BLOCK, px, py);
		if (player.stamina >= 0)
			return 0;   /* held */
		player.stamina = 0;
		player.state = P_STAGGER;   /* guard broken */
		player.t = 0;
		damage /= 2;
	}
	player.hp -= damage;
	player.hurt_t = 0;
	shake_t = 10;
	hitstop(5);
	emit(EV_HURT, px, py);
	if (player.state == P_DRINK) {
		player.state = P_IDLE;   /* the flask is spilled */
		player.t = 0;
	}
	if (player.hp <= 0) {
		player.hp = 0;
		die();
	}
	return 0;
}

static int dir_of(uint16_t bits) {
	if (bits & PAD_UP) return DIR_UP;
	if (bits & PAD_RIGHT) return DIR_RIGHT;
	if (bits & PAD_DOWN) return DIR_DOWN;
	if (bits & PAD_LEFT) return DIR_LEFT;
	return -1;
}

static int try_dodge(int dir) {
	if (player.stamina <= 0)
		return 0;
	int nx = player.x + DX[dir], ny = player.y + DY[dir];
	if (!inside(nx, ny) || enemy_at(nx, ny) >= 0)
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
	if (!inside(nx, ny) || enemy_at(nx, ny) >= 0)
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

/* Face the nearest enemy, along a row or a column; exactly diagonal: as before. */
static void face_nearest(void) {
	int best = 1 << 30, bdx = 0, bdy = 0;
	for (int i = 0; i < MAX_ENEMIES; i++) {
		const Enemy *e = &enemies[i];
		if (e->state == E_NONE || e->state == E_DYING) continue;
		int dx = e->x - player.x, dy = e->y - player.y;
		int d = dx * dx + dy * dy;
		if (d < best) { best = d; bdx = dx; bdy = dy; }
	}
	if (best == 1 << 30) return;
	int adx = bdx < 0 ? -bdx : bdx, ady = bdy < 0 ? -bdy : bdy;
	if (adx > ady) player.facing = bdx > 0 ? DIR_RIGHT : DIR_LEFT;
	else if (ady > adx) player.facing = bdy > 0 ? DIR_DOWN : DIR_UP;
}

static int try_attack(void) {
	if (player.stamina <= 0)
		return 0;
	player.state = P_ATTACK;
	player.t = 0;
	player.stamina -= ATTACK_COST;
	player.stamina_wait = STAMINA_WAIT;
	emit(EV_SWING, player.x, player.y);
	return 1;
}

static void sword_hits(void) {
	int tx = player.x + DX[player.facing], ty = player.y + DY[player.facing];
	int i = inside(tx, ty) ? enemy_at(tx, ty) : -1;
	if (i < 0)
		return;
	Enemy *e = &enemies[i];
	int riposte = e->state == E_STAGGER;
	e->hp -= ATTACK_DAMAGE * (riposte ? RIPOSTE_FACTOR : 1);
	e->hit_t = 0;
	shake_t = riposte ? 8 : 3;
	hitstop(riposte ? 9 : 4);
	emit(riposte ? EV_RIPOSTE : EV_HIT, tx, ty);
	if (e->hp <= 0) {
		e->state = E_DYING;
		e->t = 0;
		absolution += HUSK_REWARD;
		enemies_alive--;
		emit(EV_KILL, tx, ty);
	} else if (riposte) {
		e->state = E_IDLE;
		e->t = 0;
	}
}

enum { BUF_NONE, BUF_DODGE, BUF_ATTACK };

static void update_player(const Input *in) {
	Player *p = &player;
	p->t++;
	if (p->parry_t < 999) p->parry_t++;
	if (p->hurt_t < 999) p->hurt_t++;
	if (p->parried_t < 999) p->parried_t++;
	if (p->buffer_t > 0 && --p->buffer_t == 0) p->buffered = BUF_NONE;
	if (p->state == P_DEAD)
		return;

	/* The D-pad walks (screen directions: Up = away from the camera). */
	int held_dir = dir_of(in->held);
	if (in->pressed & PAD_SQUARE) p->parry_t = 0;

	/* Wanted this frame (or buffered while busy). */
	if (in->pressed & PAD_CIRCLE) {
		p->buffered = BUF_DODGE;
		p->buffer_t = BUFFER_FRAMES;
		p->buffer_dir = held_dir;   /* a direction held: dodge that way, else away from the enemy */
	}
	if (in->pressed & PAD_CROSS) {
		p->buffered = BUF_ATTACK;
		p->buffer_t = BUFFER_FRAMES;
	}

	switch (p->state) {
	case P_DODGE:
		if (p->t >= STEP_FRAMES) { p->state = P_IDLE; p->t = 0; p->from_x = p->x; p->from_y = p->y; }
		break;
	case P_WALK:
		if (p->t >= WALK_FRAMES) { p->state = P_IDLE; p->t = 0; p->from_x = p->x; p->from_y = p->y; }
		break;
	case P_ATTACK:
		if (p->t == ATTACK_WINDUP) sword_hits();
		if (p->t >= ATTACK_FRAMES) { p->state = P_IDLE; p->t = 0; }
		break;
	case P_DRINK:
		if (p->t == DRINK_HEAL_AT) { p->hp = p->hp + FLASK_HEAL > HP_MAX ? HP_MAX : p->hp + FLASK_HEAL; emit(EV_HEAL, p->x, p->y); }
		if (p->t >= DRINK_FRAMES) { p->state = P_IDLE; p->t = 0; }
		break;
	case P_STAGGER:
		if (p->t >= GUARD_BREAK) { p->state = P_IDLE; p->t = 0; }
		break;
	}

	if (p->state == P_IDLE) {
		p->blocking = (in->held & PAD_SQUARE) != 0;
		if (p->buffered == BUF_DODGE) {
			try_dodge(p->buffer_dir >= 0 ? p->buffer_dir : (p->facing + 2) & 3);
			p->buffered = BUF_NONE;
		} else if (p->buffered == BUF_ATTACK) {
			try_attack();
			p->buffered = BUF_NONE;
		} else if ((in->pressed & PAD_TRIANGLE) && p->flasks > 0) {
			p->flasks--;
			p->state = P_DRINK;
			p->t = 0;
		} else if (held_dir >= 0 && !p->blocking) {
			try_walk(held_dir);
		}
		if (p->state != P_IDLE) p->blocking = 0;
	} else {
		p->blocking = 0;
	}

	/* Always facing the nearest enemy, except mid-swing. */
	if (p->state != P_ATTACK) face_nearest();

	/* Stamina refills after a short pause, slowly while blocking. */
	if (p->stamina_wait > 0) {
		p->stamina_wait--;
	} else if (p->state != P_ATTACK && p->state != P_DODGE) {
		p->stamina += p->blocking ? BLOCK_REGEN : STAMINA_REGEN;
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
	case E_IDLE: {
		if (e->t < HUSK_IDLE || player.state == P_DEAD)
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
		if (e->t >= HUSK_MOVE) { e->state = E_IDLE; e->t = 0; e->from_x = e->x; e->from_y = e->y; }
		break;
	case E_WINDUP:
		if (e->t >= e->windup) {
			player_tile(&px, &py);
			if (e->target[py * MAX_N + px] && player_hit(e->x, e->y, HUSK_DAMAGE)) {
				e->state = E_STAGGER;   /* parried */
				e->t = 0;
				break;
			}
			shake_t = shake_t > 3 ? shake_t : 3;
			emit(EV_SLAM, e->x, e->y);
			e->state = E_RECOVER;
			e->t = 0;
		}
		break;
	case E_RECOVER:
		if (e->t >= HUSK_RECOVER + rnd(20)) { e->state = E_IDLE; e->t = 0; }
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
	floor_t++;
	if (message_t > 0) message_t--;
	if (shake_t > 0) shake_t--;

	switch (floor_state) {
	case F_WAVE_INTRO:
		if (floor_t == 1 && wave > 0) say(wave == 1 ? "WAVE II" : "WAVE III", 90);
		if (floor_t >= 90) {
			spawn_wave();
			floor_state = F_FIGHT;
			floor_t = 0;
		}
		break;
	case F_FIGHT:
		if (enemies_alive <= 0) {
			int dying = 0;
			for (int i = 0; i < MAX_ENEMIES; i++) dying |= enemies[i].state == E_DYING;
			if (!dying) {
				floor_t = 0;
				if (++wave < NUM_WAVES) floor_state = F_WAVE_INTRO;
				else { floor_state = F_CLEARED; say("THE FLOOR FALLS SILENT", 0); emit(EV_CLEAR, 0, 0); }
			}
		}
		break;
	case F_DEAD:
		if (floor_t > 90 && (in->pressed & PAD_CROSS)) {
			/* Rise again: what you carried stays where you fell; what you
			 * left last time is gone. */
			int x = player.x, y = player.y, amount = absolution;
			guilt++;
			absolution = 0;
			drop_amount = amount;
			drop_x = amount ? x : -1;
			drop_y = amount ? y : -1;
			game_start_floor(1);
		}
		break;
	}

	update_player(in);
	for (int i = 0; i < MAX_ENEMIES; i++) update_enemy(&enemies[i]);
}
