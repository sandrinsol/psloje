#!/usr/bin/env bash
# Downloads central Vlora (Sheshi i Flamurit, the port, the Lungomare down to
# Uji i Ftohtë) from the OpenStreetMap API in 1.2 km pieces (the API returns at
# most 50 000 nodes per request) and merges them into data/vlora.osm.
#   tools/fetch-vlora.sh
set -euo pipefail
cd "$(dirname "$0")/.."
BOX="19.474,40.418,19.500,40.475"   # minlon,minlat,maxlon,maxlat: what the game uses
TMP="$(mktemp -d)"
for lat in 40.415 40.427 40.439 40.451 40.463; do
  for lon in 19.465 19.477 19.489; do
    la2=$(awk "BEGIN{printf \"%.3f\", $lat+0.012}"); lo2=$(awk "BEGIN{printf \"%.3f\", $lon+0.012}")
    curl -sSf -m 180 -o "$TMP/c_${lat}_${lon}.osm" "https://api.openstreetmap.org/api/0.6/map?bbox=$lon,$lat,$lo2,$la2"
    sleep 1
  done
done
mkdir -p data
python3 tools/osm-merge.py "$TMP" data/vlora.osm "$BOX"
rm -rf "$TMP"
