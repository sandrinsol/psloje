# VLORA TAXI (PS1)

A taxi game for the original PlayStation, set in Vlora, Albania. You drive a
Mercedes-Benz W124 taxi through the city's real streets, built from
[OpenStreetMap](https://www.openstreetmap.org) data. Written in C with
[PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) and tested with the
[ps1-test](https://github.com/sandrinsol/ps1-test) emulator harness and MCP server.

**Status: driving, fares and day/night work in real Vlora**: Sheshi i
Flamurit, the old town, the port and the Lungomare down to Uji i Ftohtë.
Streets (232 km, with their names), 655 places, the coastline and the sea
come from OpenStreetMap; 10,006 buildings from OpenStreetMap and Microsoft's
footprints (via Overture Maps); the terrain (Kuzum Baba, the hills above the
coast road) from the Copernicus DEM. See [data/README.md](data/README.md).

| Button | Action |
|---|---|
| R2 | Accelerate |
| L2 | Brake, then reverse |
| D-pad left/right or left stick | Steer |
| Square | Handbrake |

**Fares.** A passenger waits at a place from the map (green column, green
arrow and minimap dot). Stop next to them and they get in; drive to their
destination (orange column) and stop. The meter starts at 300 Lek and adds
120 Lek per km; arriving fast without hitting walls earns a tip. The HUD
shows the clock, your money, the fare, the street you're on and a
heading-up minimap.

**Time of day.** The shift starts at 07:00 and a game minute passes every
second: the light and the sky follow, with a dusk at about 19:30 and night
from 20:30, when the headlights, tail lights and the TAXI sign glow.
## Play it

Every push builds the disc on GitHub: open the latest run of the
[ps1-tests workflow](../../actions/workflows/ps1-tests.yml), download
**vlora-taxi-disc** from its Summary page, then go to
**https://ps1-test.vercel.app**, click **Load disc or EXE** and pick that zip
(or drop it on the page). On a keyboard: arrow keys steer, **R** is R2 (gas),
**Tab** is L2 (brake), **S** is Square (handbrake). A gamepad works too.

## Plan

1. Drive around the centre: chase camera, the city streamed from the CD in
   64 m tiles, buildings you can't drive through, fog, 30 fps. **Done.**
2. Day/night cycle and the whole Lungomare out to Uji i Ftohtë. **Done.**
3. Fares: passengers, the meter, money, a minimap with the destination arrow,
   street names on the HUD. **Done.**
4. Fuel and a gas station, damage and the mechanic, saves on the memory card.
5. Traffic: AI cars, traffic lights, rules and fines.
6. Car radio and sound effects.

## Map data

`tools/osm2map.mjs` turns an OpenStreetMap export into `MAP.BIN`, which the
build puts on the disc. The build uses `data/vlora.osm` (and starts the taxi
at Sheshi i Flamurit); without it, the made-up `tools/test/testtown.osm`.
`tools/fetch-vlora.sh` downloads `data/vlora.osm` again from the OpenStreetMap
API (see [data/README.md](data/README.md)).

From the data the converter takes:

- **Buildings** (`building=*`, `building:part=*`): walls and a flat roof. Height
  from `height=` or `building:levels=` (3 m a floor); when OSM has neither, a
  guess by building type (the report counts these). `building:colour=` and
  `roof:colour=` tint them.
- **Roads** (`highway=*`): width from `width=`, `lanes=` or the road class;
  footways and pedestrian streets in paving.
- **Areas**: parks, grass, pitches, water, beaches, parking, squares.
- **Collision**: the outlines of the buildings.
- **Places** for fares: named shops, cafés, hotels, schools, offices and other
  named features within 60 m of a street (moved onto the street), plus fuel
  stations (`amenity=fuel`) and mechanics (`shop=car_repair`) for later.
- **Minimap and street names**: the drivable roads and their `name=`. Albanian
  letters become plain capitals for the PS1 font (ë → E, ç → C).

- **Sea**: `natural=coastline` (land on its left); the ground is classified as
  land or sea, in 4 m strips near the shore, and the coast is a collision edge.

- **More buildings** (`--extra data/vlora-extra-buildings.osm`): footprints
  OpenStreetMap lacks.
- **Terrain** (`--dem data/vlora-dem.json`): ground, roads and areas follow it;
  buildings stand on their lowest corner; each tile carries an 8 m height grid
  the car drives on (it tilts on slopes and gravity pulls along them).
- **Per-building culling and detail**: each building is a group with a bounding
  sphere (skipped in one test when out of view) and two sets of faces,
  textured panels up close and one quad per wall beyond 50 m.

The build writes `build/map-report.json` (buildings, guessed heights, roads,
street names, places, tile sizes). **Accuracy:** street layout, footprints,
the coast, terrain and names come from real data; building heights are the
exception (see [data/README.md](data/README.md)): 9,953 of 10,006 are guesses
by building type. Not yet handled: inner courtyards of multipolygon buildings.

Map data © OpenStreetMap contributors (ODbL), building footprints ©
Microsoft via the Overture Maps Foundation (ODbL), terrain from the Copernicus
DEM (© DLR e.V., © Airbus Defence and Space GmbH, provided under COPERNICUS by
the EU and ESA). The game shows these credits at start.

## Build

| | |
|---|---|
| Build | `cmake --preset default && cmake --build build` → `build/game.cue` + `game.bin` (bootable disc, with `MAP.BIN`), `build/game.exe`, `build/game.elf` (symbols) |
| Needs | PSn00bSDK with `PSN00BSDK_LIBS` set, a MIPS GCC (`mipsel-none-elf`, or Debian/Ubuntu's `gcc-mipsel-linux-gnu`), CMake, Ninja and Node.js — `.devcontainer/install-psn00bsdk.sh` installs the SDK and compiler |
| Play | Load `build/game.cue` in a PS1 emulator, e.g. the ps1-test web player (**Load disc or EXE**) |
| Test | `tests/ps1/smoke.json` (boot, stream the city, a fare offered, accelerate, brake, night), run by the `ps1-tests` workflow or `node <ps1-test>/harness/runner.mjs tests/ps1/smoke.json` |

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
- `src/game.c`: clock and day/night light, fares and the meter, passenger and
  destination markers, headlights, HUD, minimap, destination arrow.

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
which variables to watch. The golden images in `tests/ps1/goldens/` are
recreated on the first run; delete them when the map changes.
