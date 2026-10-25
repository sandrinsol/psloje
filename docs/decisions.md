# Grid Souls: decisions

What was decided on top of the [design brief](design-brief.md).

## Platform and look
- Real PS1 homebrew: C and PSn00bSDK, tested with ps1-test, 60 fps.
- Low-poly 3D, dark gothic: carved stone slabs with runes, robed figures,
  a sky gradient and set pieces per sin (Sloth: ash, sunken statues).
- A fixed camera from the south, high and tilted, showing the whole floor
  (it followed the soul at first: changed after playing); your own slab
  flashes red when it's about to be hit.
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
  health at the bottom during boss fights.
- English.

## Controls
| Button | Action |
|---|---|
| D-pad | Walk one tile (20 frames, no stamina), screen directions |
| Circle + direction | Dodge two tiles that way, over an enemy too (12 frames, 8 invincible, 25 % stamina); one tile if the second is off the floor or taken |
| Circle alone | Dodge away from the enemy you face |
| (automatic) | Face the nearest enemy along a row or column; exactly diagonal: keep facing |
| Square | Attack: windup, hit, recovery; stamina. Held 30 frames: a heavy blow (2.5x damage, staggers), on release |
| X | Jump in place (28 frames, airborne 3..21: ground attacks miss; 20 % stamina) |
| Triangle | Drink a flask (takes time, you can be hit) |
| L1 / R1 | The two active skills |

Input: a press while busy is buffered (12 frames); a dodge or jump cancels a
charge or a swing's recovery (from 6 frames after a light hit, 12 after a
heavy one); any action cancels a walk (back to the tile it left if not yet
halfway, gliding there). The left stick walks like the D-pad.

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
- One fixed difficulty (the assists: game speed in Options).

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
