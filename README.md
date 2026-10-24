# VLORA TAXI (PS1)

A taxi game for the original PlayStation, set in Vlora, Albania. You drive a
Mercedes-Benz W124 taxi through the city's real streets, built from
[OpenStreetMap](https://www.openstreetmap.org) data. Written in C with
[PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) and tested with the
[ps1-test](https://github.com/sandrinsol/ps1-test) emulator harness and MCP server.

**Status: milestone 1 (drive around the centre), running on a made-up test
town.** The real Vlora map needs an OpenStreetMap export in `data/vlora.osm`
(see [Map data](#map-data)); until then the game says `TEST TOWN (NOT VLORA)`.

| Button | Action |
|---|---|
| R2 | Accelerate |
| L2 | Brake, then reverse |
| D-pad left/right or left stick | Steer |
| Square | Handbrake |

## Plan

1. **Drive around the centre** (now): chase camera, the city streamed from the
   CD in 64 m tiles, buildings you can't drive through, fog, 30 fps.
2. The whole Lungomare out to Uji i Ftohtë, and a day/night cycle.
3. Fares: passengers, the meter, money, a minimap with the destination arrow,
   street names on the HUD.
4. Fuel and a gas station, damage and the mechanic, saves on the memory card.
5. Traffic: AI cars, traffic lights, rules and fines.
6. Car radio and sound effects.

## Map data

`tools/osm2map.mjs` turns an OpenStreetMap export into `MAP.BIN`, which the
build puts on the disc. The build uses `data/vlora.osm` when it exists (and
starts the taxi at Sheshi i Flamurit), otherwise `tools/test/testtown.osm`.

To get `data/vlora.osm`: on openstreetmap.org, find central Vlora, click
**Export**, choose **Manually select a different area**, draw a box from Sheshi
i Flamurit to the Lungomare and the port, and save the `.osm` file. For a bigger
area use the Overpass API link on the same page.

From the data the converter takes:

- **Buildings** (`building=*`, `building:part=*`): walls and a flat roof. Height
  from `height=` or `building:levels=` (3 m a floor); when OSM has neither, a
  guess by building type (the report counts these). `building:colour=` and
  `roof:colour=` tint them.
- **Roads** (`highway=*`): width from `width=`, `lanes=` or the road class;
  footways and pedestrian streets in paving.
- **Areas**: parks, grass, pitches, water, beaches, parking, squares.
- **Collision**: the outlines of the buildings.

The build writes `build/map-report.json` (buildings, guessed heights, roads,
street names, tile sizes). Not yet handled: the coastline (the sea), terrain
height, and inner courtyards of multipolygon buildings.

Map data © OpenStreetMap contributors, available under the
[Open Database License](https://www.openstreetmap.org/copyright); the game
must credit it (planned for the credits screen).

## Build

| | |
|---|---|
| Build | `cmake --preset default && cmake --build build` → `build/game.cue` + `game.bin` (bootable disc, with `MAP.BIN`), `build/game.exe`, `build/game.elf` (symbols) |
| Needs | PSn00bSDK with `PSN00BSDK_LIBS` set, a MIPS GCC (`mipsel-none-elf`, or Debian/Ubuntu's `gcc-mipsel-linux-gnu`), CMake, Ninja and Node.js — `.devcontainer/install-psn00bsdk.sh` installs the SDK and compiler |
| Play | Load `build/game.cue` in a PS1 emulator, e.g. the ps1-test web player (**Load disc or EXE**) |
| Test | `tests/ps1/smoke.json` (boot, stream the city, accelerate, brake), run by the `ps1-tests` workflow or `node <ps1-test>/harness/runner.mjs tests/ps1/smoke.json` |

Generated files, committed so the build doesn't need to regenerate them:

| File | Made by |
|---|---|
| `assets/city.tim`, `src/textures.h` | `node tools/maketex.mjs` (placeholder textures: asphalt, paving, three walls, ground, grass, sand) |
| `src/car_model.h` | `node tools/makecar.mjs` (the W124: 118 vertices, 64 faces) |
| `tools/test/testtown.osm` | `node tools/test/make-testtown.mjs` |

## Code

- `src/main.c`: main loop, pad input (60 Hz physics whatever the frame rate),
  HUD.
- `src/world.c`: streams tiles from `MAP.BIN` (one CD read at a time, 5 × 5
  tiles around the car), draws them with the GTE (textured near, fogged
  Gouraud far, faces crossing the near plane split until they fit), building
  collisions.
- `src/car.c`: W124 physics (speed-dependent steering, grip, handbrake drift,
  sliding along walls), chase camera, car drawing.

Two PS1 details worth knowing:

- **GTE scale.** The camera matrix handed to the GTE is scaled by 7. The GTE's
  perspective divide overflows for depths under H/2 (100 with H = 200), which
  squashed everything closer than 6 m at 1/16 m units; scaled, that limit is
  under 1 m. See `GTE_SCALE` in `src/world.h`.
- **SDK macro bug.** PSn00bSDK's `gte_SetRotMatrix()` overwrites `$t0`–`$t2`
  but declares `$12`/`$13` as clobbered, so the compiler can lose a value kept
  in `$t1` (it crashed `world_draw`). Matrices are loaded through
  `gte_load_matrix()`, a real function call, instead.

`ps1test.json` tells the ps1-test harness how to build and load the game and
which variables to watch. The golden image in `tests/ps1/goldens/` is
recreated on the first run; delete it when the map changes.
