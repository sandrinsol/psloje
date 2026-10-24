# Game Design Brief: Grid Souls (working title)

A souls-like action RPG with PS1-era constraints, played in real time on a fixed, empty grid. You are a damned soul fighting through 7 floors of an afterlife trial to earn redemption.

## Pillars
- **Souls-like difficulty**: telegraphed enemy patterns, learning through death, punishing but fair.
- **Real-time grid combat**: movement is tile-by-tile, but the game never pauses for turns.
- **One empty grid per floor**: no obstacles, pillars or terrain. Variety comes only from enemies and bosses.
- **Shrinking space**: each floor's grid is smaller than the last.
- **Cheap, minimal art**: ASCII / glyph style (final choice still open, see below).
- **Target 60 fps** for precise timing (PS1 max: 60 fps NTSC, 50 fps PAL).

## Story and theme
- The player is a soul that died with unresolved guilt and no memory of who they were.
- Instead of passing on, they are bound to a judgment floor floating in a void.
- Each floor represents one of the seven deadly sins. Each boss is a moment or person from the player's life.
- Beating a boss returns a memory, revealed as a short cutscene or text between floors.
- The grid never changes within a floor; the void around it (color, sky, onlookers, music) changes per floor to give each one identity.

## Floors

| # | Sin | Grid | Regular enemies | Boss | Memory |
|---|-----|------|-----------------|------|--------|
| 1 | Sloth | 9×9 | Slow, heavy; tutorial floor | The Unfinished: hollow figure of everything never done | A broken promise |
| 2 | Gluttony | 8×8 | Grow larger by eating the player's dropped healing | The Feast: bloated body that occupies many tiles | — |
| 3 | Greed | 7×7 | Claim tiles; steal absolution on hit | The Collector: marks more and more of the grid as his | — |
| 4 | Envy | 6×6 | Mirrors that copy player movement one beat later | Someone the player envied, using the player's own moveset | — |
| 5 | Obsession | 5×5 | Pull the player toward them or lock movement | The Beloved: twisted memory of someone the player couldn't let go | — |
| 6 | Wrath | 4×4 | Fast and relentless, almost no gaps between attacks | The moment the player lost control (the cause of death) | — |
| 7 | Pride | 3×3 | — | The player themselves, refusing to admit guilt | The full truth |

Notes:
- Smaller grids need simpler but faster patterns. On 3×3, a single telegraphed tile matters as much as a whole row on 9×9.
- Odd sizes (9, 7, 5, 3) have a center tile; even sizes (8, 6, 4) don't. Even floors can feel deliberately unbalanced.

## Combat (real time on a grid)

### Movement
- Moving one tile takes a short fixed time (~0.15–0.2 s). Once a step starts, it's committed.
- Inputs can be buffered during a move so controls feel responsive.
- Heavier equipment makes each step slower.

### Enemy attacks and telegraphs
- Enemies light up the tiles they are about to hit, with a windup.
  - Faint = warning, bright = about to land.
- Pattern types: lines, crosses, rings, checkerboards, tiles that chase the player.
- Some attacks are delayed or fake-outs to punish panic dodging.

### Player defense and stamina
- **Dash**: move 2 tiles at once, costs stamina, brief invincibility (the "roll").
- **Block**: stay in place, each blocked hit drains stamina.
- **Parry**: face the attacker at the exact moment its tile fires; opens a riposte.

### Player attacks (weapon shapes)
- Sword: tile directly in front.
- Spear: 2 tiles in a line.
- Axe: 3-tile arc in front.
- Hammer: cross pattern.
- Long reach dominates early floors and becomes clumsy on small grids, pushing players to rethink their build.

### Death and currency
- Currency is **absolution** (replaces "souls").
- On death, absolution drops on the tile where the player died. Reaching that tile on the next attempt recovers it. Dying again before that loses it.
- Optional twist: each death adds guilt, making the trial harder but the ending more meaningful.

## Endings
- **Accept**: acknowledge guilt and pass on.
- **Refuse**: become one of the trial's enemies.
- **Hidden**: spare or forgive certain bosses instead of destroying them.

## Art direction (cheap)
Preferred: **ASCII or glyph style**.
- Grid drawn as dots or simple tiles in a dark void.
- Enemies are characters, symbols or occult-looking glyphs; bosses are larger multi-character drawings with simple frame animation.
- Attack telegraphs shown as colored or flashing tiles/symbols (e.g. `!`, `*`, `#`).
- Each floor gets its own color palette (e.g. grey for Sloth, gold for Greed, red for Wrath).
- Atmosphere from color shifts, screen shake, sound and music rather than detailed art.

Alternatives considered: CP437-style tileset, 1-bit pixel art, pure geometric shapes, vector line art, low-poly PS1 with tiny textures.

## Open decisions
- Final art style: ASCII vs glyph vs CP437 tileset.
- Platform: real PS1 homebrew, or a PS1-inspired game for PC/web.
- Engine / language.
- Main character's backstory (determines exact bosses and memories).
- Whether the "guilt" death penalty is included.
- Leveling/stats system and how absolution is spent.

## Suggested first milestone
A playable prototype of Floor 1: one 9×9 grid, player movement with timing, dash, one weapon, one regular enemy type with telegraphed attacks, and absolution drop on death.
