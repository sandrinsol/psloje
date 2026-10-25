/*
 * The memory card (slot 1): one block, "GRID SOULS", with the settings, the
 * trials met, the best time and a few totals.
 */
#pragma once
#include <stdint.h>

typedef struct {
	uint32_t trials;       /* bit per TRIAL_* met */
	uint32_t best_time;    /* frames to beat floor I, 0: never */
	uint16_t runs, bosses; /* trials begun, bosses beaten */
	uint32_t deaths;
} Record;

extern Record record;
extern volatile int32_t save_state;   /* 0 none yet, 1 loaded, 2 written, -1 no card / failed */

/* Reads the save into record and settings (if there is one). */
int save_load(void);
/* Writes record and settings (blocks for a moment). */
int save_write(void);
