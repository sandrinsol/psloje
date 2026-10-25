/*
 * Everything around the fight: text in the game's serif fonts, carved stone
 * panels, fades and letterbox bars, the title screen and the menus (title,
 * pause, options, controls, credits), and the settings they change.
 *
 * Screen coordinates: 320 x 240 in the fight, 640 x 480 on the title and
 * memory screens (ui_hires).
 */
#pragma once
#include <stdint.h>

/* ---- Settings ------------------------------------------------------------------------ */

/* The actions the buttons can be mapped to (Options > Controls). */
enum { ACT_ATTACK, ACT_DODGE, ACT_JUMP, ACT_FLASK, ACT_SKILL1, ACT_SKILL2, NUM_ACTIONS };

typedef struct {
	uint8_t music, sound;          /* 0..10 */
	uint8_t slow;                  /* assist: the game at 3/4 speed */
	uint16_t keys[NUM_ACTIONS];    /* PAD_* button for each action */
} Settings;

extern Settings settings;
void settings_defaults(void);
/* Pad bits (as read) -> the game's own layout (Square attack, Circle dodge,
 * Cross jump, Triangle flask, L1 / R1 skills); the D-pad and Start pass. */
uint16_t settings_map(uint16_t pad);
/* The button an action is on now, by name ("CROSS"). */
const char *action_button(int action);

/* ---- Drawing --------------------------------------------------------------------------- */

enum { FONT_SMALL, FONT_LARGE };

extern int ui_hires;   /* 1: 640 x 480 */

void ui_init(void);
/* Start of a frame: the ordering table's first slots (0 = on top) and prims. */
void ui_begin(uint32_t *ot, uint8_t *prim, uint8_t *end);
uint8_t *ui_end(void);

int text_width(const char *s, int font);
/* align: 0 left, 1 centre, 2 right of x. layer: ordering table slot (0 = top). */
void text(int x, int y, const char *s, int font, int r, int g, int b, int align, int layer);
/* A carved stone panel: light top-left edge, dark bottom-right; alpha 0..255
 * darkens what's behind instead of covering it (0: solid). */
void panel(int x, int y, int w, int h, int layer);
void dark_box(int x, int y, int w, int h, int amount, int layer);   /* subtracts amount */
void bar(int x, int y, int w, int h, int value, int max, int r, int g, int b, int layer);
void gradient(int x, int y, int w, int h, int r0, int g0, int b0, int r1, int g1, int b1, int layer);
void logo(int cx, int y, int bright, int layer);

/* ---- Fades and bars --------------------------------------------------------------------- */

/* 0 = clear .. 255 = black; the fade moves towards the target by speed a frame. */
void fade_to(int target, int speed);
int fade_level(void);
int fade_done(void);
void fade_draw(void);
/* Letterbox bars (boss intros): 0..256 how far in. */
void letterbox(int amount);

/* ---- Screens and menus ------------------------------------------------------------------ */

enum { MENU_NONE, MENU_TITLE, MENU_PAUSE, MENU_OPTIONS, MENU_CONTROLS, MENU_CREDITS, MENU_TRIALS };
/* What a menu asks the main loop to do. */
enum { UI_NOTHING, UI_NEW_GAME, UI_RESUME, UI_QUIT_TO_TITLE };

void menu_open(int menu);
int menu_current(void);
/* held / pressed: raw pad bits. Returns UI_*. */
int menu_update(uint16_t held, uint16_t pressed);
void menu_draw(int frame);
/* The title screen's backdrop (sky, ash, logo). */
void title_draw(int frame, int show_logo);

/* The studio's mark before the title (640 x 480): a sun rising over the
 * sea, then "SaraSol Games". t: frames since shown. */
void splash_draw(int t);
#define SPLASH_FRAMES 300
#define SPLASH_CHIME  112   /* the sun clears the horizon: its chord */

/* The story (640 x 480): the opening lines, and a memory after its boss.
 * t: frames since shown. Return 1 once all of it is written. */
int opening_draw(int t);
int memory_draw(int t, int guilt);

/* The trials' names and what they ask (TRIAL_* in game.h). */
extern const char *const TRIAL_NAMES[], *const TRIAL_TEXTS[];
/* "M:SS" for a number of frames. */
void time_text(char *out, int frames);
