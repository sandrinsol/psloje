# Map data

`vlora.osm`: central Vlora, Albania, from [OpenStreetMap](https://www.openstreetmap.org)
(downloaded 2026-10-24 through the OpenStreetMap API with `tools/fetch-vlora.sh`).
The game uses the box 19.474–19.500 E, 40.418–40.475 N: Sheshi i Flamurit,
the old town, the port, and the Lungomare down to Uji i Ftohtë.

© OpenStreetMap contributors. The data is available under the
[Open Database License (ODbL) 1.0](https://opendatacommons.org/licenses/odbl/1-0/);
see <https://www.openstreetmap.org/copyright>. The game's `MAP.BIN` is a
Produced Work made from it and must credit OpenStreetMap.

To refresh it: `tools/fetch-vlora.sh`, then rebuild. To improve the game's
accuracy, improve OpenStreetMap: most Vlora buildings have no `building:levels`
or `height`, so the converter guesses (see `build/map-report.json`).
