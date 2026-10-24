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
//   natural=coastline              the sea (ground on its right) and collision
//   building outlines              collision edges (split into <= 32 m pieces)
//
// Terrain: --dem (tools/fetch-dem.py) lifts the ground, roads and areas, and
// stands buildings on their lowest corner. --extra adds more .osm files (e.g.
// the buildings OSM is missing, from tools/fetch-overture-buildings.py).
//
// Coordinates: 1 game unit = 1/16 m. X = east, Z = north, Y = down (PS1 GTE
// convention, so heights are negative). The world is cut into 64 m tiles; each
// tile's vertices are relative to its south-west corner so they fit in 16 bits.
//
// MAP.BIN layout (little-endian, every tile starts on a 2048-byte sector):
//   header  'VMAP' u16 version, u16 tilesX, u16 tilesZ, u16 tileUnits,
//           u16 headerSectors, u16 maxTileSectors, i32 spawnX, i32 spawnZ,
//           i16 spawnHeading (4096 = full turn, 0 = north, clockwise),
//           u16 globalSectors, char name[32], u32 globalSector,
//           then per tile: u32 sector, u16 sectors, u16 pad
//   tile    u16 nverts, nfaces, nedges, nflat, i16 minX, minZ, maxX, maxZ,
//           i16 minY (tallest point, negative), u16 flags (1 = height grid),
//           u16 ngroups, u16 pad
//           verts  nverts x { i16 x, y, z, pad }
//           faces  nfaces x { u16 v[4], u8 u0, v0, u1, v1, u8 tex|layer<<4,
//                             u8 r, g, b }   (v[3] == 0xffff: triangle)
//           edges  nedges x { i16 x0, z0, x1, z1 }
//           height 9 x 9 i16: ground Y every 8 m (row = z), for the car
//           groups ngroups x { i16 cx, cz, radius, topY, bottomY (units),
//                              u16 first, nFine, nCoarse }: one per building,
//                              its fine faces then its coarse ones (nCoarse 0:
//                              use the fine ones), all after the flat faces
//   The first nflat faces lie on the ground (drawn first, by layer); the rest
//   are depth-sorted.
//   global  places, minimap road segments and names (see "Places" below)

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
const extraPaths = (opt('--extra') || '').split(',').filter(Boolean);
const demPath = opt('--dem');
const [inPath, outPath] = args;
if (!inPath || !outPath) {
  console.error('usage: osm2map.mjs <map.osm> <out.bin> [--extra more.osm,...] [--dem dem.json] [--spawn "<name>"] [--name LABEL] [--report out.json]');
  process.exit(2);
}

// ---- OSM XML ------------------------------------------------------------

const xml = readFileSync(inPath, 'utf8');
const unescape = (s) => s.replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
const attrs = (s) => { const o = {}; for (const m of s.matchAll(/([\w:]+)="([^"]*)"/g)) o[m[1]] = unescape(m[2]); return o; };
const tagsOf = (body) => { const t = {}; for (const m of body.matchAll(/<tag\s+([^>]*?)\/>/g)) { const a = attrs(m[1]); t[a.k] = a.v; } return t; };

const nodes = new Map();
const nodeTags = new Map();
const ways = new Map();
const relations = [];
// The main file, then extra ones (e.g. buildings OSM is missing; their ids must not clash).
for (const text of [xml, ...extraPaths.map((p) => readFileSync(p, 'utf8'))]) {
  for (const m of text.matchAll(/<node\s+([^>]*?)(\/>|>([\s\S]*?)<\/node>)/g)) {
    const a = attrs(m[1]);
    nodes.set(a.id, [+a.lat, +a.lon]);
    if (m[3]) nodeTags.set(a.id, tagsOf(m[3]));
  }
  for (const m of text.matchAll(/<way\s+([^>]*?)>([\s\S]*?)<\/way>/g)) {
    const a = attrs(m[1]);
    const refs = [...m[2].matchAll(/<nd\s+ref="([^"]+)"/g)].map((r) => r[1]);
    ways.set(a.id, { id: a.id, refs, tags: tagsOf(m[2]) });
  }
  for (const m of text.matchAll(/<relation\s+([^>]*?)>([\s\S]*?)<\/relation>/g)) {
    const members = [...m[2].matchAll(/<member\s+([^>]*?)\/>/g)].map((x) => attrs(x[1]));
    relations.push({ id: attrs(m[1]).id, members, tags: tagsOf(m[2]) });
  }
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
// The OSM API returns whole ways, reaching past the requested box: keep only
// what lies inside it (with a margin for vertices of things that straddle it).
const inside = (x, z, margin = 0) => x >= -margin && z >= -margin && x <= widthM + margin && z <= heightM + margin;
const stats0 = { outside: 0 };

// Ground height in metres above the sea (0 without a DEM), bilinear in the
// grid made by tools/fetch-dem.py.
const dem = demPath ? JSON.parse(readFileSync(demPath, 'utf8')) : null;
function elev(x, z) {
  if (!dem) return 0;
  const lat = minLat + z / mPerLat, lon = minLon + x / mPerLon;
  const r = Math.max(0, Math.min(dem.rows - 1.001, (dem.north - lat) / dem.stepLat - 0.5));
  const c = Math.max(0, Math.min(dem.cols - 1.001, (lon - dem.west) / dem.stepLon - 0.5));
  const r0 = Math.floor(r), c0 = Math.floor(c), fr = r - r0, fc = c - c0;
  const at = (rr, cc) => dem.decimetres[rr * dem.cols + cc] / 10;
  return (at(r0, c0) * (1 - fc) + at(r0, c0 + 1) * fc) * (1 - fr) + (at(r0 + 1, c0) * (1 - fc) + at(r0 + 1, c0 + 1) * fc) * fr;
}

// ---- Geometry collection, per tile ----------------------------------------

const tiles = Array.from({ length: tilesX * tilesZ }, (_, i) => ({ tx: i % tilesX, tz: Math.floor(i / tilesX), verts: [], vmap: new Map(), flat: [], solid: [], groups: [], edges: [] }));
const tileAt = (x, z) => {
  const tx = Math.min(tilesX - 1, Math.max(0, Math.floor(x / TILE_M)));
  const tz = Math.min(tilesZ - 1, Math.max(0, Math.floor(z / TILE_M)));
  return tiles[tz * tilesX + tx];
};
// Vertex in tile-local units; shared when identical.
const vert = (t, x, y, z) => {
  if (![x, y, z].every(Number.isFinite)) throw new Error(`bad vertex ${x}, ${y}, ${z}`);
  const lx = Math.round((x - t.tx * TILE_M) * UNIT), ly = Math.round(-y * UNIT), lz = Math.round((z - t.tz * TILE_M) * UNIT);
  if (Math.abs(lx) > 32000 || Math.abs(lz) > 32000 || Math.abs(ly) > 32000) throw new Error('vertex out of 16-bit range');
  const key = `${lx},${ly},${lz}`;
  let i = t.vmap.get(key);
  if (i === undefined) { i = t.verts.length; t.verts.push([lx, ly, lz]); t.vmap.set(key, i); }
  return i;
};
// A point on the ground (metres): vertex at the terrain height.
const groundVert = (t, x, z) => vert(t, x, elev(x, z), z);
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
  if (!inside(cx, cz) || poly.some(([x, z]) => !inside(x, z, 200))) { stats0.outside++; return; }
  const t = tileAt(cx, cz);
  // On a slope the building stands on its lowest corner; the hill covers the rest.
  const ground = Math.min(...poly.map(([x, z]) => elev(x, z)));
  const wallCol = parseColour(tags['building:colour'] || tags.colour);
  if (wallCol) stats.colouredBuildings++;
  const wallTex = (tags['building:colour'] || tags.colour) ? TEX.wallA : [TEX.wallA, TEX.wallB, TEX.wallC][hash(id) % 3];
  const roofCol = parseColour(tags['roof:colour']) || [[150, 80, 60], [130, 130, 128], [170, 160, 150]][hash(id + 'r') % 3];
  // Each building is a group the game culls in one test (bounding sphere),
  // with fine faces (8 m x 2-floor textured panels) for up close and coarse
  // ones (one quad per wall) for far away.
  const group = { fine: [], coarse: [], cx, cz, r: Math.max(...poly.map(([x, z]) => Math.hypot(x - cx, z - cz))), top: ground + h, bottom: ground + minH };
  t.groups.push(group);

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
    group.coarse.push({ v: [vert(t, x0, ground + h, z0), vert(t, x1, ground + h, z1), vert(t, x0, ground + minH, z0), vert(t, x1, ground + minH, z1)], uv: [0, 0, 63, 63], tex: wallTex, layer: 0, rgb });
    for (let c = 0; c < cols; c++) {
      const a = c / cols, bb = (c + 1) / cols;
      const ax = x0 + (x1 - x0) * a, az = z0 + (z1 - z0) * a, bx = x0 + (x1 - x0) * bb, bz = z0 + (z1 - z0) * bb;
      const uSpan = Math.min(63, Math.round(len / cols / PANEL_W * 63));
      for (let r = 0; r < rows; r++) {
        const y0 = minH + (h - minH) * r / rows, y1 = minH + (h - minH) * (r + 1) / rows;
        const vSpan = Math.min(63, Math.round((y1 - y0) / (FLOOR_H * PANEL_FLOORS) * 63));
        // Seen from outside (to the right of a counter-clockwise outline), the
        // a end is on the left: TL, TR, BL, BR = a top, b top, a bottom, b bottom.
        group.fine.push({ v: [vert(t, ax, ground + y1, az), vert(t, bx, ground + y1, bz), vert(t, ax, ground + y0, az), vert(t, bx, ground + y0, bz)], uv: [0, 63 - vSpan, uSpan, 63], tex: wallTex, layer: 0, rgb });
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
    const roof = { v: [vert(t, poly[a][0], ground + h, poly[a][1]), vert(t, poly[c][0], ground + h, poly[c][1]), vert(t, poly[bb][0], ground + h, poly[bb][1]), 0xffff], uv: [0, 0, 0, 0], tex: TEX.none, layer: 0, rgb: roofCol };
    group.fine.push(roof);
    group.coarse.push(roof);
  }
}

// Flat polygon on the ground (park, water…), untextured.
function flatArea(poly, layer, rgb) {
  if (poly.length < 3) return;
  if (area2(poly) < 0) poly = poly.slice().reverse();
  stats.areas++;
  for (const [a, bb, c] of triangulate(poly)) {
    const cx = (poly[a][0] + poly[bb][0] + poly[c][0]) / 3, cz = (poly[a][1] + poly[bb][1] + poly[c][1]) / 3;
    if (!inside(cx, cz) || [a, bb, c].some((i) => !inside(poly[i][0], poly[i][1], 300))) continue;
    const t = tileAt(cx, cz);
    addFace(t, true, { v: [groundVert(t, poly[a][0], poly[a][1]), groundVert(t, poly[c][0], poly[c][1]), groundVert(t, poly[bb][0], poly[bb][1]), 0xffff], uv: [0, 0, 0, 0], tex: TEX.none, layer, rgb });
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
      if (!inside((ax + bx) / 2, (az + bz) / 2)) continue;
      const t = tileAt((ax + bx) / 2, (az + bz) / 2);
      const vSpan = Math.min(63, Math.round(len / n / ROAD_PIECE * 63));
      // Seen from above, travelling along the road: far-left, far-right, near-left, near-right.
      addFace(t, true, { v: [groundVert(t, bx + px, bz + pz), groundVert(t, bx - px, bz - pz), groundVert(t, ax + px, az + pz), groundVert(t, ax - px, az - pz)], uv: [0, 0, 63, vSpan], tex, layer, rgb: [128, 128, 128] });
    }
  }
  // Joints: an octagon at every inner node fills the gap where segments turn.
  for (let i = 1; i + 1 < pts.length; i++) joint(pts[i], half, layer, tex);
}

function joint([x, z], r, layer, tex) {
  if (!inside(x, z)) return;
  const t = tileAt(x, z);
  const p = Array.from({ length: 8 }, (_, k) => [x + r * Math.cos(k * Math.PI / 4), z + r * Math.sin(k * Math.PI / 4)]);
  const v = p.map(([px, pz]) => groundVert(t, px, pz));
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

// Coastline (natural=coastline): OSM draws it with the land on the left. Split
// into <= 32 m pieces, bucketed in 64 m cells for the land/sea test, and used
// as collision edges so the taxi stays out of the sea.
const coast = [], coastGrid = new Map();
const COAST_CELL = 64;
for (const w of ways.values()) {
  if (w.tags.natural !== 'coastline') continue;
  const pts = wayPts(w);
  for (let i = 0; i + 1 < pts.length; i++) {
    const [x0, z0] = pts[i], [x1, z1] = pts[i + 1];
    const n = Math.max(1, Math.ceil(Math.hypot(x1 - x0, z1 - z0) / EDGE_PIECE));
    for (let k = 0; k < n; k++) {
      const sg = [x0 + (x1 - x0) * k / n, z0 + (z1 - z0) * k / n, x0 + (x1 - x0) * (k + 1) / n, z0 + (z1 - z0) * (k + 1) / n];
      const id = coast.push(sg) - 1;
      for (let cz = Math.floor(Math.min(sg[1], sg[3]) / COAST_CELL); cz <= Math.floor(Math.max(sg[1], sg[3]) / COAST_CELL); cz++)
        for (let cx = Math.floor(Math.min(sg[0], sg[2]) / COAST_CELL); cx <= Math.floor(Math.max(sg[0], sg[2]) / COAST_CELL); cx++) {
          const key = cx + ',' + cz;
          if (!coastGrid.has(key)) coastGrid.set(key, []);
          coastGrid.get(key).push(id);
        }
      const mx = (sg[0] + sg[2]) / 2, mz = (sg[1] + sg[3]) / 2;
      if (inside(mx, mz)) {
        const et = tileAt(mx, mz);
        et.edges.push(sg.map((v, j) => Math.round((v - (j % 2 ? et.tz : et.tx) * TILE_M) * UNIT)));
      }
    }
  }
}
stats.coastKm = coast.reduce((s, c) => s + Math.hypot(c[2] - c[0], c[3] - c[1]), 0) / 1000;

// Distance to the coast and which side: { d (metres), sea }.
function coastAt(x, z) {
  if (!coast.length) return { d: Infinity, sea: false };
  const gx = Math.floor(x / COAST_CELL), gz = Math.floor(z / COAST_CELL);
  let best = null;
  for (let r = 0; r < 60; r++) {
    if (best && (r - 1) * COAST_CELL > best.d) break;
    for (let cz = gz - r; cz <= gz + r; cz++) for (let cx = gx - r; cx <= gx + r; cx++) {
      if (Math.max(Math.abs(cx - gx), Math.abs(cz - gz)) !== r) continue;
      for (const id of coastGrid.get(cx + ',' + cz) || []) {
        const [x0, z0, x1, z1] = coast[id];
        const dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz || 1e-9;
        const u = Math.max(0, Math.min(1, ((x - x0) * dx + (z - z0) * dz) / l2));
        const d = Math.hypot(x0 + dx * u - x, z0 + dz * u - z);
        const cross = (dx * (z - z0) - dz * (x - x0)) / Math.sqrt(l2);   // > 0: left = land
        // At a shared end point several pieces are equally close: trust the one
        // the point is most squarely beside.
        if (!best || d < best.d - 1e-6 || (Math.abs(d - best.d) <= 1e-6 && Math.abs(cross) > Math.abs(best.cross))) best = { d, cross };
      }
    }
  }
  return best ? { d: best.d, sea: best.cross < 0 } : { d: Infinity, sea: false };
}

// Ground: a grid of quads under everything: textured land, flat-coloured sea.
// Cells near the coast are classified in 4 m strips so the shore follows it.
const SEA = [46, 88, 128];
function groundQuad(t, x0, z0, w, h, sea) {
  addFace(t, true, {
    v: sea ? [vert(t, x0, 0, z0 + h), vert(t, x0 + w, 0, z0 + h), vert(t, x0, 0, z0), vert(t, x0 + w, 0, z0)]
           : [groundVert(t, x0, z0 + h), groundVert(t, x0 + w, z0 + h), groundVert(t, x0, z0), groundVert(t, x0 + w, z0)],
    uv: sea ? [0, 0, 0, 0] : [0, 0, Math.round(w / GROUND_CELL * 63), Math.round(h / GROUND_CELL * 63)],
    tex: sea ? TEX.none : TEX.ground, layer: LAYER.ground, rgb: sea ? SEA : [128, 128, 128],
  });
}
let seaCells = 0;
for (const t of tiles) {
  for (let gz = 0; gz < TILE_M; gz += GROUND_CELL) for (let gx = 0; gx < TILE_M; gx += GROUND_CELL) {
    const x0 = t.tx * TILE_M + gx, z0 = t.tz * TILE_M + gz;
    const c = coastAt(x0 + GROUND_CELL / 2, z0 + GROUND_CELL / 2);
    if (c.d > GROUND_CELL * 0.75) { groundQuad(t, x0, z0, GROUND_CELL, GROUND_CELL, c.sea); seaCells += c.sea; continue; }
    const S = 4;
    for (let sz = 0; sz < GROUND_CELL; sz += S) {
      let run = 0, runSea = null;
      for (let sx = 0; sx <= GROUND_CELL; sx += S) {
        const sea = sx < GROUND_CELL ? coastAt(x0 + sx + S / 2, z0 + sz + S / 2).sea : null;
        if (sea !== runSea && runSea !== null) { groundQuad(t, x0 + sx - run, z0 + sz, run, S, runSea); run = 0; }
        runSea = sea;
        run += S;
      }
    }
  }
}
stats.seaCells = seaCells;

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
// Drivable road segments (metres), for snapping, the minimap and street names.
const drivable = [];
for (const w of ways.values()) {
  const k = w.tags.highway && w.tags.highway.replace(/_link$/, '');
  if (!k || FOOT.has(k) || !ROAD_WIDTH[k] || w.tags.area === 'yes') continue;
  const pts = wayPts(w);
  for (let i = 0; i + 1 < pts.length; i++)
    if (inside((pts[i][0] + pts[i + 1][0]) / 2, (pts[i][1] + pts[i + 1][1]) / 2)) drivable.push({ a: pts[i], b: pts[i + 1], name: w.tags.name || '', kind: k });
}
function nearestRoad([x, z]) {
  let best = null;
  for (const { a: [x0, z0], b: [x1, z1] } of drivable) {
    const dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz || 1;
    const u = Math.max(0, Math.min(1, ((x - x0) * dx + (z - z0) * dz) / l2));
    const px = x0 + dx * u, pz = z0 + dz * u, d = Math.hypot(px - x, pz - z);
    if (!best || d < best.d) best = { d, p: [px, pz], heading: Math.atan2(dx, dz) };
  }
  return best;
}
// Move the spawn to the nearest road point so the taxi starts on a street.
{
  const best = nearestRoad(spawn);
  if (best) { spawn = best.p; spawn.heading = best.heading; }
}

// ---- Places, minimap roads, names ------------------------------------------------
//
// Global section (read into RAM once): u16 nplaces, nsegs, nnames, ntiles,
//   places  nplaces x { i32 x, z (world units, on the nearest road), u16 name, u8 kind, u8 pad }
//   buckets ntiles x { u16 first, count }   (segments sorted by tile)
//   segs    nsegs x { i16 x0, z0, x1, z1 (metres), u16 name (0xffff none), u8 class, u8 pad }
//   names   nnames x u16 offset (from the start of the name text), then the text (NUL-ended)
// Names are in the PS1 font's character set: upper case ASCII (e and c with
// accents, as in Albanian, become E and C).

const ascii = (s) => s.normalize('NFD').replace(/[\u0300-\u036f]/g, '').replace(/[^\x20-\x7e]/g, '').toUpperCase().trim().slice(0, 27);
const names = [], nameIds = new Map();
const nameId = (s) => { const a = ascii(s); if (!a) return 0xffff; if (!nameIds.has(a)) { nameIds.set(a, names.length); names.push(a); } return nameIds.get(a); };

// Place kinds: 0 = destination, 1 = fuel station, 2 = car repair.
const PLACE_KEYS = ['amenity', 'shop', 'tourism', 'office', 'leisure', 'historic', 'public_transport', 'craft', 'healthcare'];
const placeKind = (t) => t.amenity === 'fuel' ? 1 : t.shop === 'car_repair' || t.craft === 'car_repair' ? 2 : 0;
const places = [], seenPlace = new Set();
function addPlace(tags, pt) {
  const isPlace = tags.name && (PLACE_KEYS.some((k) => tags[k]) || (tags.building && tags.building !== 'yes'));
  const kind = placeKind(tags);
  if (!pt || (!isPlace && !kind) || !inside(pt[0], pt[1])) return;
  const road = nearestRoad(pt);
  if (!road || road.d > 60) return;            // too far from a street to drive to
  const label = tags.name || (kind === 1 ? 'Fuel' : 'Mechanic');
  const key = ascii(label) + Math.round(road.p[0] / 20) + ',' + Math.round(road.p[1] / 20);
  if (seenPlace.has(key)) return;
  seenPlace.add(key);
  places.push({ x: Math.round(road.p[0] * UNIT), z: Math.round(road.p[1] * UNIT), name: nameId(label), kind });
}
for (const [id, t] of nodeTags) addPlace(t, toXZ(id));
for (const w of ways.values()) if (w.tags.name || placeKind(w.tags)) {
  const pts = closed(w) ? ring(w) : wayPts(w);
  if (pts.length) addPlace(w.tags, [pts.reduce((s, p) => s + p[0], 0) / pts.length, pts.reduce((s, p) => s + p[1], 0) / pts.length]);
}

// Minimap / street-name segments, split to <= 32 m and bucketed by tile.
const segs = [];
for (const { a: [x0, z0], b: [x1, z1], name, kind } of drivable) {
  const n = Math.max(1, Math.ceil(Math.hypot(x1 - x0, z1 - z0) / EDGE_PIECE));
  const cls = ['motorway', 'trunk', 'primary'].includes(kind) ? 2 : ['secondary', 'tertiary'].includes(kind) ? 1 : 0;
  for (let k = 0; k < n; k++) {
    const ax = x0 + (x1 - x0) * k / n, az = z0 + (z1 - z0) * k / n, bx = x0 + (x1 - x0) * (k + 1) / n, bz = z0 + (z1 - z0) * (k + 1) / n;
    segs.push({ c: [ax, az, bx, bz].map(Math.round), name: name ? nameId(name) : 0xffff, cls, tile: tileAt((ax + bx) / 2, (az + bz) / 2) });
  }
}
segs.sort((a, b) => (a.tile.tz * tilesX + a.tile.tx) - (b.tile.tz * tilesX + b.tile.tx));
if (places.length > 4000 || segs.length > 20000 || names.length > 4000) throw new Error('too many places, road segments or names');

function globalBlob() {
  const text = names.map((n) => Buffer.from(n + '\0', 'latin1'));
  const textBytes = text.reduce((s, b) => s + b.length, 0);
  const size = 8 + places.length * 12 + tiles.length * 4 + segs.length * 12 + names.length * 2 + textBytes;
  const buf = Buffer.alloc((size + 3) & ~3);
  let o = 0;
  for (const v of [places.length, segs.length, names.length, tiles.length]) { buf.writeUInt16LE(v, o); o += 2; }
  for (const p of places) { buf.writeInt32LE(p.x, o); buf.writeInt32LE(p.z, o + 4); buf.writeUInt16LE(p.name, o + 8); buf.writeUInt8(p.kind, o + 10); o += 12; }
  const first = new Array(tiles.length).fill(0), count = new Array(tiles.length).fill(0);
  segs.forEach((sg, i) => { const t = sg.tile.tz * tilesX + sg.tile.tx; if (!count[t]) first[t] = i; count[t]++; });
  for (let t = 0; t < tiles.length; t++) { buf.writeUInt16LE(first[t], o); buf.writeUInt16LE(count[t], o + 2); o += 4; }
  for (const sg of segs) { sg.c.forEach((v, k) => buf.writeInt16LE(v, o + k * 2)); buf.writeUInt16LE(sg.name, o + 8); buf.writeUInt8(sg.cls, o + 10); o += 12; }
  let off = 0;
  for (const b of text) { buf.writeUInt16LE(off, o); o += 2; off += b.length; }
  for (const b of text) { b.copy(buf, o); o += b.length; }
  return buf;
}
const global = globalBlob();
const MAX_GLOBAL_BYTES = 160 * 1024;  // must match GLOBAL_BYTES in src/world.c
if (global.length > MAX_GLOBAL_BYTES) throw new Error(`places/roads/names take ${global.length} bytes (max ${MAX_GLOBAL_BYTES})`);

// ---- Write ----------------------------------------------------------------------

const tileBlobs = [];
let maxVerts = 0, maxBytes = 0, worst = null;
for (const t of tiles) {
  const flat = t.flat.sort((a, b) => a.layer - b.layer);
  if (t.solid.length) t.groups.push({ fine: t.solid, coarse: t.solid, cx: (t.tx + 0.5) * TILE_M, cz: (t.tz + 0.5) * TILE_M, r: TILE_M, top: 100, bottom: 0 });
  const faces = [...flat];
  const groupRecs = t.groups.map((g) => {
    const first = faces.length;
    faces.push(...g.fine, ...(g.coarse === g.fine ? [] : g.coarse));
    return [Math.round((g.cx - t.tx * TILE_M) * UNIT), Math.round((g.cz - t.tz * TILE_M) * UNIT), Math.ceil(g.r * UNIT) + 1,
      Math.round(-g.top * UNIT), Math.round(-g.bottom * UNIT), first, g.fine.length, g.coarse === g.fine ? g.fine.length : g.coarse.length, g.coarse === g.fine ? 1 : 0];
  });
  if (faces.length > 65535) throw new Error(`tile ${t.tx},${t.tz} has too many faces`);
  if (t.verts.length > MAX_TILE_VERTS) throw new Error(`tile ${t.tx},${t.tz} has ${t.verts.length} vertices (max ${MAX_TILE_VERTS}): lower the detail`);
  // Ground height every 8 m (9 x 9 over the tile, in units, Y down) for the car.
  const hm = [];
  for (let j = 0; j <= 8; j++) for (let i = 0; i <= 8; i++) hm.push(Math.round(-elev(t.tx * TILE_M + i * 8, t.tz * TILE_M + j * 8) * UNIT));
  const size = 24 + t.verts.length * 8 + faces.length * 16 + t.edges.length * 8 + hm.length * 2 + groupRecs.length * 16;
  if (size > MAX_TILE_BYTES) throw new Error(`tile ${t.tx},${t.tz} is ${size} bytes (max ${MAX_TILE_BYTES}): lower the detail`);
  const buf = Buffer.alloc(size);
  let minX = 32767, minZ = 32767, maxX = -32768, maxZ = -32768, minY = 0;
  for (const [x, y, z] of t.verts) { minX = Math.min(minX, x); maxX = Math.max(maxX, x); minZ = Math.min(minZ, z); maxZ = Math.max(maxZ, z); minY = Math.min(minY, y); }
  let o = 0;
  for (const v of [t.verts.length, faces.length, t.edges.length, flat.length]) { buf.writeUInt16LE(v, o); o += 2; }
  for (const v of [minX, minZ, maxX, maxZ, minY, 1]) { buf.writeInt16LE(v, o); o += 2; }   // 1: has a height grid
  buf.writeUInt16LE(groupRecs.length, o); o += 4;
  for (const [x, y, z] of t.verts) { buf.writeInt16LE(x, o); buf.writeInt16LE(y, o + 2); buf.writeInt16LE(z, o + 4); o += 8; }
  for (const f of faces) {
    for (let k = 0; k < 4; k++) buf.writeUInt16LE(f.v[k] ?? 0xffff, o + k * 2);
    f.uv.forEach((u, k) => buf.writeUInt8(u, o + 8 + k));
    buf.writeUInt8(f.tex | (f.layer << 4), o + 12);
    f.rgb.forEach((c, k) => buf.writeUInt8(clampByte(c), o + 13 + k));
    o += 16;
  }
  for (const e of t.edges) { e.forEach((v, k) => buf.writeInt16LE(v, o + k * 2)); o += 8; }
  for (const v of hm) { buf.writeInt16LE(v, o); o += 2; }
  for (const [gx, gz, r, top, bottom, first, nfine, ncoarse, shared] of groupRecs) {
    for (const v of [gx, gz, r, top, bottom]) { buf.writeInt16LE(v, o); o += 2; }
    // Coarse faces follow the fine ones; 0 = use the fine ones.
    buf.writeUInt16LE(first, o); buf.writeUInt16LE(nfine, o + 2); buf.writeUInt16LE(shared ? 0 : ncoarse, o + 4);
    o += 6;
  }
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
const globalSector = sector, globalSectors = Math.ceil(global.length / SECTOR);
sector += globalSectors;
header.writeUInt16LE(2, 4);
header.writeUInt16LE(tilesX, 6);
header.writeUInt16LE(tilesZ, 8);
header.writeUInt16LE(TILE, 10);
header.writeUInt16LE(headerSectors, 12);
header.writeUInt16LE(maxSectors, 14);
header.writeInt32LE(Math.round(spawn[0] * UNIT), 16);
header.writeInt32LE(Math.round(spawn[1] * UNIT), 20);
header.writeInt16LE(Math.round(((spawn.heading ?? 0) / (2 * Math.PI)) * 4096) & 4095, 24);
header.writeUInt16LE(globalSectors, 26);
header.write(mapLabel.toUpperCase().slice(0, 31), 28, 'latin1');
header.writeUInt32LE(globalSector, 60);
index.forEach(([s, n], i) => { header.writeUInt32LE(s, 64 + i * 8); header.writeUInt16LE(n, 68 + i * 8); });

const out = Buffer.alloc(sector * SECTOR);
header.copy(out, 0);
tileBlobs.forEach((blob, i) => blob.copy(out, index[i][0] * SECTOR));
global.copy(out, globalSector * SECTOR);
writeFileSync(outPath, out);

const report = {
  source: inPath, areaMetres: [Math.round(widthM), Math.round(heightM)], tiles: [tilesX, tilesZ],
  buildings: stats.buildings, buildingParts: stats.buildingParts, buildingsWithDefaultHeight: stats.defaultHeights,
  buildingsWithColour: stats.colouredBuildings, roads: stats.roads, roadKm: +stats.roadKm.toFixed(2), areas: stats.areas,
  streetNames: stats.names.size, spawn: { from: spawnFrom, metres: spawn.map((v) => +v.toFixed(1)) },
  largestTile: { at: worst, bytes: maxBytes, maxVerts }, fileBytes: out.length,
  places: places.length, fuelStations: places.filter((p) => p.kind === 1).length, mechanics: places.filter((p) => p.kind === 2).length,
  minimapSegments: segs.length, globalBytes: global.length,
  terrain: dem ? dem.source : 'flat', extraFiles: extraPaths,
  coastKm: +stats.coastKm.toFixed(2), seaGroundCells: stats.seaCells, featuresOutsideBox: stats0.outside,
};
if (reportPath) writeFileSync(reportPath, JSON.stringify({ ...report, streetNameList: [...stats.names].sort(), placeList: places.map((p) => names[p.name]) }, null, 2));
console.log(`osm2map: ${report.buildings} buildings (${report.buildingsWithDefaultHeight} with a guessed height), ${report.roads} roads (${report.roadKm} km), ${report.areas} areas, ${report.places} places (${report.fuelStations} fuel, ${report.mechanics} mechanics), ${tilesX}x${tilesZ} tiles, largest tile ${maxBytes} bytes, ${out.length} bytes total`);
