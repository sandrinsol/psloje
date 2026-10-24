/*
 * The taxi job: game clock and day/night light, fares (passengers, the
 * meter, money), and the HUD (clock, money, meter, street name, minimap,
 * destination arrow).
 */
#pragma once
#include <stdint.h>
#include "world.h"
#include "car.h"

enum {
	FARE_NONE,      /* looking for the next passenger */
	FARE_PICKUP,    /* a passenger is waiting at fare_from */
	FARE_RIDE,      /* on the way to fare_to, meter running */
	FARE_PAID,      /* just paid: showing the amount */
};

extern volatile int32_t game_minutes;  /* minutes since midnight */
extern volatile int32_t money;         /* Lek */
extern volatile int32_t fare_state, meter, fares_done, last_fare, last_tip;
extern volatile int32_t fare_from, fare_to;   /* place indices */

void game_init(void);
/* ticks = 1/60 s steps since the last call. */
void game_update(const Car *car, int ticks);
/* Markers for the passenger and destination, headlights at night. */
uint8_t *game_draw_world(const Car *car, const Camera *cam, uint32_t *ot, uint8_t *prim);
uint8_t *game_draw_hud(const Car *car, const Camera *cam, uint32_t *ot, uint8_t *prim);
