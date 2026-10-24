# Merges OSM API chunk downloads (<dir>/*.osm) into one .osm, dropping duplicates.
# python3 tools/osm-merge.py <chunk dir> <out.osm> [minlon,minlat,maxlon,maxlat]
import glob, sys, xml.etree.ElementTree as ET
src, out = sys.argv[1], sys.argv[2]
bbox = [float(v) for v in sys.argv[3].split(',')] if len(sys.argv) > 3 else None  # minlon,minlat,maxlon,maxlat
nodes, ways, rels = {}, {}, {}
for f in sorted(glob.glob(src + '/*.osm')):
    r = ET.parse(f).getroot()
    for el in r:
        d = {'node': nodes, 'way': ways, 'relation': rels}.get(el.tag)
        if d is not None and el.get('id') not in d: d[el.get('id')] = el
root = ET.Element('osm', version='0.6', generator='psloje merge.py (OpenStreetMap API 0.6)')
lons = [float(n.get('lon')) for n in nodes.values()]; lats = [float(n.get('lat')) for n in nodes.values()]
b = bbox or [min(lons), min(lats), max(lons), max(lats)]
ET.SubElement(root, 'bounds', minlat=str(b[1]), minlon=str(b[0]), maxlat=str(b[3]), maxlon=str(b[2]))
for d in (nodes, ways, rels):
    for el in d.values(): root.append(el)
ET.ElementTree(root).write(out, encoding='UTF-8', xml_declaration=True)
print(out, len(nodes), 'nodes', len(ways), 'ways', len(rels), 'relations')
