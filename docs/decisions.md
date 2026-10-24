# Grid Souls: decisions

What was decided on top of the [design brief](design-brief.md).

## Platform and look
- Real PS1 homebrew: C and PSn00bSDK, tested with ps1-test, 60 fps.
- Low-poly 3D, dark gothic: carved stone slabs with runes, robed figures,
  a sky gradient and set pieces per sin (Sloth: ash, sunken statues).
- The camera follows the soul from behind, high and close, and swings round
  when it turns; your own slab flashes red when it's about to be hit.
- Atmosphere: the soul carries a light, ash / embers fall, the outer ring of
  the floor breaks away when it's won.
- Hit feel: hit-stop, camera kick, bursts, synthesised sound effects.
- Tiles do more than telegraphs only on some floors, through their bosses.
- Glyphs (`tools/makeglyphs.mjs`) only for the HUD.
- HUD: health and stamina bars, flasks, absolution on top; boss name and
  health at the bottom during boss fights.
- English.

## Controls
| Button | Action |
|---|---|
| Left / Right | Turn a quarter (no time), relative to the soul |
| Down | Turn around |
| Circle | Dodge one tile forward: the only way to move (9 frames, 6 invincible, 25 % stamina) |
| Left / Right / Down held + Circle | Dodge that way (relative) and keep your facing |
| X | Attack: windup, hit, recovery; stamina |
| Square | Block (stamina per hit); pressed within 6 frames before a hit: parry, then a riposte does 3x damage |
| Triangle | Drink a flask (takes time, you can be hit) |
| L1 / R1 | The two active skills |

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
- Music and effects: synthesised in code for the SPU.

## Milestones
1. Floor 1 prototype: 9 x 9 grid, dodge-movement, turning, sword, block and
   parry, stamina, flasks, Sloth husks with telegraphs, three waves, death and
   the absolution drop, HUD. Then rebuilt in 3D (above).
2. Then: the Unfinished (boss), memory screen, music; skills and the shop;
   the other floors.
