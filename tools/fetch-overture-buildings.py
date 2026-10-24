#!/usr/bin/env python3
# Adds the buildings OpenStreetMap is missing: downloads the Overture Maps
# buildings inside the map box and writes the ones that don't come from
# OpenStreetMap (Microsoft's footprints traced from aerial imagery) as an .osm
# file the converter reads next to data/vlora.osm.
#
#   python3 tools/fetch-overture-buildings.py [release] [out.osm]
#
# Reads only what it needs over HTTPS (Overture's public S3 bucket): each
# file's parquet footer to find the row groups covering the box, then those
# row groups. Needs pyarrow, requests and shapely (pip install ...).
# Licence: Overture buildings are ODbL; credit "Microsoft" and "Overture Maps
# Foundation" with OpenStreetMap.
import concurrent.futures as cf, io, re, sys, time
import pyarrow.parquet as pq, requests
from shapely import wkb
from xml.sax.saxutils import quoteattr

BOX = (19.450, 40.405, 19.525, 40.495)   # minlon, minlat, maxlon, maxlat (= tools/fetch-vlora.sh)
BASE = 'https://overturemaps-us-west-2.s3.amazonaws.com/'
release = sys.argv[1] if len(sys.argv) > 1 else '2026-10-21.0'
out = sys.argv[2] if len(sys.argv) > 2 else 'data/vlora-extra-buildings.osm'

def get(url, **kw):
    for attempt in range(6):
        try:
            r = requests.get(url, timeout=120, **kw)
            r.raise_for_status()
            return r
        except Exception:
            time.sleep(2 * (attempt + 1))
    raise RuntimeError('download failed: ' + url)

class HttpFile(io.RawIOBase):
    """Seekable read-only file over HTTP range requests."""
    def __init__(self, url):
        self.url, self.pos = url, 0
        self.size = int(get(url, headers={'Range': 'bytes=0-0'}).headers['Content-Range'].split('/')[1])
    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.pos
    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos
    def read(self, n=-1):
        if n < 0: n = self.size - self.pos
        if n <= 0: return b''
        data = get(self.url, headers={'Range': f'bytes={self.pos}-{self.pos + n - 1}'}).content
        self.pos += len(data)
        return data
    def readinto(self, b):
        d = self.read(len(b)); b[:len(d)] = d; return len(d)

def overlaps(xmin, ymin, xmax, ymax):
    return xmin <= BOX[2] and xmax >= BOX[0] and ymin <= BOX[3] and ymax >= BOX[1]

def scan(key):
    pf = pq.ParquetFile(HttpFile(BASE + key))
    md = pf.metadata
    names = [md.schema.column(i).path for i in range(md.num_columns)]
    ix = [names.index(n) for n in ('bbox.xmin', 'bbox.ymin', 'bbox.xmax', 'bbox.ymax')]
    groups = []
    for g in range(md.num_row_groups):
        st = [md.row_group(g).column(i).statistics for i in ix]
        if overlaps(st[0].min, st[1].min, st[2].max, st[3].max):
            groups.append(g)
    return key, groups

listing = get(f'{BASE}?list-type=2&prefix=release/{release}/theme=buildings/type=building/&max-keys=1000').text
keys = re.findall(r'<Key>([^<]*\.parquet)</Key>', listing)
if not keys:
    sys.exit(f'no Overture building files for release {release}')
with cf.ThreadPoolExecutor(6) as ex:
    hits = [(k, g) for k, g in ex.map(scan, keys) if g]
print(f'{len(keys)} files, {sum(len(g) for _, g in hits)} row groups cover the box', file=sys.stderr)

ways, nodes = [], []
nid = -1
for key, groups in hits:
    t = pq.ParquetFile(HttpFile(BASE + key)).read_row_groups(groups, columns=['id', 'bbox', 'geometry', 'sources', 'height', 'num_floors'])
    for row in t.to_pylist():
        b = row['bbox']
        if not overlaps(b['xmin'], b['ymin'], b['xmax'], b['ymax']):
            continue
        src = row['sources'] or []
        if any(s['dataset'] == 'OpenStreetMap' for s in src):
            continue   # already in data/vlora.osm
        geom = wkb.loads(row['geometry'])
        for poly in getattr(geom, 'geoms', [geom]):
            ring = list(poly.exterior.coords)
            refs = []
            for lon, lat in ring[:-1]:
                nodes.append((nid, lat, lon))
                refs.append(nid)
                nid -= 1
            refs.append(refs[0])
            tags = {'building': 'yes', 'source': src[0]['dataset'] if src else 'Overture'}
            if row['height']: tags['height'] = f"{row['height']:.1f}"
            if row['num_floors']: tags['building:levels'] = str(row['num_floors'])
            ways.append((nid, refs, tags))
            nid -= 1

with open(out, 'w', encoding='utf-8') as f:
    f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
    f.write(f'<osm version="0.6" generator="psloje fetch-overture-buildings.py (Overture {release})">\n')
    f.write(f'  <bounds minlat="{BOX[1]}" minlon="{BOX[0]}" maxlat="{BOX[3]}" maxlon="{BOX[2]}"/>\n')
    for i, lat, lon in nodes:
        f.write(f'  <node id="{i}" lat="{lat:.7f}" lon="{lon:.7f}"/>\n')
    for i, refs, tags in ways:
        f.write(f'  <way id="{i}">\n')
        f.write(''.join(f'    <nd ref="{r}"/>\n' for r in refs))
        f.write(''.join(f'    <tag k={quoteattr(k)} v={quoteattr(v)}/>\n' for k, v in tags.items()))
        f.write('  </way>\n')
    f.write('</osm>\n')
print(f'{out}: {len(ways)} buildings not in OpenStreetMap (Overture {release})', file=sys.stderr)
