# GRID SOULS (PS1)

A souls-like for the original PlayStation, played in real time on an empty
grid that shrinks floor by floor. You are a damned soul fighting through the
seven sins of an afterlife trial. See [docs/design-brief.md](docs/design-brief.md)
and [docs/decisions.md](docs/decisions.md).

**Status: Floor 1 prototype, in 3D.** A title screen (the name carved in
stone, 640 x 480), menus (pause, options with volume, a slower game speed
as an assist, button remapping, credits), a carved
stone HUD in the game's own serif lettering, fades and a slow zoom onto the
floor as it begins. The 9 x 9 floor of Sloth: carved rune
slabs floating over a sea of ash, sunken statues in the haze, ash falling,
clouds drifting, a great eye watching from the sky, lightning now and then.
The figures are textured and lit by the GTE: the sky, the soul's light,
flashes from parries and hits, the red of a telegraph under them; soft round
shadows. Killed husks shatter into stone and embers; when the soul falls the
game slows, its light goes out and the colour drains away.
A fixed camera looks at the whole floor from the south, high and tilted. Three waves: husks that raise
their arms over their heads, and the runes of the slabs they're about to
hit glow, then burn (your own slab flashes red); a sleeper slumped on the
floor that wakes with a roar when you come near (a blow in its sleep does
double); a bell-ringer that keeps to the floor's edge, lines up with you and
tolls its bell to bring a whole row down. Walk with the D-pad, dodge with
Circle; the soul always faces the nearest husk. Jump over the blows that
hit the ground, strike, or hold to charge a heavy blow that breaks a husk,
then riposte it for triple damage; drink flasks, die, rise again and fetch
the absolution you dropped. Hit-stop, camera kicks, ash bursts and
synthesised sound effects. When the floor is won its outer ring breaks away.
Music synthesised for the SPU (a choir, a drone, bells, a heartbeat; a
title theme and Sloth's floor), whispers and sighs, everything in the SPU's
hall reverb. After the third wave, the boss: **the Unfinished**, a hollow
giant of half-made things, rises out of the ash (letterbox, the camera
sweeping in, its name carved, its theme) and fights with blows that come in
two halves (a ring then a wider ring; every other row, then the rest
"tomorrow"; a rope lashed out to the edge), faster below half its life.
Then Memory I, written out over a picture of the bell tower in the rain.
The story (docs/story.md) opens with a few lines before the first floor.
Nine trials to meet (riposte a broken husk, destroy a sleeper in its sleep,
silence a bell-ringer before it tolls, jump ten blows, and destroying the
Unfinished without dying, without a flask, within five minutes, untouched),
announced as they're met and listed under TRIALS on the title screen; the
memory screen shows the time, deaths, ripostes and flasks, and the best
time. Everything is kept on the memory card in slot 1 (one block, with its
own icon): settings, trials, best time, totals.
60 fps. No skills or later floors yet.

## Controls

| Button | Action | Keyboard (web player) |
|---|---|---|
| D-pad or left stick | Walk a tile at a time (Up = away from the camera) | arrow keys |
| Circle + direction | Dodge that way (invincible frames, stamina) | arrow + Z |
| Circle | Dodge away from the enemy you face | Z |
| (automatic) | Face the nearest enemy, along a row or column | |
| Square | Attack (sword: the tile in front); hold to charge a heavy blow | S |
| X | Jump in place: ground attacks miss while you're in the air (stamina) | X |
| Triangle | Drink a flask | A |
| X (when judged) | Rise again | X |
| Start | Pause (options, return to title) | Enter |

The action buttons can be changed in Options > Controls. A press while busy
is kept for 12 frames; a dodge or jump cuts a charge or the end of a swing,
and any action cuts a walk.

## Play it

Every push builds the disc on GitHub: open the latest run of the
[ps1-tests workflow](../../actions/workflows/ps1-tests.yml), download
**grid-souls-disc** from its Summary page, then go to
**https://ps1-test.vercel.app**, click **Load disc or EXE** and pick the zip.

## Build

Needs PSn00bSDK (see `.devcontainer/install-psn00bsdk.sh`) and Node.js:

    cmake --preset default && cmake --build build

Generated assets (committed; rerun after changing them):
`node tools/makeglyphs.mjs` (HUD glyphs), `python3 tools/makefont.py`
(lettering and the logo, from Liberation Serif, see docs/credits.md), `node tools/makeslabs.mjs` (the
floor's rune slabs and their glow colours), `node tools/makefigtex.mjs`
(textures for the figures, clouds, the eye, stone chunks, glows), `node tools/makemusic.mjs [preview dir]` (instruments, voices and songs; WAV previews), `node tools/makesfx.mjs` (sound
effects, synthesised and encoded as SPU ADPCM).

## Code

- `src/game.c`: the rules: player (facing, walking, dodges, jumps, sword
  and charged blow, stamina, flasks), husks (moving, telegraphed slam / line / cross attacks,
  stagger), waves, death and the absolution drop.
- `src/draw.c`: the 3D scene: the fixed camera, the sky (clouds, the eye,
  lightning), the slabs (soul light, flashes, telegraph glow, fog, the
  crumbling ring), statues, textured figures lit by the GTE, shadows, ash,
  sparks and stone chunks, the death's grading, the HUD.
- `src/models.c`: the soul, the husk and the statues, built from boxes,
  prisms and cones (materials, rounded normals); parts hang from parents
  (arms and heads from the body).
- `src/anim.c`: keyframed animations (idle, walk, dodge, jump, charge,
  swings, drink, death; the husk's lumber, windup, slam, broken reel),
  eased between keys and blended from one to the next.
- `src/audio.c`: sound effects on SPU voices, from game events.
- `src/save.c`: the memory card, spoken to directly a frame at a time
  (the directory, one save block with its title and icon).
- `src/music.c`: the sequencer (one SPU voice a track, one tick a frame),
  fades, the hall reverb, the voices (whisper, sigh, choir swell).
- `src/ui.c`: lettering, carved panels, fades, the title screen and the
  menus, settings and button mapping.
- `src/main.c`: the screens (title at 640 x 480, the fight at 320 x 240),
  60 fps loop and pad input.
- `tests/ps1/*.json`: ps1-test scripts (moving and facing, death and the
  drop, jumping a slam, the heavy blow, cancels and the analog stick, the
  sleeper and the bell-ringer, the boss and its memory, the save surviving a
  reset), run on every push.
