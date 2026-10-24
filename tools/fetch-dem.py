#!/usr/bin/env python3
# Terrain for the map box from the Copernicus DEM GLO-30 (30 m, public S3
# bucket), written as data/vlora-dem.json for tools/osm2map.mjs.
#
#   python3 tools/fetch-dem.py [out.json]
#
# GLO-30 is a surface model: it includes buildings and trees. A morphological
# opening (3 x 3 pixel minimum, then maximum) removes bumps narrower than
# ~90 m (buildings) while keeping the shape of hills; a light blur then
# smooths the 1 m steps. Needs numpy, tifffile and imagecodecs.
# Licence: Copernicus DEM, © DLR e.V. 2010-2014 and © Airbus Defence and Space
# GmbH 2014-2018, provided under COPERNICUS by the European Union and ESA.
import io, json, sys
import numpy as np, requests, tifffile

BOX = (19.450, 40.405, 19.525, 40.495)   # minlon, minlat, maxlon, maxlat (= tools/fetch-vlora.sh)
MARGIN = 0.004                           # degrees, so filters and edges are clean
out = sys.argv[1] if len(sys.argv) > 1 else 'data/vlora-dem.json'

lat_tile, lon_tile = int(np.floor(BOX[1])), int(np.floor(BOX[0]))
assert int(np.floor(BOX[3])) == lat_tile and int(np.floor(BOX[2])) == lon_tile, 'box spans DEM tiles'
name = f'Copernicus_DSM_COG_10_N{lat_tile:02d}_00_E{lon_tile:03d}_00_DEM'
url = f'https://copernicus-dem-30m.s3.amazonaws.com/{name}/{name}.tif'
dem = tifffile.imread(io.BytesIO(requests.get(url, timeout=300).content)).astype(np.float32)
rows, cols = dem.shape                     # row 0 = north edge (lat_tile + 1)
step_lat, step_lon = 1 / rows, 1 / cols

r0 = int((lat_tile + 1 - (BOX[3] + MARGIN)) * rows); r1 = int(np.ceil((lat_tile + 1 - (BOX[1] - MARGIN)) * rows))
c0 = int((BOX[0] - MARGIN - lon_tile) * cols);       c1 = int(np.ceil((BOX[2] + MARGIN - lon_tile) * cols))
a = dem[r0:r1, c0:c1]

def window(img, size, fn):
    pad = size // 2
    p = np.pad(img, pad, mode='edge')
    stack = [p[dy:dy + img.shape[0], dx:dx + img.shape[1]] for dy in range(size) for dx in range(size)]
    return fn(np.stack(stack), axis=0)

ground = window(window(a, 3, np.min), 3, np.max)       # opening: buildings out, hills kept
ground = window(window(ground, 3, np.mean), 3, np.mean)
ground = np.maximum(ground, 0)                          # the sea is at 0

json.dump({
    'source': name, 'licence': 'Copernicus DEM GLO-30 (c) DLR e.V. 2010-2014 and (c) Airbus Defence and Space GmbH 2014-2018, provided under COPERNICUS by the European Union and ESA',
    'north': lat_tile + 1 - r0 * step_lat, 'west': lon_tile + c0 * step_lon,
    'stepLat': step_lat, 'stepLon': step_lon, 'rows': int(ground.shape[0]), 'cols': int(ground.shape[1]),
    'decimetres': [int(round(v * 10)) for v in ground.ravel()],
}, open(out, 'w'))
print(f'{out}: {ground.shape[1]} x {ground.shape[0]} samples, {ground.min():.1f} to {ground.max():.1f} m', file=sys.stderr)
