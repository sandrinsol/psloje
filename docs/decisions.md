# Grid Souls: decisions

What was decided on top of the [design brief](design-brief.md).

## Platform and look
- Real PS1 homebrew: C and PSn00bSDK, tested with ps1-test, 60 fps.
- Low-poly 3D, dark gothic: carved stone slabs with runes, robed figures,
  a sky gradient and set pieces per sin (Sloth: ash, sunken statues).
- A camera from the south, high and tilted, that frames the fight: the
  soul, the enemies and any slab about to be struck, at least 5 tiles
  across (closer when it's all near the soul, wider as it spreads); the
  whole floor between waves and on death. Your own slab flashes red when
  it's about to be hit.
- Atmosphere: the soul carries a light, ash / embers fall, the outer ring of
  the floor breaks away when it's won. Clouds, a distant eye in the sky,
  lightning.
- Figures: small textures, Gouraud shaded, real-time lights (sky, the soul's
  light, flashes from events, telegraph red). Husks shatter into stone; the
  soul's death: slow motion, its light goes out, colour drains.
- Hit feel: hit-stop, camera kick, bursts, synthesised sound effects.
- Tiles do more than telegraphs only on some floors, through their bosses.
- Glyphs (`tools/makeglyphs.mjs`) only for the HUD.
- HUD: health and stamina bars, flasks, absolution on top; boss name and
  health at the bottom during boss fights. What a blow took stays pale on
  the health bar a moment, then drains; the stamina bar flashes red when an
  action wanted more than there was. Damage rises off the enemy hit (gold
  for a riposte), "BROKEN" over a husk a heavy blow breaks; a small health
  bar over hurt enemies.
- The soul: an ivory robe, a pale rim light from behind, a warm glow on the
  stone around it.
- Around the floor: two pillars sunk in the void, chains hanging from far
  above. The eye in the sky watches the soul and narrows while the
  Unfinished fights.
- Death: the colour drains to black and white; the flame left last time
  gutters out and a new one, the only colour, marks where the absolution
  will wait.
- English.

## Studio
- The game is built by SaraSol Games. At power-on, before the title, their
  own mark: a sun rising over the sea, a swell and three bells as it clears
  the horizon, then "SaraSol" and "GAMES" (about 5 s; Start or Cross skips).
  Original: none of the console's own start-up screens or sounds, which
  belong to its maker and come from the console's BIOS anyway.

## Controls
| Button | Action |
|---|---|
| D-pad | Walk one tile (20 frames, no stamina), screen directions |
| Circle + direction | Dodge two tiles that way, over an enemy too (15 frames, 10 invincible, 50 % stamina); one tile if the second is taken; onto nothing: the soul falls |
| Circle alone | Dodge away from the enemy you face |
| (automatic) | Face the nearest enemy along a row or column; exactly diagonal: keep facing (not mid-dodge: it turns as it lands) |
| R2 | Lock on to the next enemy; past the last, back to the nearest |
| Square | Light swings chain, three at most (20, 20, then 35; the first lands on frame 8). Held 37 frames: a heavy blow (50, breaks a husk), on release; while held, the soul can step at half pace, facing where it was |
| X | Jump in place (35 frames, airborne 4..26: ground attacks miss; 20 % stamina) |
| Triangle | Drink a flask (40 frames standing still; a hit spills it) |
| L1 / R1 | The two active skills |

Input: a press while busy is buffered (12 frames); a dodge or jump cancels a
charge or a swing's recovery (from 6 frames after a light hit, 12 after a
heavy one); any action cancels a walk (back to the tile it left if not yet
halfway, gliding there). The left stick walks like the D-pad.

Everything runs a quarter slower than at first (heavier, after playing):
the soul's steps, dodges and swings, the enemies' windups and recoveries.
Stamina refills a third faster to match a dodge's 50 %.

A perfect dodge: one started within 6 frames of a blow landing on the tile
it left. Time runs at a third for half a second, 30 % stamina comes back,
and the next blow is a riposte (3x), whoever it hits.

Backstab: a blow on an enemy's back (it faces the way it last turned:
frozen while it strikes, recovers, is broken or asleep) does double and
breaks it. A riposte on a broken husk is a finisher: the camera comes in
low and close, the thrust slowed, "FINISHED".

Taking hits: the heavy blows (the cross, the line, the bell's row, all of
the Unfinished's) knock the soul a tile away from the attacker, off the
edge if there's nothing there. What a blow takes stays pale on the bar for
3 s (rally): each blow struck in that time wins back half its damage, up to
the pale part.
While invincible the soul leaves pale afterimages.

The heavy blow lands later (frame 8 of 32), kicks the camera harder and
cracks the slab under its target; stone flies.

Falling: a step or a dodge onto nothing (off the edge, or where a slab has
gone) and the soul falls into the void: a death ("LOST TO THE VOID"); the
absolution drops on the last tile it stood on.

Block and parry were removed after playing: the jump and the charged heavy
blow took their place. The heavy blow (and later damage skills) breaks a
husk; any hit on a broken husk is a riposte, 3x damage.

## Enemies (Floor 1, Sloth)
- Husk: slow and heavy; slam (the 8 tiles around), line (4 tiles), cross
  (its row and column), every blow telegraphed on the slabs.
- Sleeper: a husk asleep until the soul comes within 2 tiles or strikes it
  (double damage in its sleep); wakes with a roar and attacks at once.
- Bell-ringer: keeps to the floor's edge, drifts along it to line up with
  the soul, and tolls its bell four times before a whole row (or column)
  comes down. Frail (40 hp).
- Waves: I a husk; II a husk and a sleeper; III two husks and a bell-ringer.
  Each rises out of the slabs as ash (50 frames, untouchable).
- Between waves, a breather (about 4 s): 25 health back, "THE EDGE
  CRUMBLES", an edge row and column crack (they tremble and glow), then
  fall one by one; anything standing on them falls too. 9 x 9, then 8 x 8,
  7 x 7, and the boss on 6 x 6. The next sin starts whole again.
- One fixed difficulty; the slower game speed assist was removed.
- The Unfinished also: unravels (both diagonals, then its row and column),
  and loose threads (a cross where the soul stands, then again where it
  went); and its rope reaches along a line (shown in purple): caught, the
  soul is hurt and dragged a tile towards it.
- Balance: tools/playtest.mjs, a bot that plays whole runs through the
  ps1-test harness and reports deaths (by wave), time and damage taken.

## Death and progression
- Each floor: waves of enemies, then the boss. Dying restarts the floor.
- Absolution drops where you die; reach it to recover it; die first and it's gone.
- Absolution is spent between floors: no stats. You buy active skills
  (e.g. Lunge, Shockwave, Blink, Ward, Mark), 3 levels each, 2 equipped (L1,
  R1); swapping keeps a skill's level. Or upgrades: more flasks, stronger flasks.
- Weapons stay separate: start with the sword; spear, axe, hammer unlocked or
  bought, chosen between floors.
- Flasks: 3 a floor, refilled when it restarts.
- Guilt (deaths) changes the story and endings only, not the difficulty.

## Title, rumble, ambience
- Behind the title (640 x 480): the floor in 3D under the eye, the soul
  standing to one side, the camera drifting slowly; ash falling over it.
- Rumble: the pad is read directly (pad.c, not the BIOS) so a DualShock's
  motors can run: hits taken, heavy blows, ripostes, kills, the edge falling,
  the soul falling, the boss's entrance and slams, a buzz on a perfect
  dodge. VIBRATION on/off in Options (saved).
- Wind and thin rain under the music while on the floor (a looped
  instrument on its own voice). At 30 health or less a heartbeat, quicker
  as it falls, and the music pulled back.
- Continue (from the last floor reached) waits for there to be more than
  one floor.

## Story and sound
- Backstory and the 7 memories: drafted by Claude, approved by the author.
- Memories between floors: text appearing over a still glyph picture.
- Music and effects: synthesised in code for the SPU. Dark ambient: a song a
  floor (choir pads, drones, bells, a heartbeat, whispers), a boss theme
  (organ, drums, choir stabs), the title theme; all through the hall reverb.
  Voices: whispers in the music, a sigh as the soul rises again, a choir
  swell on a riposte and when a floor falls silent. The music dies away when
  the soul falls and is quieter while paused.

## Story
- Drafted and approved: docs/story.md (Rowan, the bell-keeper of Saltmere).
- Floor 1: the opening lines, the Unfinished's entrance, Memory I ("A broken
  promise"), one more line with 5 or more deaths.
- The boss: the Unfinished (400 hp), after wave III; dying restarts the
  floor from wave I. Its memory, then back to the title (no floor 2 yet).

## Saves, trials, stats
- Memory card slot 1, one block ("GRID SOULS", a bell icon): settings,
  trials met, best time, runs, deaths, bosses destroyed. Read at start-up;
  written when the options are left, when the soul dies, after the boss and
  when returning to the title.
- Trials (achievements): riposte a broken husk; destroy a sleeper in its
  sleep; a bell-ringer before it tolls; jump over ten blows in one trial;
  destroy the Unfinished; without dying; without a flask; within five
  minutes; without being hit.
- Stats on the memory screen: time on the floor, deaths, ripostes, flasks;
  the best time.

## Milestones
1. Floor 1 prototype: 9 x 9 grid, dodge-movement, turning, sword, block and
   parry, stamina, flasks, Sloth husks with telegraphs, three waves, death and
   the absolution drop, HUD. Then rebuilt in 3D (above).
2. Then: the Unfinished (boss), memory screen, music; skills and the shop;
   the other floors.
