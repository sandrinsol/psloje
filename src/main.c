/*
 * VLORA TAXI: drive a Mercedes W124 taxi through Vlora, Albania, built from
 * OpenStreetMap data (tools/osm2map.mjs) and streamed from the CD.
 *
 * Drive with R2 / Cross / Up (gas), L2 / Down (brake, reverse), the D-pad
 * or left stick (steer) and Square (handbrake). Pick up passengers and take them where they want
 * to go (game.c). The day goes by at a minute a second.
 *
 * Variables the test harness reads by name (see ps1test.json):
 *   frame_count    incremented once per drawn frame (lag detection)
 *   cpu_usage      % of the 30 fps frame budget (two VSyncs) the last frame used
 *   taxi           the car (position, speed, heading: see car.h)
 *   speed_kmh, fps, tiles_loaded, tiles_drawn, faces_drawn, cd_errors
 *   game_minutes, money, fare_state, meter, fares_done (game.h)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>
#include <psxpad.h>
#include <psxapi.h>
#include <psxetc.h>
#include <hwregs_c.h>
#include "world.h"
#include "car.h"
#include "game.h"

#define SCREEN_W 320
#define SCREEN_H 240
#define PRIM_LEN (112 * 1024)

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
volatile int32_t speed_kmh;
volatile int32_t fps;
Car taxi;
static Camera camera;

static uint8_t pad_buffers[2][34];
static uint16_t held;
static int stick_x, has_stick;
static int map_ok;

static void init(void) {
	ResetGraph(0);
	for (int i = 0; i < 2; i++) {
		SetDefDispEnv(&buffers[i].disp, i ? 0 : SCREEN_W, 0, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&buffers[i].draw, i ? SCREEN_W : 0, 0, SCREEN_W, SCREEN_H);
		setRGB0(&buffers[i].draw, fog_colour.r, fog_colour.g, fog_colour.b);
		buffers[i].draw.isbg = 1;
		buffers[i].draw.dtd = 1;
	}
	ClearOTagR(buffers[0].ot, OT_LEN);
	ClearOTagR(buffers[1].ot, OT_LEN);
	PutDispEnv(&buffers[0].disp);
	PutDrawEnv(&buffers[0].draw);

	FntLoad(960, 0);

	InitGeom();
	gte_SetGeomOffset(SCREEN_W / 2, SCREEN_H / 2);
	gte_SetGeomScreen(200);

	InitPAD(pad_buffers[0], 34, pad_buffers[1], 34);
	StartPAD();
	ChangeClearPAD(0);

	/* Root counter 1 counts scanlines: how far into the frame we are. */
	TIMER_CTRL(1) = 0x0100;

	SetDispMask(1);
}

static void read_pad(void) {
	PADTYPE *pad = (PADTYPE *)pad_buffers[0];
	held = 0;
	has_stick = 0;
	if (pad->stat != 0 || pad->type == 0)
		return;
	held = ~pad->btn;
	if ((pad->type == PAD_ID_ANALOG_STICK || pad->type == PAD_ID_ANALOG) && pad->len >= 3) {
		int lx = pad->ls_x - 128;
		if (lx < -16 || lx > 16) {
			stick_x = lx < 0 ? (lx + 16) * 4096 / 112 : (lx - 16) * 4096 / 111;
			has_stick = 1;
		}
	}
}

int main(int argc, const char **argv) {
	init();
	map_ok = world_init();
	if (!map_ok) {
		/* No map on the disc: say so instead of a black screen. */
		for (;;) {
			Buffer *b = &buffers[active];
			ClearOTagR(b->ot, OT_LEN);
			FntSort(b->ot, b->prims, 40, 112, "MAP.BIN NOT FOUND ON THE DISC");
			DrawSync(0);
			VSync(0);
			PutDispEnv(&b->disp);
			PutDrawEnv(&b->draw);
			DrawOTag(b->ot + OT_LEN - 1);
			active ^= 1;
			frame_count++;
		}
	}

	int32_t sx, sz;
	int sh;
	world_spawn(&sx, &sz, &sh);
	car_reset(&taxi, sx, sz, sh);
	world_stream(sx, sz, 1);
	car_camera(&taxi, &camera, 1);
	game_init();

	int last_vsync = VSync(-1), fps_frames = 0, fps_start = last_vsync;
	for (;;) {
		Buffer *b = &buffers[active];

		/* Physics at 60 Hz whatever the frame rate. */
		int now = VSync(-1);
		int ticks = now - last_vsync;
		last_vsync = now;
		if (ticks < 1) ticks = 1;
		if (ticks > 4) ticks = 4;
		read_pad();
		CarInput in = { 0 };
		/* Gas: R2, Cross or Up; brake / reverse: L2 or Down (easier on a keyboard). */
		in.gas = !!(held & (PAD_R2 | PAD_CROSS | PAD_UP));
		in.brake = !!(held & (PAD_L2 | PAD_DOWN));
		in.handbrake = !!(held & PAD_SQUARE);
		in.steer = (held & PAD_RIGHT ? 4096 : 0) - (held & PAD_LEFT ? 4096 : 0);
		if (!in.steer && has_stick) { in.steer = stick_x; in.analog = 1; }
		for (int t = 0; t < ticks; t++) {
			car_tick(&taxi, &in);
			car_camera(&taxi, &camera, 0);
		}
		speed_kmh = car_kmh(&taxi);
		game_update(&taxi, ticks);

		world_stream(CAR_TO_WORLD(taxi.x), CAR_TO_WORLD(taxi.z), 0);

		ClearOTagR(b->ot, OT_LEN);
		uint8_t *prim = b->prims, *end = b->prims + PRIM_LEN - 4096;
		prim = world_draw(&camera, b->ot, prim, end);
		prim = car_draw(&taxi, &camera, b->ot, prim);
		prim = game_draw_world(&taxi, &camera, b->ot, prim);
		prim = game_draw_hud(&taxi, &camera, b->ot, prim);
		if (frame_count < 150) {
			/* The map's name for the first seconds. */
			prim = FntSort(b->ot, prim, 160 - 4 * (int)strlen(map_name), 76, map_name);
			/* Credits the data's licences require (ODbL, Copernicus). */
			prim = FntSort(b->ot, prim, 40, 92, "(C) OPENSTREETMAP CONTRIBUTORS");
			prim = FntSort(b->ot, prim, 28, 104, "BUILDINGS: MICROSOFT VIA OVERTURE");
			prim = FntSort(b->ot, prim, 28, 116, "TERRAIN: COPERNICUS DEM (EU, ESA)");
		}

		frame_count++;
		fps_frames++;
		if (now - fps_start >= 60) {
			fps = fps_frames * 60 / (now - fps_start);
			fps_frames = 0;
			fps_start = now;
		}

		DrawSync(0);
		cpu_usage = (TIMER_VALUE(1) * 100) / 526;   /* 30 fps: 2 x 263 lines */
		/* A steady 30 fps: show a new frame every second VSync at most. */
		static int last_flip;
		do VSync(0); while (VSync(-1) - last_flip < 2);
		last_flip = VSync(-1);
		TIMER_VALUE(1) = 0;
		PutDispEnv(&b->disp);
		setRGB0(&b->draw, fog_colour.r, fog_colour.g, fog_colour.b);   /* sky = fog, by time of day */
		PutDrawEnv(&b->draw);
		DrawOTag(b->ot + OT_LEN - 1);
		active ^= 1;
	}
	return 0;
}
