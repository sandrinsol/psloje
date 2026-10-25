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

enum { SCREEN_TITLE, SCREEN_GAME };
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

	InitPAD(pad_buffers[0], 34, pad_buffers[1], 34);
	StartPAD();
	ChangeClearPAD(0);

	/* Root counter 1 counts scanlines: how far into the frame we are. */
	TIMER_CTRL(1) = 0x0100;
	SetDispMask(1);
}

static void read_pad(uint16_t *now_out, uint16_t *pressed_out) {
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

static void go_title(void) {
	set_video(1);
	screen = SCREEN_TITLE;
	title_ready = 0;
	menu_open(MENU_NONE);
	fade_to(0, 6);
	music_play(SONG_TITLE, 120);
}

static void go_game(void) {
	set_video(0);
	screen = SCREEN_GAME;
	paused = 0;
	game_start_floor(0);
	fade_to(0, 4);
	music_play(SONG_SLOTH, 90);
}

static void update(uint16_t now, uint16_t pressed) {
	ticked = 0;
	num_events = 0;   /* this frame's events only, even when the game waits */
	/* A screen change waits for the fade to black. */
	if (pending != UI_NOTHING) {
		if (!fade_done()) return;
		int p = pending;
		pending = UI_NOTHING;
		if (p == UI_NEW_GAME) go_game();
		if (p == UI_QUIT_TO_TITLE) go_title();
		return;
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
		if (r == UI_QUIT_TO_TITLE) { paused = 0; pending = r; fade_to(255, 8); music_level(0, 30); }
		return;
	}
	if ((pressed & PAD_START) && floor_state != F_DEAD) {
		paused = 1;
		menu_open(MENU_PAUSE);
		music_level(110, 20);   /* quieter while paused */
		return;
	}
	/* Assist: at 3/4 speed every fourth frame waits (inputs carry over);
	 * the soul's fall plays at a third. */
	static uint16_t carried;
	if ((settings.slow && (frame_count & 3) == 3) || (slow_t > 0 && slow_t-- % 3)) {
		carried |= pressed;
		return;
	}
	Input in = { settings_map(now), settings_map(pressed | carried) };
	carried = 0;
	game_update(&in);
	ticked = 1;
	for (int i = 0; i < num_events; i++) {
		if (game_events[i].type == EV_DEATH) { slow_t = 90; music_level(0, 150); }   /* the music dies away */
		if (game_events[i].type == EV_RISE) music_play(SONG_SLOTH, 120);
	}
	audio_events();
}

int main(int argc, const char **argv) {
	boot_magic = 0x51D50001;
	init();
	go_title();

	int fps_frames = 0, fps_start = VSync(-1);
	for (;;) {
		Buffer *b = &buffers[ui_hires ? 0 : active];
		uint16_t now, pressed;
		read_pad(&now, &pressed);
		update(now, pressed);
		music_update();

		ClearOTagR(b->ot, OT_LEN);
		uint8_t *prim = b->prims, *end = b->prims + PRIM_LEN - 1024;
		if (screen == SCREEN_GAME) prim = draw_frame(b->ot, prim, end, frame_count, ticked);
		ui_begin(b->ot, prim, end);
		fade_draw();   /* first in the top slot: over everything */
		if (screen == SCREEN_TITLE) {
			if (!title_ready && (frame_count & 32))
				text(320, 360, "PRESS START", FONT_LARGE, 200, 186, 160, 1, 1);
			menu_draw(frame_count);
			title_draw(frame_count, menu_current() == MENU_TITLE || !title_ready);
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
