# Map data

`vlora.osm`: central Vlora, Albania, from [OpenStreetMap](https://www.openstreetmap.org)
(downloaded 2026-10-24 through the OpenStreetMap API with `tools/fetch-vlora.sh`).
The game uses the box 19.474–19.500 E, 40.418–40.475 N: Sheshi i Flamurit,
the old town, the port, and the Lungomare down to Uji i Ftohtë.

© OpenStreetMap contributors. The data is available under the
[Open Database License (ODbL) 1.0](https://opendatacommons.org/licenses/odbl/1-0/);
see <https://www.openstreetmap.org/copyright>. The game's `MAP.BIN` is a
Produced Work made from it and must credit OpenStreetMap.

`vlora-extra-buildings.osm`: the 5,614 buildings in the box that OpenStreetMap
doesn't have yet: Microsoft's building footprints (traced from aerial
imagery), taken from the [Overture Maps](https://overturemaps.org) buildings
release 2026-10-21.0 by `tools/fetch-overture-buildings.py`. ODbL; credit
Microsoft and the Overture Maps Foundation.

`vlora-dem.json`: ground height from the Copernicus DEM GLO-30 (30 m), made
by `tools/fetch-dem.py`. It is a surface model (buildings and trees
included), so the script removes bumps smaller than ~90 m. Copernicus DEM ©
DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018, provided
under COPERNICUS by the European Union and ESA; all rights reserved.

To refresh them: `tools/fetch-vlora.sh`, `python3 tools/fetch-overture-buildings.py`,
`python3 tools/fetch-dem.py`, then rebuild.

**Building heights** are the one thing no available source has: 53 of the
10,006 buildings have a height or floor count in OpenStreetMap, Overture has
none for Vlora, and the 30 m elevation model can't measure single buildings
(tested against the 52 known ones: no correlation). The rest get a height by
building type. Adding `building:levels` to Vlora's buildings on OpenStreetMap
(e.g. with the StreetComplete app, which asks for it) makes them exact here
after a refresh. To improve the game's
accuracy, improve OpenStreetMap: most Vlora buildings have no `building:levels`
or `height`, so the converter guesses (see `build/map-report.json`).
