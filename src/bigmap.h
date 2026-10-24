/*
 * The big map (Triangle): the whole of the city, north up, with the sea,
 * the roads, the taxi, the target and the route. The game waits while it's
 * open. L1 / R1 zoom, the D-pad moves it.
 */
#pragma once
#include <stdint.h>
#include "car.h"
#include "world.h"

extern volatile int32_t bigmap_open;   /* read by the tests */

/* Opens it centred on the car. */
void bigmap_show(const Car *car);
void bigmap_hide(void);
/* held / pressed: pad buttons (PAD_*) down now / just pressed. */
void bigmap_input(uint16_t held, uint16_t pressed);
uint8_t *bigmap_draw(const Car *car, const Place *target, int pickup, uint32_t *ot, uint8_t *prim, uint8_t *end);
