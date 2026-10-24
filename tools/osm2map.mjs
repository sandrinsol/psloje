#!/usr/bin/env node
// Converts an OpenStreetMap export (.osm XML, as saved by openstreetmap.org's
// Export button or the Overpass API) into MAP.BIN, the city the game streams
// from the CD.
//
//   node tools/osm2map.mjs <map.osm> <out.bin> [--spawn "<name>"] [--name LABEL] [--report out.json]
//
// What becomes what:
//   building=* / building:part=*   walls (textured, split into 8 m x 2-floor
//                                  panels) + a flat roof; height from height=,
//                                  building:levels= (3 m a floor) or a default
//                                  by building type; building:colour= and
//                                  roof:colour= tint them
//   highway=*                      road surface strips with joints, width from
//                                  width=, lanes= or the road class
//   park/grass/garden/pitch,       flat coloured ground areas
//   water, beach, parking
//   building outlines              collision edges (split into <= 32 m pieces)
//
// Coordinates: 1 game unit = 1/16 m. X = east, Z = north, Y = down (PS1 GTE
// convention, so heights are negative). The world is cut into 64 m tiles; each
// tile's vertices are relative to its south-west corner so they fit in 16 bits.
//
// MAP.BIN layout (little-endian, every tile starts on a 2048-byte sector):
//   header  'VMAP' u16 version, u16 tilesX, u16 tilesZ, u16 tileUnits,
//           u16 headerSectors, u16 maxTileSectors, i32 spawnX, i32 spawnZ,
//           i16 spawnHeading (4096 = full turn, 0 = north, clockwise), i16 pad,
//           char name[32], then per tile: u32 sector, u16 sectors, u16 pad
//   tile    u16 nverts, nfaces, nedges, nflat, i16 minX, minZ, maxX, maxZ,
//           i16 minY (tallest point, negative), u16 pad
//           verts  nverts x { i16 x, y, z, pad }
//           faces  nfaces x { u16 v[4], u8 u0, v0, u1, v1, u8 tex|layer<<4,
//                             u8 r, g, b }   (v[3] == 0xffff: triangle)
//           edges  nedges x { i16 x0, z0, x1, z1 }
//   The first nflat faces lie on the ground (drawn first, by layer); the rest
//   are depth-sorted.

import { readFileSync, writeFileSync } from 'node:fs';

const UNIT = 16;                 // units per metre
const TILE_M = 64;
const TILE = TILE_M * UNIT;
const SECTOR = 2048;
const PANEL_W = 8;               // metres of wall per texture repeat
const FLOOR_H = 3;               // metres per floor
const PANEL_FLOORS = 2;          // floors per texture repeat
const GROUND_CELL = 16;          // metres per ground quad
const ROAD_PIECE = 8;            // metres per road quad
const EDGE_PIECE = 32;           // metres per collision edge
const MAX_TILE_VERTS = 2400;     // must match MAX_TILE_VERTS in src/world.h
const MAX_TILE_BYTES = 48 * 1024; // must match TILE_SLOT_BYTES in src/world.h

// Texture ids: must match the order in tools/maketex.mjs.
const TEX = { asphalt: 0, paving: 1, wallA: 2, wallB: 3, wallC: 4, ground: 5, grass: 6, sand: 7, none: 15 };
// Flat layers (drawn back to front): ground, areas, footways, roads.
const LAYER = { ground: 0, area: 1, foot: 2, road: 3 };

// ---- Arguments ----------------------------------------------------------

const args = process.argv.slice(2);
const opt = (name) => { const i = args.indexOf(name); if (i < 0) return undefined; const v = args[i + 1]; args.splice(i, 2); return v; };
const spawnName = opt('--spawn');
const reportPath = opt('--report');
const mapLabel = opt('--name') || 'OSM MAP';
const [inPath, outPath] = args;
if (!inPath || !outPath) {
  console.error('usage: osm2map.mjs <map.osm> <out.bin> [--spawn "<name>"] [--report out.json]');
  process.exit(2);
}

// ---- OSM XML ------------------------------------------------------------

const xml = readFileSync(inPath, 'utf8');
const unescape = (s) => s.replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
const attrs = (s) => { const o = {}; for (const m of s.matchAll(/([\w:]+)="([^"]*)"/g)) o[m[1]] = unescape(m[2]); return o; };
const tagsOf = (body) => { const t = {}; for (const m of body.matchAll(/<tag\s+([^>]*?)\/>/g)) { const a = attrs(m[1]); t[a.k] = a.v; } return t; };

const nodes = new Map();
const nodeTags = new Map();
for (const m of xml.matchAll(/<node\s+([^>]*?)(\/>|>([\s\S]*?)<\/node>)/g)) {
  const a = attrs(m[1]);
  nodes.set(a.id, [+a.lat, +a.lon]);
  if (m[3]) nodeTags.set(a.id, tagsOf(m[3]));
}
const ways = new Map();
for (const m of xml.matchAll(/<way\s+([^>]*?)>([\s\S]*?)<\/way>/g)) {
  const a = attrs(m[1]);
  const refs = [...m[2].matchAll(/<nd\s+ref="([^"]+)"/g)].map((r) => r[1]);
  ways.set(a.id, { id: a.id, refs, tags: tagsOf(m[2]) });
}
const relations = [];
for (const m of xml.matchAll(/<relation\s+([^>]*?)>([\s\S]*?)<\/relation>/g)) {
  const members = [...m[2].matchAll(/<member\s+([^>]*?)\/>/g)].map((x) => attrs(x[1]));
  relations.push({ id: attrs(m[1]).id, members, tags: tagsOf(m[2]) });
}
if (!nodes.size) { console.error(`${inPath}: no OSM nodes found`); process.exit(1); }

// Bounds: <bounds> if present, else the nodes' extent.
let minLat, minLon, maxLat, maxLon;
const b = xml.match(/<bounds\s+([^>]*?)\/>/);
if (b) ({ minlat: minLat, minlon: minLon, maxlat: maxLat, maxlon: maxLon } = Object.fromEntries(Object.entries(attrs(b[1])).map(([k, v]) => [k, +v])));
else {
  minLat = minLon = Infinity; maxLat = maxLon = -Infinity;
  for (const [la, lo] of nodes.values()) { minLat = Math.min(minLat, la); maxLat = Math.max(maxLat, la); minLon = Math.min(minLon, lo); maxLon = Math.max(maxLon, lo); }
}

// Local metres (equirectangular around the south-west corner: < 1 cm error over a few km).
const lat0 = (minLat + maxLat) / 2;
const mPerLat = 111132.92 - 559.82 * Math.cos(2 * lat0 * Math.PI / 180);
const mPerLon = 111412.84 * Math.cos(lat0 * Math.PI / 180);
const toXZ = (id) => { const n = nodes.get(id); return n && [(n[1] - minLon) * mPerLon, (n[0] - minLat) * mPerLat]; };
const widthM = (maxLon - minLon) * mPerLon, heightM = (maxLat - minLat) * mPerLat;
const tilesX = Math.max(1, Math.ceil(widthM / TILE_M)), tilesZ = Math.max(1, Math.ceil(heightM / TILE_M));

// ---- Geometry collection, per tile ----------------------------------------

const tiles = Array.from({ length: tilesX * tilesZ }, (_, i) => ({ tx: i % tilesX, tz: Math.floor(i / tilesX), verts: [], vmap: new Map(), flat: [], solid: [], edges: [] }));
const tileAt = (x, z) => {
  const tx = Math.min(tilesX - 1, Math.max(0, Math.floor(x / TILE_M)));
  const tz = Math.min(tilesZ - 1, Math.max(0, Math.floor(z / TILE_M)));
  return tiles[tz * tilesX + tx];
};
// Vertex in tile-local units; shared when identical.
const vert = (t, x, y, z) => {
  const lx = Math.round((x - t.tx * TILE_M) * UNIT), ly = Math.round(-y * UNIT), lz = Math.round((z - t.tz * TILE_M) * UNIT);
  if (Math.abs(lx) > 32000 || Math.abs(lz) > 32000 || Math.abs(ly) > 32000) throw new Error('vertex out of 16-bit range');
  const key = `${lx},${ly},${lz}`;
  let i = t.vmap.get(key);
  if (i === undefined) { i = t.verts.length; t.verts.push([lx, ly, lz]); t.vmap.set(key, i); }
  return i;
};
const clampByte = (v) => Math.max(0, Math.min(255, Math.round(v)));
// face: { v: [a,b,c,d?], uv: [u0,v0,u1,v1], tex, layer, rgb }
const addFace = (t, flat, f) => (flat ? t.flat : t.solid).push(f);

const stats = { buildings: 0, buildingParts: 0, defaultHeights: 0, colouredBuildings: 0, roads: 0, roadKm: 0, areas: 0, skipped: 0, names: new Set() };

// Signed area (> 0 = counter-clockwise with X east, Z north).
const area2 = (p) => { let a = 0; for (let i = 0; i < p.length; i++) { const [x0, z0] = p[i], [x1, z1] = p[(i + 1) % p.length]; a += x0 * z1 - x1 * z0; } return a / 2; };

// Ear clipping for simple polygons (counter-clockwise input). Returns index triples.
function triangulate(p) {
  const idx = p.map((_, i) => i);
  const out = [];
  const cross = (a, b, c) => (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
  const inside = (pt, a, b, c) => cross(a, b, pt) >= 0 && cross(b, c, pt) >= 0 && cross(c, a, pt) >= 0;
  let guard = 0;
  while (idx.length > 3 && guard++ < 10000) {
    let cut = false;
    for (let i = 0; i < idx.length; i++) {
      const ia = idx[(i + idx.length - 1) % idx.length], ib = idx[i], ic = idx[(i + 1) % idx.length];
      const a = p[ia], bb = p[ib], c = p[ic];
      if (cross(a, bb, c) <= 0) continue;
      if (idx.some((j) => j !== ia && j !== ib && j !== ic && inside(p[j], a, bb, c))) continue;
      out.push([ia, ib, ic]);
      idx.splice(i, 1);
      cut = true;
      break;
    }
    if (!cut) break; // self-intersecting: give up on the rest
  }
  if (idx.length === 3) out.push([idx[0], idx[1], idx[2]]);
  return out;
}

const parseColour = (s) => {
  if (!s) return null;
  const named = { white: [235, 235, 230], yellow: [230, 205, 110], beige: [225, 205, 170], cream: [235, 225, 190], red: [180, 70, 60], brown: [140, 95, 70], grey: [150, 150, 150], gray: [150, 150, 150], orange: [220, 140, 70], pink: [225, 160, 160], blue: [110, 140, 190], green: [120, 160, 110], black: [50, 50, 50], silver: [190, 190, 195], gold: [210, 180, 90] };
  const h = s.trim().toLowerCase();
  if (named[h]) return named[h];
  const m = h.match(/^#?([0-9a-f]{6})$/) || h.match(/^#?([0-9a-f]{3})$/);
  if (!m) return null;
  const hex = m[1].length === 3 ? m[1].split('').map((c) => c + c).join('') : m[1];
  return [0, 2, 4].map((i) => parseInt(hex.slice(i, i + 2), 16));
};
const metres = (s) => { if (!s) return NaN; const m = String(s).match(/^\s*([\d.]+)\s*(m|ft)?/); return m ? +m[1] * (m[2] === 'ft' ? 0.3048 : 1) : NaN; };
const hash = (s) => { let h = 2166136261; for (const c of String(s)) h = Math.imul(h ^ c.charCodeAt(0), 16777619); return h >>> 0; };

// Default floors when OSM has no height: a guess by building type.
const DEFAULT_LEVELS = { house: 2, detached: 2, semidetached_house: 2, terrace: 2, garage: 1, garages: 1, shed: 1, roof: 1, kiosk: 1, hut: 1, service: 1, church: 3, mosque: 3, retail: 2, commercial: 4, apartments: 5, residential: 5, office: 5, hotel: 6, school: 3, hospital: 4, university: 4, public: 3, civic: 3, industrial: 2, warehouse: 2, yes: 4 };

function building(poly, tags, id, isPart) {
  if (poly.length < 3) return;
  if (area2(poly) < 0) poly = poly.slice().reverse();
  let h = metres(tags.height);
  const levels = parseFloat(tags['building:levels']);
  if (!(h > 0)) {
    if (levels > 0) h = levels * FLOOR_H + 1;
    else { h = (DEFAULT_LEVELS[tags.building || tags['building:part']] ?? 4) * FLOOR_H + 1; stats.defaultHeights++; }
  }
  let minH = metres(tags.min_height);
  if (!(minH >= 0)) minH = (parseFloat(tags['building:min_level']) || 0) * FLOOR_H;
  if (h - minH < 0.5) return;
  isPart ? stats.buildingParts++ : stats.buildings++;

  const cx = poly.reduce((s, p) => s + p[0], 0) / poly.length, cz = poly.reduce((s, p) => s + p[1], 0) / poly.length;
  const t = tileAt(cx, cz);
  const wallCol = parseColour(tags['building:colour'] || tags.colour);
  if (wallCol) stats.colouredBuildings++;
  const wallTex = (tags['building:colour'] || tags.colour) ? TEX.wallA : [TEX.wallA, TEX.wallB, TEX.wallC][hash(id) % 3];
  const roofCol = parseColour(tags['roof:colour']) || [[150, 80, 60], [130, 130, 128], [170, 160, 150]][hash(id + 'r') % 3];

  // Walls: outward faces of a counter-clockwise outline, vertex order TL, TR, BL, BR
  // seen from outside.
  const rows = Math.max(1, Math.round((h - minH) / (FLOOR_H * PANEL_FLOORS)));
  for (let i = 0; i < poly.length; i++) {
    const [x0, z0] = poly[i], [x1, z1] = poly[(i + 1) % poly.length];
    const len = Math.hypot(x1 - x0, z1 - z0);
    if (len < 0.2) continue;
    // Sun from the south-west, a bit above: walls facing it are brighter.
    const nx = (z1 - z0) / len, nz = -(x1 - x0) / len;
    const light = 0.72 + 0.28 * Math.max(-1, Math.min(1, -nx * 0.6 - nz * 0.8));
    const base = wallCol ? wallCol.map((c) => c / 255 * 160) : [128, 128, 128];
    const rgb = base.map((c) => clampByte(c * light));
    const cols = Math.max(1, Math.round(len / PANEL_W));
    for (let c = 0; c < cols; c++) {
      const a = c / cols, bb = (c + 1) / cols;
      const ax = x0 + (x1 - x0) * a, az = z0 + (z1 - z0) * a, bx = x0 + (x1 - x0) * bb, bz = z0 + (z1 - z0) * bb;
      const uSpan = Math.min(63, Math.round(len / cols / PANEL_W * 63));
      for (let r = 0; r < rows; r++) {
        const y0 = minH + (h - minH) * r / rows, y1 = minH + (h - minH) * (r + 1) / rows;
        const vSpan = Math.min(63, Math.round((y1 - y0) / (FLOOR_H * PANEL_FLOORS) * 63));
        // Viewed from outside the outline (to its right), "left" is the b end.
        addFace(t, false, { v: [vert(t, bx, y1, bz), vert(t, ax, y1, az), vert(t, bx, y0, bz), vert(t, ax, y0, az)], uv: [0, 63 - vSpan, uSpan, 63], tex: wallTex, layer: 0, rgb });
      }
    }
    // Collision only for walls that reach the street.
    if (minH < 2) {
      const pieces = Math.max(1, Math.ceil(len / EDGE_PIECE));
      for (let k = 0; k < pieces; k++) {
        const ax = x0 + (x1 - x0) * k / pieces, az = z0 + (z1 - z0) * k / pieces, bx = x0 + (x1 - x0) * (k + 1) / pieces, bz = z0 + (z1 - z0) * (k + 1) / pieces;
        const et = tileAt((ax + bx) / 2, (az + bz) / 2);
        et.edges.push([ax, az, bx, bz].map((v, j) => Math.round((v - (j % 2 ? et.tz : et.tx) * TILE_M) * UNIT)));
      }
    }
  }
  // Roof (seen from above: clockwise on screen = counter-clockwise in X/Z).
  for (const [a, bb, c] of triangulate(poly)) {
    addFace(t, false, { v: [vert(t, ...[poly[a][0], h, poly[a][1]]), vert(t, poly[c][0], h, poly[c][1]), vert(t, poly[bb][0], h, poly[bb][1]), 0xffff], uv: [0, 0, 0, 0], tex: TEX.none, layer: 0, rgb: roofCol });
  }
}

// Flat polygon on the ground (park, water…), untextured.
function flatArea(poly, layer, rgb) {
  if (poly.length < 3) return;
  if (area2(poly) < 0) poly = poly.slice().reverse();
  stats.areas++;
  for (const [a, bb, c] of triangulate(poly)) {
    const cx = (poly[a][0] + poly[bb][0] + poly[c][0]) / 3, cz = (poly[a][1] + poly[bb][1] + poly[c][1]) / 3;
    const t = tileAt(cx, cz);
    addFace(t, true, { v: [vert(t, poly[a][0], 0, poly[a][1]), vert(t, poly[c][0], 0, poly[c][1]), vert(t, poly[bb][0], 0, poly[bb][1]), 0xffff], uv: [0, 0, 0, 0], tex: TEX.none, layer, rgb });
  }
}

const ROAD_WIDTH = { motorway: 14, trunk: 12, primary: 10, secondary: 9, tertiary: 8, unclassified: 6, residential: 6, living_street: 5, service: 4, pedestrian: 5, footway: 2.5, path: 2, steps: 2, cycleway: 2, track: 3 };
const FOOT = new Set(['pedestrian', 'footway', 'path', 'steps', 'cycleway']);

function road(pts, tags) {
  const kind = tags.highway.replace(/_link$/, '');
  let w = metres(tags.width);
  if (!(w > 0)) w = parseFloat(tags.lanes) > 0 ? parseFloat(tags.lanes) * 3.2 : ROAD_WIDTH[kind];
  if (!w) { stats.skipped++; return; }
  const foot = FOOT.has(kind);
  const layer = foot ? LAYER.foot : LAYER.road, tex = foot ? TEX.paving : TEX.asphalt;
  stats.roads++;
  if (tags.name) stats.names.add(tags.name);
  const half = w / 2;
  for (let i = 0; i + 1 < pts.length; i++) {
    const [x0, z0] = pts[i], [x1, z1] = pts[i + 1];
    const len = Math.hypot(x1 - x0, z1 - z0);
    if (len < 0.05) continue;
    stats.roadKm += len / 1000;
    const px = -(z1 - z0) / len * half, pz = (x1 - x0) / len * half; // to the left
    const n = Math.max(1, Math.ceil(len / ROAD_PIECE));
    for (let k = 0; k < n; k++) {
      const a = k / n, bb = (k + 1) / n;
      const ax = x0 + (x1 - x0) * a, az = z0 + (z1 - z0) * a, bx = x0 + (x1 - x0) * bb, bz = z0 + (z1 - z0) * bb;
      const t = tileAt((ax + bx) / 2, (az + bz) / 2);
      const vSpan = Math.min(63, Math.round(len / n / ROAD_PIECE * 63));
      // Seen from above, travelling along the road: far-left, far-right, near-left, near-right.
      addFace(t, true, { v: [vert(t, bx + px, 0, bz + pz), vert(t, bx - px, 0, bz - pz), vert(t, ax + px, 0, az + pz), vert(t, ax - px, 0, az - pz)], uv: [0, 0, 63, vSpan], tex, layer, rgb: [128, 128, 128] });
    }
  }
  // Joints: an octagon at every inner node fills the gap where segments turn.
  for (let i = 1; i + 1 < pts.length; i++) joint(pts[i], half, layer, tex);
}

function joint([x, z], r, layer, tex) {
  const t = tileAt(x, z);
  const p = Array.from({ length: 8 }, (_, k) => [x + r * Math.cos(k * Math.PI / 4), z + r * Math.sin(k * Math.PI / 4)]);
  const v = p.map(([px, pz]) => vert(t, px, 0, pz));
  // Untextured would show a different shade from the road, so use a small patch of the texture.
  for (const [a, bb, c, d] of [[3, 2, 4, 1], [4, 1, 5, 0], [5, 0, 6, 7]]) addFace(t, true, { v: [v[a], v[bb], v[c], v[d]], uv: [8, 8, 24, 24], tex, layer, rgb: [128, 128, 128] });
}

// ---- Walk the data ---------------------------------------------------------

const wayPts = (w) => w.refs.map(toXZ).filter(Boolean);
const closed = (w) => w.refs.length > 3 && w.refs[0] === w.refs[w.refs.length - 1];
const ring = (w) => wayPts(w).slice(0, -1);

const AREA_COLOURS = {
  grass: [96, 140, 70], park: [90, 135, 65], garden: [90, 135, 65], pitch: [80, 140, 80], playground: [150, 130, 100],
  water: [50, 95, 140], beach: [215, 195, 150], sand: [215, 195, 150], parking: [110, 110, 112], square: [175, 168, 155],
};
const areaKind = (t) =>
  t.natural === 'water' || t.waterway === 'riverbank' || t.water ? 'water'
  : t.natural === 'beach' || t.natural === 'sand' ? 'beach'
  : t.leisure === 'park' || t.leisure === 'garden' ? 'park'
  : t.leisure === 'pitch' ? 'pitch'
  : t.leisure === 'playground' ? 'playground'
  : t.landuse === 'grass' || t.landuse === 'recreation_ground' || t.landuse === 'village_green' || t.natural === 'grassland' ? 'grass'
  : t.amenity === 'parking' ? 'parking'
  : t.place === 'square' || (t.highway === 'pedestrian' && t.area === 'yes') ? 'square'
  : null;

for (const w of ways.values()) {
  const t = w.tags;
  if (t.building && t.building !== 'no' && closed(w)) building(ring(w), t, w.id, false);
  else if (t['building:part'] && t['building:part'] !== 'no' && closed(w)) building(ring(w), t, w.id, true);
  else if (closed(w) && areaKind(t)) flatArea(ring(w), LAYER.area, AREA_COLOURS[areaKind(t)]);
  else if (t.highway && t.area !== 'yes') road(wayPts(w), t);
}
// Multipolygon buildings and areas: each outer ring on its own (inner rings ignored).
for (const r of relations) {
  if (r.tags.type !== 'multipolygon') continue;
  const outers = r.members.filter((m) => m.type === 'way' && m.role === 'outer').map((m) => ways.get(m.ref)).filter((w) => w && closed(w));
  for (const w of outers) {
    if (r.tags.building) building(ring(w), { ...r.tags, ...w.tags }, w.id, false);
    else if (areaKind(r.tags)) flatArea(ring(w), LAYER.area, AREA_COLOURS[areaKind(r.tags)]);
  }
}

// Ground: a grid of textured quads under everything.
for (const t of tiles) {
  for (let gz = 0; gz < TILE_M; gz += GROUND_CELL) for (let gx = 0; gx < TILE_M; gx += GROUND_CELL) {
    const x0 = t.tx * TILE_M + gx, z0 = t.tz * TILE_M + gz;
    addFace(t, true, { v: [vert(t, x0, 0, z0 + GROUND_CELL), vert(t, x0 + GROUND_CELL, 0, z0 + GROUND_CELL), vert(t, x0, 0, z0), vert(t, x0 + GROUND_CELL, 0, z0)], uv: [0, 0, 63, 63], tex: TEX.ground, layer: LAYER.ground, rgb: [128, 128, 128] });
  }
}

// ---- Spawn point -------------------------------------------------------------

let spawn = [widthM / 2, heightM / 2], spawnFrom = 'map centre';
if (spawnName) {
  const want = spawnName.toLowerCase();
  const hit = [...nodeTags].find(([, t]) => (t.name || '').toLowerCase() === want) ||
    [...ways.values()].map((w) => [w.id, w.tags, w]).find(([, t]) => (t.name || '').toLowerCase() === want);
  if (hit) {
    const w = ways.get(hit[0]);
    const pts = w ? wayPts(w) : [toXZ(hit[0])];
    spawn = [pts.reduce((s, p) => s + p[0], 0) / pts.length, pts.reduce((s, p) => s + p[1], 0) / pts.length];
    spawnFrom = spawnName;
  } else console.warn(`spawn "${spawnName}" not found; using the map centre`);
}
// Move the spawn to the nearest road point so the taxi starts on a street.
{
  let best = null;
  for (const w of ways.values()) {
    const k = w.tags.highway && w.tags.highway.replace(/_link$/, '');
    if (!k || FOOT.has(k) || !ROAD_WIDTH[k]) continue;
    const pts = wayPts(w);
    for (let i = 0; i + 1 < pts.length; i++) {
      const [x0, z0] = pts[i], [x1, z1] = pts[i + 1];
      const dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz || 1;
      const u = Math.max(0, Math.min(1, ((spawn[0] - x0) * dx + (spawn[1] - z0) * dz) / l2));
      const px = x0 + dx * u, pz = z0 + dz * u, d = Math.hypot(px - spawn[0], pz - spawn[1]);
      if (!best || d < best.d) best = { d, p: [px, pz], heading: Math.atan2(dx, dz) };
    }
  }
  if (best) { spawn = best.p; spawn.heading = best.heading; }
}

// ---- Write ----------------------------------------------------------------------

const tileBlobs = [];
let maxVerts = 0, maxBytes = 0, worst = null;
for (const t of tiles) {
  const flat = t.flat.sort((a, b) => a.layer - b.layer);
  const faces = [...flat, ...t.solid];
  if (t.verts.length > MAX_TILE_VERTS) throw new Error(`tile ${t.tx},${t.tz} has ${t.verts.length} vertices (max ${MAX_TILE_VERTS}): lower the detail`);
  const size = 20 + t.verts.length * 8 + faces.length * 16 + t.edges.length * 8;
  if (size > MAX_TILE_BYTES) throw new Error(`tile ${t.tx},${t.tz} is ${size} bytes (max ${MAX_TILE_BYTES}): lower the detail`);
  const buf = Buffer.alloc(size);
  let minX = 32767, minZ = 32767, maxX = -32768, maxZ = -32768, minY = 0;
  for (const [x, y, z] of t.verts) { minX = Math.min(minX, x); maxX = Math.max(maxX, x); minZ = Math.min(minZ, z); maxZ = Math.max(maxZ, z); minY = Math.min(minY, y); }
  let o = 0;
  for (const v of [t.verts.length, faces.length, t.edges.length, flat.length]) { buf.writeUInt16LE(v, o); o += 2; }
  for (const v of [minX, minZ, maxX, maxZ, minY, 0]) { buf.writeInt16LE(v, o); o += 2; }
  for (const [x, y, z] of t.verts) { buf.writeInt16LE(x, o); buf.writeInt16LE(y, o + 2); buf.writeInt16LE(z, o + 4); o += 8; }
  for (const f of faces) {
    for (let k = 0; k < 4; k++) buf.writeUInt16LE(f.v[k] ?? 0xffff, o + k * 2);
    f.uv.forEach((u, k) => buf.writeUInt8(u, o + 8 + k));
    buf.writeUInt8(f.tex | (f.layer << 4), o + 12);
    f.rgb.forEach((c, k) => buf.writeUInt8(clampByte(c), o + 13 + k));
    o += 16;
  }
  for (const e of t.edges) { e.forEach((v, k) => buf.writeInt16LE(v, o + k * 2)); o += 8; }
  tileBlobs.push(buf);
  maxVerts = Math.max(maxVerts, t.verts.length);
  if (size > maxBytes) { maxBytes = size; worst = `${t.tx},${t.tz}`; }
}

const nTiles = tiles.length;
const headerBytes = 64 + nTiles * 8;
const headerSectors = Math.ceil(headerBytes / SECTOR);
const header = Buffer.alloc(headerSectors * SECTOR);
header.write('VMAP', 0, 'ascii');
let sector = headerSectors, maxSectors = 0;
const index = tileBlobs.map((blob) => { const n = Math.ceil(blob.length / SECTOR); const e = [sector, n]; sector += n; maxSectors = Math.max(maxSectors, n); return e; });
header.writeUInt16LE(1, 4);
header.writeUInt16LE(tilesX, 6);
header.writeUInt16LE(tilesZ, 8);
header.writeUInt16LE(TILE, 10);
header.writeUInt16LE(headerSectors, 12);
header.writeUInt16LE(maxSectors, 14);
header.writeInt32LE(Math.round(spawn[0] * UNIT), 16);
header.writeInt32LE(Math.round(spawn[1] * UNIT), 20);
header.writeInt16LE(Math.round(((spawn.heading ?? 0) / (2 * Math.PI)) * 4096) & 4095, 24);
header.write(mapLabel.toUpperCase().slice(0, 31), 28, 'latin1');
index.forEach(([s, n], i) => { header.writeUInt32LE(s, 64 + i * 8); header.writeUInt16LE(n, 68 + i * 8); });

const out = Buffer.alloc(sector * SECTOR);
header.copy(out, 0);
tileBlobs.forEach((blob, i) => blob.copy(out, index[i][0] * SECTOR));
writeFileSync(outPath, out);

const report = {
  source: inPath, areaMetres: [Math.round(widthM), Math.round(heightM)], tiles: [tilesX, tilesZ],
  buildings: stats.buildings, buildingParts: stats.buildingParts, buildingsWithDefaultHeight: stats.defaultHeights,
  buildingsWithColour: stats.colouredBuildings, roads: stats.roads, roadKm: +stats.roadKm.toFixed(2), areas: stats.areas,
  streetNames: stats.names.size, spawn: { from: spawnFrom, metres: spawn.map((v) => +v.toFixed(1)) },
  largestTile: { at: worst, bytes: maxBytes, maxVerts }, fileBytes: out.length,
};
if (reportPath) writeFileSync(reportPath, JSON.stringify({ ...report, streetNameList: [...stats.names].sort() }, null, 2));
console.log(`osm2map: ${report.buildings} buildings (${report.buildingsWithDefaultHeight} with a guessed height), ${report.roads} roads (${report.roadKm} km), ${report.areas} areas, ${tilesX}x${tilesZ} tiles, largest tile ${maxBytes} bytes, ${out.length} bytes total`);
