# GRID SOULS (PS1)

A souls-like for the original PlayStation, played in real time on an empty
grid that shrinks floor by floor. You are a damned soul fighting through the
seven sins of an afterlife trial. See [docs/design-brief.md](docs/design-brief.md)
and [docs/decisions.md](docs/decisions.md).

**Status: Floor 1 prototype, in 3D.** The 9 x 9 floor of Sloth: carved rune
slabs floating over a sea of ash, sunken statues in the haze, ash falling.
The camera follows the soul from behind and above. Three waves of husks raise
their arms over their heads, and the runes of the slabs they're about to
hit glow, then burn (your own slab flashes red). Move by dodging, turn with
the D-pad, block, parry and riposte, drink flasks, die, rise again and fetch
the absolution you dropped. Hit-stop, camera kicks, ash bursts and
synthesised sound effects. When the floor is won its outer ring breaks away.
60 fps. No boss, music or skills yet.

## Controls

| Button | Action | Keyboard (web player) |
|---|---|---|
| Left / Right | Turn a quarter (the camera swings round) | arrow keys |
| Down | Turn around | down arrow |
| Circle | Dodge one tile forward | Z |
| Left / Right / Down + Circle | Dodge that way (relative to you), keep facing | arrow + Z |
| X | Attack (sword: the tile in front) | X |
| Square | Block; press just before a hit to parry | S |
| Triangle | Drink a flask | A |
| X (when judged) | Rise again | X |

## Play it

Every push builds the disc on GitHub: open the latest run of the
[ps1-tests workflow](../../actions/workflows/ps1-tests.yml), download
**grid-souls-disc** from its Summary page, then go to
**https://ps1-test.vercel.app**, click **Load disc or EXE** and pick the zip.

## Build

Needs PSn00bSDK (see `.devcontainer/install-psn00bsdk.sh`) and Node.js:

    cmake --preset default && cmake --build build

Generated assets (committed; rerun after changing them):
`node tools/makeglyphs.mjs` (HUD glyphs), `node tools/makeslabs.mjs` (the
floor's rune slabs and their glow colours), `node tools/makesfx.mjs` (sound
effects, synthesised and encoded as SPU ADPCM).

## Code

- `src/game.c`: the rules: player (turning, dodges, sword, block and parry,
  stamina, flasks), husks (moving, telegraphed slam / line / cross attacks,
  stagger), waves, death and the absolution drop.
- `src/draw.c`: the 3D scene: the following camera, the slabs (soul light,
  telegraph glow, fog, the crumbling ring), statues, figures through the GTE,
  ash and bursts, the HUD.
- `src/models.c`: the soul, the husk and the statues, built from boxes,
  prisms and cones and posed by turning their parts.
- `src/audio.c`: sound effects on SPU voices, from game events.
- `src/main.c`: 60 fps loop and pad input.
- `tests/ps1/*.json`: ps1-test scripts (moving and facing, death and the
  drop, parry and riposte), run on every push.
