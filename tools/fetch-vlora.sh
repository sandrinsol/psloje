#!/usr/bin/env bash
# Downloads Vlora (the whole urban area: the centre, Skela, the port, the
# Lungomare down to Uji i Ftohtë) from the OpenStreetMap API in 0.01-degree
# pieces (the API returns at most 50 000 nodes per request) and merges them
# into data/vlora.osm.
#   tools/fetch-vlora.sh
set -euo pipefail
cd "$(dirname "$0")/.."
# minlon minlat maxlon maxlat: the map box (also in tools/fetch-overture-buildings.py and tools/fetch-dem.py)
read -r W S E N <<< "19.450 40.405 19.525 40.495"
TMP="$(mktemp -d)"
for lat in $(awk "BEGIN{for (v=$S; v<$N-1e-9; v+=0.01) printf \"%.3f \", v}"); do
  for lon in $(awk "BEGIN{for (v=$W; v<$E-1e-9; v+=0.01) printf \"%.3f \", v}"); do
    la2=$(awk "BEGIN{v=$lat+0.01; if (v>$N) v=$N; printf \"%.3f\", v}"); lo2=$(awk "BEGIN{v=$lon+0.01; if (v>$E) v=$E; printf \"%.3f\", v}")
    for try in 1 2 3 4; do
      curl -sSf -m 180 -o "$TMP/c_${lat}_${lon}.osm" "https://api.openstreetmap.org/api/0.6/map?bbox=$lon,$lat,$lo2,$la2" && break
      sleep $((try * 3))
    done
    sleep 1
  done
done
mkdir -p data
python3 tools/osm-merge.py "$TMP" data/vlora.osm "$W,$S,$E,$N"
rm -rf "$TMP"
