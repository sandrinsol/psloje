/*
 * GRID SOULS: a souls-like on a shrinking grid, for the PlayStation.
 *
 * Controls: D-pad turns, Circle dodges one tile (the way you face, or the
 * way a held direction points, keeping your facing), X attacks, Square
 * blocks (a press just before a hit parries), Triangle drinks a flask.
 *
 * Runs at 60 fps: one game frame per VSync.
 *
 * Variables the test harness reads by name (see ps1test.json):
 *   frame_count, cpu_usage, fps
 *   player (game.h), wave, enemies_alive, absolution, guilt, floor_state,
 *   drop_x, drop_y, drop_amount
 */
#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxpad.h>
#include <psxapi.h>
#include <psxetc.h>
#include <hwregs_c.h>
#include "game.h"
#include "draw.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define PRIM_LEN (48 * 1024)

typedef struct {
	DISPENV disp;
	DRAWENV draw;
	uint32_t ot[OT_LEN];
	uint8_t prims[PRIM_LEN];
} Buffer;

static Buffer buffers[2];
static int active;

volatile uint32_t frame_count;
volatile int32_t cpu_usage;
volatile int32_t fps;

static uint8_t pad_buffers[2][34];
static uint16_t held;

static void init(void) {
	ResetGraph(0);
	Colour bg;
	draw_void_colour(&bg);
	for (int i = 0; i < 2; i++) {
		SetDefDispEnv(&buffers[i].disp, i ? 0 : SCREEN_W, 0, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&buffers[i].draw, i ? SCREEN_W : 0, 0, SCREEN_W, SCREEN_H);
		setRGB0(&buffers[i].draw, bg.r, bg.g, bg.b);
		buffers[i].draw.isbg = 1;
		buffers[i].draw.dtd = 1;
	}
	PutDispEnv(&buffers[0].disp);
	PutDrawEnv(&buffers[0].draw);

	FntLoad(960, 0);
	InitGeom();   /* isin / icos */
	draw_init();

	InitPAD(pad_buffers[0], 34, pad_buffers[1], 34);
	StartPAD();
	ChangeClearPAD(0);

	/* Root counter 1 counts scanlines: how far into the frame we are. */
	TIMER_CTRL(1) = 0x0100;
	SetDispMask(1);
}

static Input read_pad(void) {
	PADTYPE *pad = (PADTYPE *)pad_buffers[0];
	Input in = { 0, 0 };
	uint16_t now = 0;
	if (pad->stat == 0 && pad->type != 0)
		now = ~pad->btn;
	in.held = now;
	in.pressed = now & ~held;
	held = now;
	return in;
}

int main(int argc, const char **argv) {
	init();
	game_start_floor(0);

	int fps_frames = 0, fps_start = VSync(-1);
	for (;;) {
		Buffer *b = &buffers[active];
		Input in = read_pad();
		game_update(&in);

		ClearOTagR(b->ot, OT_LEN);
		draw_frame(b->ot, b->prims, b->prims + PRIM_LEN - 1024, frame_count);

		frame_count++;
		fps_frames++;
		int now = VSync(-1);
		if (now - fps_start >= 60) {
			fps = fps_frames * 60 / (now - fps_start);
			fps_frames = 0;
			fps_start = now;
		}

		DrawSync(0);
		cpu_usage = (TIMER_VALUE(1) * 100) / 263;   /* one frame: 263 lines */
		VSync(0);
		TIMER_VALUE(1) = 0;
		PutDispEnv(&b->disp);
		PutDrawEnv(&b->draw);
		DrawOTag(b->ot + OT_LEN - 1);
		active ^= 1;
	}
	return 0;
}
