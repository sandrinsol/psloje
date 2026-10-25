/*
 * GRID SOULS: a souls-like on a shrinking grid, for the PlayStation.
 *
 * The title screen and menus run at 640 x 480 (interlaced), the fight at
 * 320 x 240, 60 fps. Controls in the fight (remappable in Options): the
 * D-pad walks, Circle (+ a direction) dodges, Cross jumps, Square attacks
 * (held: a heavy blow), Triangle drinks a flask, Start pauses.
 *
 * Variables the test harness reads by name (see ps1test.json):
 *   boot_magic, frame_count, cpu_usage, fps, screen
 *   player (game.h), wave, enemies_alive, absolution, guilt, floor_state,
 *   drop_x, drop_y, drop_amount
 */
#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include <psxpad.h>
#include <psxapi.h>
#include <psxetc.h>
#include <hwregs_c.h>
#include "game.h"
#include "draw.h"
#include "audio.h"
#include "music.h"
#include "save.h"
#include "pad.h"
#include "ui.h"

#define PRIM_LEN (96 * 1024)

typedef struct {
	DISPENV disp;
	DRAWENV draw;
	uint32_t ot[OT_LEN];
	uint8_t prims[PRIM_LEN];
} Buffer;

static Buffer buffers[2];
static int active;

enum { SCREEN_TITLE, SCREEN_GAME, SCREEN_OPENING, SCREEN_MEMORY, SCREEN_SPLASH };
volatile int32_t screen = -1;
volatile uint32_t frame_count;
volatile uint32_t boot_magic;   /* 0x51D50001 once main() runs: tests wait for it */
volatile int32_t cpu_usage, cpu_only;
volatile int32_t fps;

static uint8_t pad_buffers[2][34];
static uint16_t held;

/* 640 x 480 interlaced (one buffer, drawn while shown) or 320 x 240 double
 * buffered. */
static void set_video(int hires) {
	Colour bg;
	draw_void_colour(&bg);
	DrawSync(0);
	ui_hires = hires;
	for (int i = 0; i < 2; i++) {
		if (hires) {
			SetDefDispEnv(&buffers[i].disp, 0, 0, 640, 480);
			SetDefDrawEnv(&buffers[i].draw, 0, 0, 640, 480);
			buffers[i].disp.isinter = 1;
			buffers[i].draw.dfe = 1;
			setRGB0(&buffers[i].draw, 0, 0, 0);
		} else {
			SetDefDispEnv(&buffers[i].disp, i ? 0 : 320, 0, 320, 240);
			SetDefDrawEnv(&buffers[i].draw, i ? 320 : 0, 0, 320, 240);
			setRGB0(&buffers[i].draw, bg.r, bg.g, bg.b);
		}
		buffers[i].draw.isbg = 1;
		buffers[i].draw.dtd = 1;
	}
	gte_SetGeomOffset(160, 120);
	PutDispEnv(&buffers[active].disp);
	PutDrawEnv(&buffers[active].draw);
}

static void init(void) {
	ResetGraph(0);
	InitGeom();
	gte_SetGeomScreen(170);
	set_video(1);
	draw_init();
	ui_init();
	audio_init();   /* before the pads: SpuInit() hangs after StartPAD() */
	music_init();

	pad_init();   /* the pad read directly, for its motors (pad.c) */
	/* The save, before anything takes input (one is made if there isn't). */
	if (!save_load()) save_write();
	trials = record.trials;

	/* Root counter 1 counts scanlines: how far into the frame we are. */
	TIMER_CTRL(1) = 0x0100;
	SetDispMask(1);
}

static void read_pad(uint16_t *now_out, uint16_t *pressed_out) {
	pad_poll(pad_buffers[0]);
	PADTYPE *pad = (PADTYPE *)pad_buffers[0];
	uint16_t now = 0;
	if (pad->stat == 0 && pad->type != 0)
		now = ~pad->btn;
	/* An analog pad: the left stick walks too (its stronger axis, past a
	 * dead zone). */
	if (pad->stat == 0 && (pad->type == 0x5 || pad->type == 0x7)) {
		int x = pad->ls_x - 128, y = pad->ls_y - 128;
		if (x * x + y * y > 72 * 72) {
			if (x * x > y * y) now |= x > 0 ? PAD_RIGHT : PAD_LEFT;
			else now |= y > 0 ? PAD_DOWN : PAD_UP;
		}
	}
	*pressed_out = now & ~held;
	*now_out = now;
	held = now;
}

/* ---- Screens ------------------------------------------------------------------------------ */

static int title_ready;    /* past "PRESS START" */
static int ticked;         /* the game moved on this frame */
static int slow_t;         /* the soul fell: slow motion for a while */
static int pending = UI_NOTHING;   /* waiting for a fade-out to finish */
static int paused;
static int story_t;        /* frames on the opening or memory screen */
static int memory_done;    /* the memory is all written out */

/* To the card, with any trials met since. */
static void save_now(void) {
	record.trials |= trials;
	save_write();
}
/* Screen changes of our own, alongside the menus' UI_* ones. */
enum { GO_GAME = 100, GO_MEMORY, GO_TITLE };

static void go_title(void) {
	set_video(1);
	music_ambience(0);
	game_title_scene();   /* the floor behind it */
	screen = SCREEN_TITLE;
	title_ready = 0;
	menu_open(MENU_NONE);
	fade_to(0, 6);
	music_play(SONG_TITLE, 120);
}

/* At power-on: the studio's mark, then the title. */
static void go_splash(void) {
	set_video(1);
	screen = SCREEN_SPLASH;
	story_t = 0;
	fade_to(0, 8);
	music_play(-1, 0);
}

/* The opening lines, white on black, before the first floor. */
static void go_opening(void) {
	screen = SCREEN_OPENING;
	story_t = 0;
	fade_to(0, 4);
	music_play(-1, 0);
}

/* After the boss: its memory. */
static void go_memory(void) {
	set_video(1);
	music_ambience(0);
	screen = SCREEN_MEMORY;
	story_t = 0;
	fade_to(0, 3);
	music_play(SONG_TITLE, 240);
}

static void go_game(void) {
	set_video(0);
	screen = SCREEN_GAME;
	paused = 0;
	game_start_floor(0);
	fade_to(0, 4);
	music_play(SONG_SLOTH, 90);
	music_ambience(1);
	record.runs++;
	trials = record.trials;
}

static void update(uint16_t now, uint16_t pressed) {
	ticked = 0;
	num_events = 0;   /* this frame's events only, even when the game waits */
	/* A screen change waits for the fade to black. */
	if (pending != UI_NOTHING) {
		if (!fade_done()) return;
		int p = pending;
		pending = UI_NOTHING;
		if (p == UI_NEW_GAME) go_opening();
		if (p == GO_GAME) go_game();
		if (p == GO_MEMORY) go_memory();
		if (p == UI_QUIT_TO_TITLE || p == GO_TITLE) go_title();
		return;
	}
	if (screen == SCREEN_SPLASH) {
		story_t++;
		/* As the sun clears the horizon: a low swell, then bells, one by one. */
		if (story_t == SPLASH_CHIME) {
			music_note(0, INST_SWELL, 45, 0x1400, 0);
			music_note(1, INST_DRONE, 33, 0x0c00, 0);
		}
		if (story_t == SPLASH_CHIME) music_note(2, INST_BELL, 57, 0x1800, -20);
		if (story_t == SPLASH_CHIME + 14) music_note(3, INST_BELL, 64, 0x1400, 20);
		if (story_t == SPLASH_CHIME + 28) music_note(4, INST_BELL, 69, 0x1200, 0);
		/* Start or Cross: straight on to the title. */
		if (story_t >= SPLASH_FRAMES || (story_t > 20 && (pressed & (PAD_START | PAD_CROSS)))) {
			pending = GO_TITLE;
			fade_to(255, story_t >= SPLASH_FRAMES ? 5 : 10);
		}
		return;
	}
	if (screen == SCREEN_OPENING || screen == SCREEN_MEMORY) {
		story_t++;
		/* A bell, far off, under the opening lines. */
		if (screen == SCREEN_OPENING && (story_t == 200 || story_t == 290)) music_voice(INST_BELL, 57, 0x1800, 0);
		/* Cross: past the opening at any time; out of a memory once it's written. */
		int done = screen == SCREEN_OPENING ? story_t > 20 : memory_done;
		if (done && (pressed & (PAD_CROSS | PAD_START))) {
			pending = screen == SCREEN_OPENING ? GO_GAME : GO_TITLE;
			fade_to(255, 6);
			if (screen == SCREEN_MEMORY) music_level(0, 40);
		}
		return;
	}
	/* Leaving the options: keep them. */
	{
		static int last_menu;
		int m = menu_current();
		if ((last_menu == MENU_OPTIONS || last_menu == MENU_CONTROLS) && m != MENU_OPTIONS && m != MENU_CONTROLS) save_now();
		last_menu = m;
	}
	if (screen == SCREEN_TITLE) {
		if (!title_ready) {
			if (pressed & (PAD_START | PAD_CROSS)) {
				title_ready = 1;
				menu_open(MENU_TITLE);
			}
			return;
		}
		int r = menu_update(now, pressed);
		if (r == UI_NEW_GAME) { pending = r; fade_to(255, 8); music_level(0, 30); }
		return;
	}
	/* The fight. */
	if (paused) {
		int r = menu_update(now, pressed);
		if (r == UI_RESUME) { paused = 0; music_level(256, 20); }
		if (r == UI_QUIT_TO_TITLE) { paused = 0; pending = r; fade_to(255, 8); music_level(0, 30); save_now(); }
		return;
	}
	/* The boss fallen and the floor silent: on to its memory. */
	if (floor_state == F_CLEARED && boss >= 0 && floor_t > 240) {
		record.bosses++;
		if (!record.best_time || (uint32_t)stats.frames < record.best_time) record.best_time = stats.frames;
		save_now();
		pending = GO_MEMORY;
		fade_to(255, 3);
		music_level(0, 80);
		return;
	}
	if ((pressed & PAD_START) && floor_state != F_DEAD && floor_state != F_BOSS_INTRO) {
		paused = 1;
		menu_open(MENU_PAUSE);
		music_level(110, 20);   /* quieter while paused */
		return;
	}
	/* The soul's fall, and a perfect dodge, play at a third (inputs carry
	 * over). */
	static uint16_t carried;
	if (slow_t > 0 && slow_t-- % 3) {
		carried |= pressed;
		return;
	}
	Input in = { settings_map(now), settings_map(pressed | carried) };
	carried = 0;
	game_update(&in);
	ticked = 1;
	/* Low on health: a heartbeat, quicker as it falls, and the music
	 * pulled back under it. */
	{
		static int beat_t, low;
		int now_low = player.hp > 0 && player.hp <= 30 && player.state != P_DEAD && player.state != P_FALL;
		if (now_low != low) { low = now_low; music_level(low ? 140 : 256, 40); beat_t = 0; }
		if (low) {
			int period = 36 + player.hp;   /* frames between beats */
			if (beat_t == 0) music_voice(INST_PULSE, 38, 0x3400, 0);
			if (beat_t == 11) music_voice(INST_PULSE, 36, 0x2400, 0);
			if (++beat_t >= period) beat_t = 0;
		}
	}
	for (int i = 0; i < num_events; i++) {
		if (game_events[i].type == EV_DEATH) {
			slow_t = 90;
			music_level(0, 150);   /* the music dies away */
			record.deaths++;
			save_now();
		}
		if (game_events[i].type == EV_RISE) music_play(SONG_SLOTH, 120);
		if (game_events[i].type == EV_PERFECT && slow_t < 30) slow_t = 30;
		if (game_events[i].type == EV_FINISHER && slow_t < 36) slow_t = 36;   /* the finishing thrust, slowed */
		if (game_events[i].type == EV_BOSS) music_play(SONG_BOSS, 20);   /* its name carved: its theme */
	}
	audio_events();
}

int main(int argc, const char **argv) {
	boot_magic = 0x51D50001;
	init();
	go_splash();

	int fps_frames = 0, fps_start = VSync(-1);
	for (;;) {
		Buffer *b = &buffers[ui_hires ? 0 : active];
		uint16_t now, pressed;
		read_pad(&now, &pressed);
		update(now, pressed);
		music_update();

		ClearOTagR(b->ot, OT_LEN);
		uint8_t *prim = b->prims, *end = b->prims + PRIM_LEN - 1024;
		/* The fade first in the top slot: drawn over everything after it there,
		 * the HUD too. */
		ui_begin(b->ot, prim, end);
		fade_draw();
		prim = ui_end();
		hud_quiet = paused;
		if (screen == SCREEN_GAME) prim = draw_frame(b->ot, prim, end, frame_count, ticked);
		if (screen == SCREEN_TITLE) {
			game_title_tick();
			prim = draw_title_scene(b->ot, prim, end, frame_count);
		}
		ui_begin(b->ot, prim, end);
		if (screen == SCREEN_TITLE) {
			if (!title_ready && (frame_count & 32))
				text(320, 360, "PRESS START", FONT_LARGE, 200, 186, 160, 1, 1);
			menu_draw(frame_count);
			if (save_state < 0)
				text(320, 460, "NO MEMORY CARD IN SLOT 1: NOTHING WILL BE SAVED", FONT_SMALL, 150, 110, 100, 1, 1);
			title_draw(frame_count, menu_current() == MENU_TITLE || !title_ready);
		} else if (screen == SCREEN_SPLASH) {
			splash_draw(story_t);
		} else if (screen == SCREEN_OPENING) {
			opening_draw(story_t);
		} else if (screen == SCREEN_MEMORY) {
			memory_done = memory_draw(story_t, guilt);
		} else if (paused) {
			menu_draw(frame_count);
		}
		ui_end();

		frame_count++;
		fps_frames++;
		int vnow = VSync(-1);
		if (vnow - fps_start >= 60) {
			fps = fps_frames * 60 / (vnow - fps_start);
			fps_frames = 0;
			fps_start = vnow;
		}

		cpu_only = (TIMER_VALUE(1) * 100) / 263;    /* building the frame */
		DrawSync(0);
		cpu_usage = (TIMER_VALUE(1) * 100) / 263;   /* ... and the GPU done with the last: one frame is 263 lines */
		VSync(0);
		TIMER_VALUE(1) = 0;
		PutDispEnv(&b->disp);
		PutDrawEnv(&b->draw);
		DrawOTag(b->ot + OT_LEN - 1);
		if (!ui_hires) active ^= 1;
	}
	return 0;
}
