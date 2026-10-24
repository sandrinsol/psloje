# GRID SOULS (PS1)

A souls-like for the original PlayStation, played in real time on an empty
grid that shrinks floor by floor. You are a damned soul fighting through the
seven sins of an afterlife trial. See [docs/design-brief.md](docs/design-brief.md)
and [docs/decisions.md](docs/decisions.md).

**Status: Floor 1 prototype.** The 9 x 9 floor of Sloth: three waves of husks
whose every blow lights up the tiles it will hit (faint, then bright just
before it lands). Move by dodging, turn with the D-pad, block, parry and
riposte, drink flasks, die, rise again and fetch the absolution you dropped.
60 fps. Placeholder glyphs; no boss, music or skills yet.

## Controls

| Button | Action | Keyboard (web player) |
|---|---|---|
| D-pad | Turn in place | arrow keys |
| Circle | Dodge one tile the way you face | Z |
| Direction + Circle | Dodge that way, keep facing | arrow + Z |
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

`node tools/makeglyphs.mjs` regenerates `assets/glyphs.tim` and
`src/glyphs.h` after changing the glyph art.

## Code

- `src/game.c`: the rules: player (turning, dodges, sword, block and parry,
  stamina, flasks), husks (moving, telegraphed slam / line / cross attacks,
  stagger), waves, death and the absolution drop.
- `src/draw.c`: the void, the grid, telegraphs, glyphs, the HUD.
- `src/main.c`: 60 fps loop and pad input.
- `tests/ps1/*.json`: ps1-test scripts (moving and facing, death and the
  drop, parry and riposte), run on every push.
