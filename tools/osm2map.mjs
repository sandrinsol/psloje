#!/usr/bin/env node
// Converts an OpenStreetMap export (.osm XML, as saved by openstreetmap.org's
// Export button or the Overpass API) into MAP.BIN, the city the game streams
// from the CD.
//
//   node tools/osm2map.mjs <map.osm> <out.bin> [--spawn "<name>" [--spawn-road "<street>"]] [--name LABEL] [--report out.json]
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
//   header  'VMAP' u16 version (4), u16 tilesX, u16 tilesZ, u16 tileUnits,
//           u16 headerSectors, u16 graphSectors, i32 spawnX, i32 spawnZ,
//           i16 spawnHeading (4096 = full turn, 0 = north, clockwise),
//           u16 globalSectors, char name[32], u32 globalSector,
//           then per tile: u32 first sector << 10 | far sectors << 5 | near sectors
//   per tile: a far blob (building boxes, 32 m ground) then a near blob
//   (everything), each from a sector start, both in the tile layout below
//   tile    u16 nverts, nfaces, nedges, nflat, i16 minX, minZ, maxX, maxZ,
//           i16 minY (tallest point, negative), u16 flags (1 = height grid),
//           u16 ngroups, u16 nsegs, u16 nflatVerts, u16 pad
//           (vertices: the flat faces' first, then each group's own range)
//           verts  nverts x { i16 x, y, z, pad }
//           faces  nfaces x { u16 v[4], u8 u0, v0, u1, v1, u8 tex|layer<<6 (tex 63: none),
//                             u8 r, g, b }   (v[3] == 0xffff: triangle)
//           edges  nedges x { i16 x0, z0, x1, z1 }
//           height 9 x 9 i16: ground Y every 8 m (row = z), for the car
//           groups ngroups x { i16 cx, cz, radius, topY, bottomY (units),
//                              u16 first, nFine, nCoarse, vFirst, vCount,
//                              nRoof, pad }: one per building (its roof faces last),
//                              its fine faces then its coarse ones (nCoarse 0:
//                              use the fine ones), all after the flat faces
//           segs   nsegs x { i16 x0, z0, x1, z1 (metres), u16 name, u8 class, u8 pad }:
//                  drivable roads for the minimap and street names (near only)
//   The first nflat faces lie on the ground (drawn first, by layer); the rest
//   are depth-sorted.
//   global  places and names (see "Places" below)
//   graph   the road graph and sea bitmap (see "Road graph" below), right
//           after the global section

import { readFileSync, writeFileSync } from 'node:fs';
import { LANDMARKS, TEX as TEXTURE_IDS } from './landmarks.mjs';
import { signLines, signAtlas, SIGN_PALETTE, to15 as sign15 } from './signs.mjs';

const UNIT = 16;                 // units per metre
const TILE_M = 64;
const TILE = TILE_M * UNIT;
const SECTOR = 2048;
const PANEL_W = 8;               // metres of wall per texture repeat
const FLOOR_H = 3;               // metres per floor
const PANEL_FLOORS = 2;          // floors per texture repeat
const GROUND_CELL = 32;          // metres per ground quad (split finer at the coast and near the camera)
const ROAD_PIECE = 32;           // metres per road quad (the texture repeats every 8 m)
const ROAD_REPEAT = 8;           // metres of road per texture repeat
// Walls and roads use the GPU's texture window: a 64-pixel texture repeats up
// to 4 times across one polygon (u/v 0..255), so a wall quad covers up to
// 4 x 8 m wide and 4 x 2 floors tall.
const MAX_REPEAT = 4;
const EDGE_PIECE = 32;           // metres per collision edge
const MAX_TILE_VERTS = 2400;     // must match MAX_TILE_VERTS in src/world.h
const MAX_TILE_BYTES = 20 * 1024; // must match TILE_SLOT_BYTES in src/world.h
const MAX_FAR_BYTES = 6 * 1024;   // must match FAR_SLOT_BYTES in src/world.h

// Texture ids (by name, in tools/maketex.mjs's order; 63 = none).
const TEX = TEXTURE_IDS;
// Flat layers (drawn back to front): ground, areas, footways, roads.
const LAYER = { ground: 0, area: 1, foot: 2, road: 3 };

// ---- Arguments ----------------------------------------------------------

const args = process.argv.slice(2);
const opt = (name) => { const i = args.indexOf(name); if (i < 0) return undefined; const v = args[i + 1]; args.splice(i, 2); return v; };
const spawnName = opt('--spawn');
const spawnRoad = opt('--spawn-road');
const noLandmarks = args.includes('--no-landmarks') && args.splice(args.indexOf('--no-landmarks'), 1);   // start on this street (the one the place faces)
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

const newTile = (i) => ({ tx: i % tilesX, tz: Math.floor(i / tilesX), verts: [], vmap: new Map(), flat: [], solid: [], groups: [], edges: [], segs: [] });
const tiles = Array.from({ length: tilesX * tilesZ }, (_, i) => newTile(i));
// The far version of each tile: building boxes and coarse ground only, for the
// ring of tiles beyond the detailed ones (the view reaches ~200 m).
const farTiles = Array.from({ length: tilesX * tilesZ }, (_, i) => newTile(i));
const tileAt = (x, z) => {
  const tx = Math.min(tilesX - 1, Math.max(0, Math.floor(x / TILE_M)));
  const tz = Math.min(tilesZ - 1, Math.max(0, Math.floor(z / TILE_M)));
  return tiles[tz * tilesX + tx];
};
const farTileAt = (x, z) => farTiles[tileAt(x, z).tz * tilesX + tileAt(x, z).tx];
// Vertex in tile-local units; shared when identical.
let vertScope = '';   // set while making a building: its vertices aren't shared
const vert = (t, x, y, z) => {
  if (![x, y, z].every(Number.isFinite)) throw new Error(`bad vertex ${x}, ${y}, ${z}`);
  const lx = Math.round((x - t.tx * TILE_M) * UNIT), ly = Math.round(-y * UNIT), lz = Math.round((z - t.tz * TILE_M) * UNIT);
  if (Math.abs(lx) > 32000 || Math.abs(lz) > 32000 || Math.abs(ly) > 32000) throw new Error('vertex out of 16-bit range');
  const key = `${vertScope}${lx},${ly},${lz}`;
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



// Floors for a building OSM gives no height: a guess from what Vlora looks
// like. Houses and sheds by type; otherwise by footprint and place: big
// footprints on the seafront are the Lungomare's hotel and apartment towers,
// big ones on main roads the boulevards' 7-9 storey blocks, small ones houses.
// A hash of the id varies it by a floor or so, the same on every build.
const SMALL = new Set(['garage', 'garages', 'shed', 'roof', 'kiosk', 'hut', 'service', 'carport', 'toilets']);
const HOUSE = new Set(['house', 'detached', 'semidetached_house', 'terrace', 'bungalow', 'farm']);
const levelStats = {};
function guessLevels(tags, poly, id) {
  const type = tags.building || tags['building:part'] || 'yes';
  const area = Math.abs(area2(poly));
  const cx = poly.reduce((s, p) => s + p[0], 0) / poly.length, cz = poly.reduce((s, p) => s + p[1], 0) / poly.length;
  const r = hash(id + 'h') % 3;   // 0, 1, 2
  let kind, levels;
  if (SMALL.has(type) || area < 35) { kind = 'small'; levels = 1; }
  else if (HOUSE.has(type) || area < 90) { kind = 'house'; levels = 1 + (r > 0); }
  else if (['industrial', 'warehouse', 'hangar'].includes(type)) { kind = 'industrial'; levels = 2; }
  else if (['mosque', 'church', 'chapel'].includes(type)) { kind = 'religious'; levels = 3; }
  else if (type === 'kindergarten') { kind = 'school'; levels = 1 + (r > 1); }
  else if (['school', 'college', 'university'].includes(type)) { kind = 'school'; levels = 2 + (r > 0) + (type === 'university'); }
  else if (type === 'hospital') { kind = 'hospital'; levels = 3 + r; }
  else if (area < 180) { kind = 'small block'; levels = 2 + r; }
  else if (area > 250 && coastAt(cx, cz).d < 150) { kind = 'seafront tower'; levels = 9 + r + (area > 700) * 2; }
  else if (area > 250 && mainRoadDist(cx, cz) < 35) { kind = 'boulevard block'; levels = 7 + r; }
  else if (area > 700) { kind = 'large block'; levels = 6 + r; }
  else { kind = 'block'; levels = 4 + r; }
  levelStats[kind] = (levelStats[kind] || 0) + 1;
  return levels;
}

function building(poly, tags, id, isPart) {
  if (poly.length < 3) return;
  if (area2(poly) < 0) poly = poly.slice().reverse();
  let h = metres(tags.height);
  const levels = parseFloat(tags['building:levels']);
  if (!(h > 0)) {
    if (levels > 0) h = levels * FLOOR_H + 1;
    else { h = guessLevels(tags, poly, id) * FLOOR_H + 1; stats.defaultHeights++; }
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
  const btype = tags.building || '';
  const wallTex = ['school', 'kindergarten', 'college', 'university'].includes(btype) ? TEX.school : btype === 'hospital' ? TEX.hospital
    : (tags['building:colour'] || tags.colour) ? TEX.wallA : [TEX.wallA, TEX.wallB, TEX.wallC][hash(id) % 3];
  // Schools come in a few pastel colours.
  const schoolTint = wallTex === TEX.school && !wallCol ? [[128, 128, 128], [136, 118, 112], [120, 128, 104], [138, 124, 96]][hash(id + 's') % 4] : null;
  const roofCol = parseColour(tags['roof:colour']) || [[150, 80, 60], [130, 130, 128], [170, 160, 150]][hash(id + 'r') % 3];
  // Each building is a group the game culls in one test (bounding sphere),
  // with fine faces (8 m x 2-floor textured panels) for up close and coarse
  // ones (one quad per wall) for far away.
  vertScope = `b${id}:`;
  const group = { fine: [], coarse: [], cx, cz, r: Math.max(...poly.map(([x, z]) => Math.hypot(x - cx, z - cz))), top: ground + h, bottom: ground + minH };
  t.groups.push(group);
  // Far: the same boxes in the far tile (tiny sheds left out).
  const ft = farTileAt(cx, cz);
  const far = Math.abs(area2(poly)) >= 35 ? { ...group, fine: [], coarse: null } : null;
  if (far) { far.coarse = far.fine; ft.groups.push(far); }

  // Walls: outward faces of a counter-clockwise outline, vertex order TL, TR, BL, BR
  // seen from outside.
  // Wall pieces: as few as the texture window allows (see MAX_REPEAT).
  const repeatH = FLOOR_H * PANEL_FLOORS;
  const rows = Math.max(1, Math.ceil((h - minH) / (repeatH * MAX_REPEAT) - 0.05));
  for (let i = 0; i < poly.length; i++) {
    const [x0, z0] = poly[i], [x1, z1] = poly[(i + 1) % poly.length];
    const len = Math.hypot(x1 - x0, z1 - z0);
    if (len < 0.2) continue;
    // Sun from the south-west, a bit above: walls facing it are brighter.
    const nx = (z1 - z0) / len, nz = -(x1 - x0) / len;
    const light = 0.72 + 0.28 * Math.max(-1, Math.min(1, -nx * 0.6 - nz * 0.8));
    const base = wallCol ? wallCol.map((c) => c / 255 * 160) : schoolTint || [128, 128, 128];
    const rgb = base.map((c) => clampByte(c * light));
    const cols = Math.max(1, Math.ceil(len / (PANEL_W * MAX_REPEAT) - 0.05));
    group.coarse.push({ v: [vert(t, x0, ground + h, z0), vert(t, x1, ground + h, z1), vert(t, x0, ground + minH, z0), vert(t, x1, ground + minH, z1)], uv: [0, 0, 63, 63], tex: wallTex, layer: 0, rgb });
    if (far) far.fine.push({ v: [vert(ft, x0, ground + h, z0), vert(ft, x1, ground + h, z1), vert(ft, x0, ground + minH, z0), vert(ft, x1, ground + minH, z1)], uv: [0, 0, 63, 63], tex: wallTex, layer: 0, rgb });
    for (let c = 0; c < cols; c++) {
      const a = c / cols, bb = (c + 1) / cols;
      const ax = x0 + (x1 - x0) * a, az = z0 + (z1 - z0) * a, bx = x0 + (x1 - x0) * bb, bz = z0 + (z1 - z0) * bb;
      const uSpan = Math.max(1, Math.min(255, Math.round(len / cols / PANEL_W * 64) - 1));
      for (let r = 0; r < rows; r++) {
        const y0 = minH + (h - minH) * r / rows, y1 = minH + (h - minH) * (r + 1) / rows;
        const vSpan = Math.max(1, Math.min(255, Math.round((y1 - y0) / repeatH * 64) - 1));
        // Seen from outside (to the right of a counter-clockwise outline), the
        // a end is on the left: TL, TR, BL, BR = a top, b top, a bottom, b bottom.
        group.fine.push({ v: [vert(t, ax, ground + y1, az), vert(t, bx, ground + y1, bz), vert(t, ax, ground + y0, az), vert(t, bx, ground + y0, bz)], uv: [0, 255 - vSpan, uSpan, 255], tex: wallTex, layer: 0, rgb });   // bottom of the texture at street level
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
  const k = !isPart && known.get(id);
  if (k && k.name) signQueue.push({ poly, group, t, ground, h, minH, id, kind: k.kind, name: k.name, roofAt: group.fine.length });
  // Roof (seen from above: clockwise on screen = counter-clockwise in X/Z).
  for (const [a, bb, c] of triangulate(poly)) {
    group.nroof = (group.nroof || 0) + 1;
    if (far) far.nroof = (far.nroof || 0) + 1;
    const roof = { v: [vert(t, poly[a][0], ground + h, poly[a][1]), vert(t, poly[c][0], ground + h, poly[c][1]), vert(t, poly[bb][0], ground + h, poly[bb][1]), 0xffff], uv: [0, 0, 0, 0], tex: TEX.none, layer: 0, rgb: roofCol };
    group.fine.push(roof);
    group.coarse.push(roof);
    if (far) far.fine.push({ ...roof, v: [vert(ft, poly[a][0], ground + h, poly[a][1]), vert(ft, poly[c][0], ground + h, poly[c][1]), vert(ft, poly[bb][0], ground + h, poly[bb][1]), 0xffff] });
  }
}

// Flat polygon on the ground (park, water…), untextured.
function flatArea(poly, layer, rgb) {
  vertScope = '';
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
  vertScope = '';
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
      const vSpan = Math.max(1, Math.min(255, Math.round(len / n / ROAD_REPEAT * 64) - 1));
      // Seen from above, travelling along the road: far-left, far-right, near-left, near-right.
      addFace(t, true, { v: [groundVert(t, bx + px, bz + pz), groundVert(t, bx - px, bz - pz), groundVert(t, ax + px, az + pz), groundVert(t, ax - px, az - pz)], uv: [0, 0, 63, vSpan], tex, layer, rgb: [128, 128, 128] });
    }
  }
  // Joints: an octagon fills the gap where a road turns (not on straight runs,
  // nor on narrow footways where the gap doesn't show).
  if (!foot || half >= 2)
    for (let i = 1; i + 1 < pts.length; i++) {
      const [ax, az] = pts[i - 1], [bx, bz] = pts[i], [cx, cz] = pts[i + 1];
      const turn = Math.abs(Math.atan2(cx - bx, cz - bz) - Math.atan2(bx - ax, bz - az));
      if (Math.min(turn, 2 * Math.PI - turn) > 0.2) joint(pts[i], half, layer, tex);
    }
}

function joint([x, z], r, layer, tex) {
  vertScope = '';
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

// Main roads (trunk to tertiary), bucketed in 64 m cells: buildings fronting
// them are taller (see guessLevels).
const mainGrid = new Map();
for (const w of ways.values()) {
  const k = (w.tags.highway || '').replace(/_link$/, '');
  if (!['trunk', 'primary', 'secondary', 'tertiary'].includes(k)) continue;
  const pts = wayPts(w);
  for (let i = 0; i + 1 < pts.length; i++) {
    const [x0, z0] = pts[i], [x1, z1] = pts[i + 1];
    for (let cz = Math.floor(Math.min(z0, z1) / 64); cz <= Math.floor(Math.max(z0, z1) / 64); cz++)
      for (let cx = Math.floor(Math.min(x0, x1) / 64); cx <= Math.floor(Math.max(x0, x1) / 64); cx++) {
        const key = cx + ',' + cz;
        if (!mainGrid.has(key)) mainGrid.set(key, []);
        mainGrid.get(key).push([x0, z0, x1, z1]);
      }
  }
}
function mainRoadDist(x, z) {
  let best = Infinity;
  for (let cz = Math.floor(z / 64) - 1; cz <= Math.floor(z / 64) + 1; cz++)
    for (let cx = Math.floor(x / 64) - 1; cx <= Math.floor(x / 64) + 1; cx++)
      for (const [x0, z0, x1, z1] of mainGrid.get(cx + ',' + cz) || []) {
        const dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz || 1e-9;
        const u = Math.max(0, Math.min(1, ((x - x0) * dx + (z - z0) * dz) / l2));
        best = Math.min(best, Math.hypot(x0 + dx * u - x, z0 + dz * u - z));
      }
  return best;
}

// Landmarks (tools/landmarks.mjs): found by name; their own buildings are
// replaced by the models (placed further down, once the streets are known).
const landmarkHits = (noLandmarks ? [] : LANDMARKS).map((l) => {
  const w = [...ways.values()].find((w) => w.tags.name === l.name && closed(w));
  if (w) return { l, way: w, pts: ring(w) };
  const n = [...nodeTags].find(([, t]) => t.name === l.name);
  if (n) return { l, pts: [toXZ(n[0])] };
  console.warn(`landmark "${l.name}" not found`);
  return null;
}).filter(Boolean);
const landmarkWays = new Set(landmarkHits.filter((h) => h.way).map((h) => h.way.id));
// Other buildings in the same place (e.g. Microsoft's footprint of the same
// mosque) go too: anything whose centre is within 2 m of a landmark's
// outline (its OSM outline, or its model's for one on a point).
const landmarkAreas = landmarkHits.map(({ l, pts }) => {
  if (pts.length > 2) return pts;
  const { collide } = l.build({ w: 0, d: 0 });
  const b = (l.facing || 0) * Math.PI / 180, s = -Math.sin(b), c = -Math.cos(b), [cx, cz] = pts[0];
  return (collide || []).map(([x, z]) => [cx + x * c + z * s, cz - x * s + z * c]);
}).filter((p) => p.length > 2);
function nearLandmark(poly) {
  const x = poly.reduce((s, p) => s + p[0], 0) / poly.length, z = poly.reduce((s, p) => s + p[1], 0) / poly.length;
  return landmarkAreas.some((area) => {
    let inside = false;
    for (let i = 0, j = area.length - 1; i < area.length; j = i++) {
      const [xi, zi] = area[i], [xj, zj] = area[j];
      if ((zi > z) !== (zj > z) && x < ((xj - xi) * (z - zi)) / (zj - zi) + xi) inside = !inside;
      const dx = xj - xi, dz = zj - zi, l2 = dx * dx + dz * dz || 1, u = Math.max(0, Math.min(1, ((x - xi) * dx + (z - zi) * dz) / l2));
      if (Math.hypot(xi + dx * u - x, zi + dz * u - z) < 2) return true;
    }
    return inside;
  });
}
stats.replacedByLandmarks = 0;

// Known buildings (schools, hospitals, hotels...): from the building's own
// tags or a named point inside it. They get the look of their kind and,
// with a name, a sign over the door on the street side.
const knownKind = (t) => {
  const a = t.amenity, b = t.building, tr = t.tourism;
  if (['school', 'college', 'kindergarten'].includes(a) || ['school', 'kindergarten', 'college'].includes(b)) return 'school';
  if (a === 'university' || b === 'university') return 'university';
  if (['hospital', 'clinic', 'doctors'].includes(a) || b === 'hospital') return 'hospital';
  if (a === 'place_of_worship' || ['church', 'mosque', 'chapel', 'cathedral'].includes(b)) return 'worship';
  if (['museum'].includes(tr) || ['theatre', 'library', 'arts_centre', 'cinema'].includes(a)) return 'culture';
  if (['townhall', 'courthouse'].includes(a) || t.office === 'government' || ['government', 'civic'].includes(b)) return 'government';
  if (a === 'police') return 'police';
  if (a === 'post_office') return 'post';
  if (['hotel', 'hostel', 'guest_house'].includes(tr) || b === 'hotel') return 'hotel';
  if (a === 'bank') return 'bank';
  return null;
};
const known = new Map();   // building way id -> { kind, name }
const insidePoly = (x, z, p) => { let c = false; for (let i = 0, j = p.length - 1; i < p.length; j = i++) if ((p[i][1] > z) !== (p[j][1] > z) && x < ((p[j][0] - p[i][0]) * (z - p[i][1])) / (p[j][1] - p[i][1]) + p[i][0]) c = !c; return c; };
{
  const grid = new Map(), cell = 64;
  for (const w of ways.values()) {
    if (!w.tags.building || !closed(w)) continue;
    const kind = knownKind(w.tags);
    if (kind) known.set(w.id, { kind, name: w.tags.name || '' });
    const p = ring(w);
    if (p.length < 3) continue;
    const xs = p.map((q) => q[0]), zs = p.map((q) => q[1]);
    for (let cz = Math.floor(Math.min(...zs) / cell); cz <= Math.floor(Math.max(...zs) / cell); cz++)
      for (let cx = Math.floor(Math.min(...xs) / cell); cx <= Math.floor(Math.max(...xs) / cell); cx++) {
        const k = cx + ',' + cz;
        if (!grid.has(k)) grid.set(k, []);
        grid.get(k).push([w.id, p]);
      }
  }
  // Sites (school grounds, hospital areas): every building in them takes the
  // look, the biggest the name.
  for (const w of ways.values()) {
    const kind = !w.tags.building && closed(w) && knownKind(w.tags);
    if (!kind) continue;
    const site = ring(w);
    if (site.length < 3) continue;
    const xs = site.map((q) => q[0]), zs = site.map((q) => q[1]);
    const inSite = new Map();
    for (let cz = Math.floor(Math.min(...zs) / cell); cz <= Math.floor(Math.max(...zs) / cell); cz++)
      for (let cx = Math.floor(Math.min(...xs) / cell); cx <= Math.floor(Math.max(...xs) / cell); cx++)
        for (const [wid, p] of grid.get(cx + ',' + cz) || []) {
          const mx = p.reduce((s, q) => s + q[0], 0) / p.length, mz = p.reduce((s, q) => s + q[1], 0) / p.length;
          if (!inSite.has(wid) && insidePoly(mx, mz, site)) inSite.set(wid, Math.abs(area2(p)));
        }
    if (!inSite.size) continue;
    const biggest = [...inSite].sort((a, b) => b[1] - a[1])[0][0];
    for (const wid of inSite.keys()) {
      const k = known.get(wid);
      if (k && k.name) continue;
      known.set(wid, { kind, name: wid === biggest ? w.tags.name || '' : '' });
    }
  }
  for (const [id, t] of nodeTags) {
    const kind = knownKind(t);
    if (!kind || !t.name) continue;
    const pt = toXZ(id);
    if (!pt) continue;
    for (const [wid, p] of grid.get(Math.floor(pt[0] / cell) + ',' + Math.floor(pt[1] / cell)) || [])
      if (insidePoly(pt[0], pt[1], p)) { const k = known.get(wid); if (!k || !k.name) known.set(wid, { kind, name: t.name }); break; }
  }
}
const KIND_BUILDING = { school: 'school', university: 'university', hospital: 'hospital' };
const signQueue = [];

for (const w of ways.values()) {
  const t = w.tags;
  if (landmarkWays.has(w.id)) continue;
  if ((t.building || t['building:part']) && closed(w) && landmarkAreas.length && nearLandmark(ring(w))) { stats.replacedByLandmarks++; continue; }
  if (t.building && t.building !== 'no' && closed(w)) {
    const k = known.get(w.id);
    building(ring(w), k && t.building === 'yes' && KIND_BUILDING[k.kind] ? { ...t, building: KIND_BUILDING[k.kind] } : t, w.id, false);
  }
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

// Ground: a grid of quads under everything: textured land, flat-coloured sea.
// Cells near the coast are classified in 4 m strips so the shore follows it.
const SEA = [46, 88, 128];
function groundQuad(t, x0, z0, w, h, sea) {
  vertScope = '';
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
vertScope = '';
for (const ft of farTiles) {
  for (let gz = 0; gz < TILE_M; gz += 32) for (let gx = 0; gx < TILE_M; gx += 32) {
    const x0 = ft.tx * TILE_M + gx, z0 = ft.tz * TILE_M + gz;
    const sea = coastAt(x0 + 16, z0 + 16).sea;
    const v = sea ? [vert(ft, x0, 0, z0 + 32), vert(ft, x0 + 32, 0, z0 + 32), vert(ft, x0, 0, z0), vert(ft, x0 + 32, 0, z0)]
                  : [groundVert(ft, x0, z0 + 32), groundVert(ft, x0 + 32, z0 + 32), groundVert(ft, x0, z0), groundVert(ft, x0 + 32, z0)];
    ft.flat.push({ v, uv: [0, 0, 0, 0], tex: sea ? TEX.none : TEX.ground, layer: LAYER.ground, rgb: sea ? SEA : [128, 128, 128] });
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
// Drivable road segments (metres), for snapping, the minimap and street names.
const drivable = [];
for (const w of ways.values()) {
  const k = w.tags.highway && w.tags.highway.replace(/_link$/, '');
  if (!k || FOOT.has(k) || !ROAD_WIDTH[k] || w.tags.area === 'yes') continue;
  const pts = wayPts(w);
  for (let i = 0; i + 1 < pts.length; i++)
    if (inside((pts[i][0] + pts[i + 1][0]) / 2, (pts[i][1] + pts[i + 1][1]) / 2)) drivable.push({ a: pts[i], b: pts[i + 1], name: w.tags.name || '', kind: k });
}
function nearestRoad([x, z], among = drivable) {
  let best = null;
  for (const { a: [x0, z0], b: [x1, z1] } of among) {
    const dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz || 1;
    const u = Math.max(0, Math.min(1, ((x - x0) * dx + (z - z0) * dz) / l2));
    const px = x0 + dx * u, pz = z0 + dz * u, d = Math.hypot(px - x, pz - z);
    if (!best || d < best.d) best = { d, p: [px, pz], heading: Math.atan2(dx, dz) };
  }
  return best;
}
// Move the spawn to the nearest road point so the taxi starts on a street
// (on --spawn-road if given and found).
{
  const want = spawnRoad && spawnRoad.toLowerCase();
  const named = want ? drivable.filter((d) => d.name.toLowerCase() === want) : [];
  if (want && !named.length) console.warn(`spawn road "${spawnRoad}" not found; using the nearest road`);
  const best = nearestRoad(spawn, named.length ? named : drivable);
  if (best) { spawn = best.p; spawn.heading = best.heading; }
}

// ---- Landmarks ------------------------------------------------------------------------

// Smallest rectangle around an outline: { cx, cz, angle (of the long side), w (long), d }.
function orientedBox(pts) {
  let best = null;
  for (let i = 0; i < pts.length; i++) {
    const [x0, z0] = pts[i], [x1, z1] = pts[(i + 1) % pts.length];
    const a = Math.atan2(z1 - z0, x1 - x0), c = Math.cos(a), s = Math.sin(a);
    let u0 = Infinity, u1 = -Infinity, v0 = Infinity, v1 = -Infinity;
    for (const [x, z] of pts) { const u = x * c + z * s, v = -x * s + z * c; u0 = Math.min(u0, u); u1 = Math.max(u1, u); v0 = Math.min(v0, v); v1 = Math.max(v1, v); }
    const area = (u1 - u0) * (v1 - v0);
    if (!best || area < best.area) {
      const uc = (u0 + u1) / 2, vc = (v0 + v1) / 2;
      best = { area, cx: uc * c - vc * s, cz: uc * s + vc * c, angle: a, w: u1 - u0, d: v1 - v0 };
    }
  }
  if (best.d > best.w) { best.angle += Math.PI / 2; [best.w, best.d] = [best.d, best.w]; }
  return best;
}

// Nearest point on any highway with this name (footways included).
function nearestNamedWay([x, z], name) {
  let best = null;
  for (const w of ways.values()) {
    if (!w.tags.highway || w.tags.name !== name) continue;
    const p = wayPts(w);
    for (let i = 0; i + 1 < p.length; i++) {
      const [x0, z0] = p[i], [x1, z1] = p[i + 1], dx = x1 - x0, dz = z1 - z0, l2 = dx * dx + dz * dz || 1;
      const u = Math.max(0, Math.min(1, ((x - x0) * dx + (z - z0) * dz) / l2)), px = x0 + dx * u, pz = z0 + dz * u, d = Math.hypot(px - x, pz - z);
      if (!best || d < best.d) best = { d, p: [px, pz] };
    }
  }
  return best;
}

const signs = [];   // name signs (landmarks' first, then known buildings')
const landmarkStats = [];
function placeLandmark({ l, pts }) {
  let cx, cz, size = { w: 0, d: 0 }, front;   // front: unit vector the front looks along
  if (l.place === 'footprint' && pts.length > 2) {
    const box = orientedBox(pts);
    ({ cx, cz } = box);
    const short = l.frontSide === 'short';
    size = short ? { w: box.d, d: box.w } : { w: box.w, d: box.d };
    // The front faces the nearest street (or the one named): one of the two
    // normals of the long (or short) sides.
    const nx = short ? Math.cos(box.angle) : -Math.sin(box.angle), nz = short ? Math.sin(box.angle) : Math.cos(box.angle);
    const road = l.frontRoad ? nearestNamedWay([cx, cz], l.frontRoad) : nearestRoad([cx, cz]);
    const toRoad = road ? (road.p[0] - cx) * nx + (road.p[1] - cz) * nz : 1;
    front = toRoad >= 0 ? [nx, nz] : [-nx, -nz];
  } else {
    cx = pts.reduce((s, p) => s + p[0], 0) / pts.length;
    cz = pts.reduce((s, p) => s + p[1], 0) / pts.length;
    const b = (l.facing || 0) * Math.PI / 180;
    front = [Math.sin(b), Math.cos(b)];
  }
  // Model -> world: local -z is the front, local x its right (Model.add's turn).
  const s = -front[0], c = -front[1];
  const toWorld = ([x, y, z]) => [cx + x * c + z * s, y, cz - x * s + z * c];
  const { model, far, collide } = l.build(size);
  const outline = (collide || []).map(([x, z]) => toWorld([x, 0, z]));
  const ground = Math.min(elev(cx, cz), ...outline.map(([x, , z]) => elev(x, z)));
  const t = tileAt(cx, cz), ft = farTileAt(cx, cz);
  vertScope = `lm:${l.key}:`;
  const all = model.faces.flatMap((f) => f.p).map(toWorld);
  const r = Math.max(...all.map(([x, , z]) => Math.hypot(x - cx, z - cz))) + 0.5;
  const top = ground + Math.max(...all.map((p) => p[1]));
  const faces = (m, tile) => m.faces.map((f) => {
    const p = f.p.map(toWorld);
    // Outward normal (u x v of the first corner's edges) for the sun's shading.
    const u = p[1].map((v, i) => v - p[0][i]), v = p[2].map((v, i) => v - p[0][i]);
    let n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
    const len = Math.hypot(...n) || 1;
    n = n.map((x) => x / len);
    const light = 0.72 + 0.28 * Math.max(-1, Math.min(1, -n[0] * 0.6 - n[2] * 0.8 + n[1] * 0.9));
    const vs = p.map(([x, y, z]) => vert(tile, x, ground + y, z));
    if (f.sign) {
      // Its own sign in the atlas (once, for the near model).
      if (f.signIndex === undefined) { f.signIndex = signs.length; signs.push({ lines: signLines(f.sign.text), kind: f.sign.kind, at: [Math.round(cx), Math.round(cz)] }); }
      const i = f.signIndex;
      return { v: vs, uv: [0, (i & 3) * 16, 63, (i & 3) * 16 + 15], tex: 32 + (i >> 2), layer: 0, rgb: [128, 128, 128] };
    }
    return { v: vs.length === 3 ? [...vs, 0xffff] : vs, uv: f.uv, tex: f.tex, layer: 0, rgb: f.rgb.map((x) => clampByte(x * light)) };
  });
  const fine = faces(model, t), coarse = far ? faces(far, t) : fine;
  t.groups.push({ fine, coarse, cx, cz, r, top, bottom: ground, nroof: 0 });
  ft.groups.push({ fine: far ? faces(far, ft) : faces(model, ft), coarse: null, cx, cz, r, top, bottom: ground, nroof: 0 });
  ft.groups[ft.groups.length - 1].coarse = ft.groups[ft.groups.length - 1].fine;
  vertScope = '';
  // Collision: the outline, in <= 32 m pieces, counter-clockwise.
  if (outline.length > 2 && area2(outline.map(([x, , z]) => [x, z])) < 0) outline.reverse();
  for (let i = 0; i < outline.length; i++) {
    const [ax, , az] = outline[i], [bx, , bz] = outline[(i + 1) % outline.length];
    const et = tileAt((ax + bx) / 2, (az + bz) / 2);
    et.edges.push([ax, az, bx, bz].map((v, j) => Math.round((v - (j % 2 ? et.tz : et.tx) * TILE_M) * UNIT)));
  }
  landmarkStats.push({ key: l.key, at: [Math.round(cx), Math.round(cz)], faces: model.faces.length, farFaces: far ? far.faces.length : 0, size: [+size.w.toFixed(1), +size.d.toFixed(1)] });
}
for (const h of landmarkHits) if (inside(h.pts[0][0], h.pts[0][1])) placeLandmark(h);

// ---- Name signs ------------------------------------------------------------------------
// The most useful first (schools...), at most 64 (16 texture slots).
const SIGN_ORDER = ['school', 'university', 'hospital', 'culture', 'government', 'police', 'worship', 'post', 'bank', 'hotel'];
const MAX_SIGNS = 64;
for (const q of signQueue.sort((a, b) => SIGN_ORDER.indexOf(a.kind) - SIGN_ORDER.indexOf(b.kind))) {
  if (signs.length >= MAX_SIGNS) break;
  const plain = q.name.replace(/&quot;/g, '"').normalize('NFD').replace(/[\u0300-\u036f]/g, '').replace(/[“”„]/g, '"').replace(/[^\x20-\x7e]/g, '').toUpperCase().trim();
  const lines = signLines(plain);
  if (!lines.length || !lines[0]) continue;
  // The wall facing the nearest street, at least 4.6 m long.
  const cx = q.poly.reduce((s, p) => s + p[0], 0) / q.poly.length, cz = q.poly.reduce((s, p) => s + p[1], 0) / q.poly.length;
  const road = nearestRoad([cx, cz]);
  if (!road) continue;
  const toRoad = [road.p[0] - cx, road.p[1] - cz], tl = Math.hypot(...toRoad) || 1;
  let best = null;
  for (let i = 0; i < q.poly.length; i++) {
    const [x0, z0] = q.poly[i], [x1, z1] = q.poly[(i + 1) % q.poly.length];
    const len = Math.hypot(x1 - x0, z1 - z0);
    if (len < 4.6) continue;
    const nx = (z1 - z0) / len, nz = -(x1 - x0) / len;   // outward (outline counter-clockwise)
    const score = (nx * toRoad[0] + nz * toRoad[1]) / tl;
    if (!best || score > best.score) best = { score, x0, z0, x1, z1, len, nx, nz };
  }
  if (!best) continue;
  const i = signs.length, slot = i >> 2, band = i & 3;
  signs.push({ lines, kind: q.kind, at: [Math.round(cx), Math.round(cz)] });
  const ex = (best.x1 - best.x0) / best.len, ez = (best.z1 - best.z0) / best.len;
  const mx = (best.x0 + best.x1) / 2 + best.nx * 0.12, mz = (best.z0 + best.z1) / 2 + best.nz * 0.12;
  const y0 = q.ground + Math.max(q.minH + 0.3, Math.min(3.2, q.h - 1.4)), y1 = y0 + 1;
  vertScope = `b${q.id}:`;
  const t = q.t;
  const face = { v: [vert(t, mx - ex * 2, y1, mz - ez * 2), vert(t, mx + ex * 2, y1, mz + ez * 2), vert(t, mx - ex * 2, y0, mz - ez * 2), vert(t, mx + ex * 2, y0, mz + ez * 2)],
    uv: [0, band * 16, 63, band * 16 + 15], tex: 32 + slot, layer: 0, rgb: [128, 128, 128] };
  q.group.fine.splice(q.roofAt, 0, face);   // before the roof faces (they must stay last)
  vertScope = '';
}
stats.signs = signs.length;

// ---- Places, minimap roads, names ------------------------------------------------
//
// Global section (read into RAM once): u16 nplaces, nnames, nsigns, signs offset / 4,
//   places  nplaces x { i32 x, z (world units, on the nearest road), u16 name, u8 kind, u8 pad }
//   names   nnames x u16 offset (from the start of the name text), then the text (NUL-ended)
//   signs   (at the offset) 16 x u16 CLUT, then the 4-bit sign atlas (tools/signs.mjs),
//           128 bytes a row, 64 rows per 4 slots
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
    const sg = { c: [ax, az, bx, bz].map(Math.round), name: name ? nameId(name) : 0xffff, cls };
    segs.push(sg);
    tileAt((ax + bx) / 2, (az + bz) / 2).segs.push(sg);
  }
}
if (places.length > 4000 || names.length > 8000) throw new Error('too many places or names');

// ---- Road graph and the big map ----------------------------------------------------
//
// For the route to the passenger / destination (shortest path on the PS1,
// respecting one-way streets) and the full-screen map (Triangle). A separate
// section right after the global one, read into RAM once:
//   u16 nnodes, nedges, gridW, gridH, cellM, seaW, seaH, seaCell,
//   u32 npoints, nadj, ncellEntries, 0
//   nodes   nnodes x { i16 x, z (metres) }          junctions and dead ends
//   adj     nnodes + 1 x u32 first (into the list), then nadj x u16 edge
//   edges   nedges x { u16 a, b, length (m), u8 npoints, u8 flags, u32 firstPoint }
//           flags: bits 0-1 class (0 minor, 1 secondary/tertiary, 2 primary+),
//           4 = one-way (only a -> b), 8 = service road (routes avoid it)
//   points  npoints x { i16 x, z }: each edge's shape between a and b
//   grid    gridW x gridH cells of cellM metres (row = z): ncells + 1 x u32
//           first, then ncellEntries x u16 edge (the edges crossing each cell)
//   sea     seaH rows of ceil(seaW / 8) bytes, a bit per seaCell-metre cell
//           (1 = sea, bit 0 = west), for the big map
const GRID_M = 128, SEA_CELL = 32;
const gNodes = [], gNodeId = new Map(), gEdges = [], gPoints = [];
{
  const ONEWAY = (t) => {
    if (t.oneway === '-1' || t.oneway === 'reverse') return -1;
    if (['yes', 'true', '1'].includes(t.oneway)) return 1;
    if (t.oneway === 'no') return 0;
    return ['roundabout', 'circular'].includes(t.junction) || t.highway === 'motorway' ? 1 : 0;
  };
  // Runs of each drivable way inside the box, and how often each node is used.
  const runs = [], uses = new Map();
  for (const w of ways.values()) {
    const k = w.tags.highway && w.tags.highway.replace(/_link$/, '');
    if (!k || FOOT.has(k) || !ROAD_WIDTH[k] || w.tags.area === 'yes') continue;
    const refs = w.refs.filter((r) => nodes.has(r));
    let run = [];
    const flush = () => { if (run.length > 1) runs.push({ refs: run, tags: w.tags, kind: k }); run = []; };
    for (let i = 0; i + 1 < refs.length; i++) {
      const a = toXZ(refs[i]), b = toXZ(refs[i + 1]);
      if (inside((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)) { if (!run.length) run.push(refs[i]); run.push(refs[i + 1]); }
      else flush();
    }
    flush();
  }
  for (const r of runs) r.refs.forEach((id, i) => uses.set(id, (uses.get(id) || 0) + (i === 0 || i === r.refs.length - 1 ? 2 : 1)));
  const nodeIndex = (id) => {
    if (!gNodeId.has(id)) { const [x, z] = toXZ(id); gNodeId.set(id, gNodes.length); gNodes.push([Math.round(x), Math.round(z)]); }
    return gNodeId.get(id);
  };
  // Douglas-Peucker: the shape to ~1.5 m.
  const simplify = (pts) => {
    if (pts.length < 3) return pts;
    const [x0, z0] = pts[0], [x1, z1] = pts[pts.length - 1];
    const dx = x1 - x0, dz = z1 - z0, l = Math.hypot(dx, dz) || 1e-9;
    let worst = 0, at = 0;
    for (let i = 1; i < pts.length - 1; i++) {
      const d = Math.abs((pts[i][0] - x0) * dz - (pts[i][1] - z0) * dx) / l;
      if (d > worst) { worst = d; at = i; }
    }
    if (worst < 1.5) return [pts[0], pts[pts.length - 1]];
    return [...simplify(pts.slice(0, at + 1)).slice(0, -1), ...simplify(pts.slice(at))];
  };
  for (const r of runs) {
    const dir = ONEWAY(r.tags);
    const cls = ['motorway', 'trunk', 'primary'].includes(r.kind) ? 2 : ['secondary', 'tertiary'].includes(r.kind) ? 1 : 0;
    const flags = cls | (dir ? 4 : 0) | (['service', 'track'].includes(r.kind) ? 8 : 0);
    let start = 0;
    for (let i = 1; i < r.refs.length; i++) {
      if (i < r.refs.length - 1 && uses.get(r.refs[i]) < 2) continue;
      let ids = r.refs.slice(start, i + 1);
      if (dir < 0) ids = ids.reverse();
      start = i;
      const pts = ids.map(toXZ);
      let len = 0;
      for (let j = 0; j + 1 < pts.length; j++) len += Math.hypot(pts[j + 1][0] - pts[j][0], pts[j + 1][1] - pts[j][1]);
      const a = nodeIndex(ids[0]), b = nodeIndex(ids[ids.length - 1]);
      if (a === b && len < 1) continue;
      let shape = simplify(pts).slice(1, -1).map((p) => p.map(Math.round));
      // Long edges in pieces of <= 255 shape points (never happens in practice).
      if (shape.length > 255) shape = shape.filter((_, j) => j % Math.ceil(shape.length / 255) === 0);
      gEdges.push({ a, b, len: Math.max(1, Math.round(len)), flags, first: gPoints.length, n: shape.length });
      gPoints.push(...shape);
    }
  }
}
function graphBlob() {
  const N = gNodes.length, E = gEdges.length;
  if (N > 65535 || E > 65535) throw new Error(`road graph too big: ${N} nodes, ${E} edges`);
  const adj = Array.from({ length: N }, () => []);
  gEdges.forEach((e, i) => { adj[e.a].push(i); if (e.b !== e.a) adj[e.b].push(i); });
  const gw = Math.ceil(widthM / GRID_M), gh = Math.ceil(heightM / GRID_M);
  const cells = Array.from({ length: gw * gh }, () => new Set());
  gEdges.forEach((e, i) => {
    const pts = [gNodes[e.a], ...gPoints.slice(e.first, e.first + e.n), gNodes[e.b]];
    for (let j = 0; j + 1 < pts.length; j++) {
      const [x0, z0] = pts[j], [x1, z1] = pts[j + 1];
      for (let cz = Math.floor(Math.min(z0, z1) / GRID_M); cz <= Math.floor(Math.max(z0, z1) / GRID_M); cz++)
        for (let cx = Math.floor(Math.min(x0, x1) / GRID_M); cx <= Math.floor(Math.max(x0, x1) / GRID_M); cx++)
          if (cx >= 0 && cz >= 0 && cx < gw && cz < gh) cells[cz * gw + cx].add(i);
    }
  });
  const nEntries = cells.reduce((s, c) => s + c.size, 0), nAdj = adj.reduce((s, a) => s + a.length, 0);
  const sw = Math.ceil(widthM / SEA_CELL), sh = Math.ceil(heightM / SEA_CELL), rowBytes = Math.ceil(sw / 8);
  const size = 32 + N * 4 + (N + 1) * 4 + nAdj * 2 + 2 + E * 12 + gPoints.length * 4 + (gw * gh + 1) * 4 + nEntries * 2 + 2 + sh * rowBytes;
  const buf = Buffer.alloc((size + 3) & ~3);
  let o = 0;
  for (const v of [N, E, gw, gh, GRID_M, sw, sh, SEA_CELL]) { buf.writeUInt16LE(v, o); o += 2; }
  for (const v of [gPoints.length, nAdj, nEntries, 0]) { buf.writeUInt32LE(v, o); o += 4; }
  for (const [x, z] of gNodes) { buf.writeInt16LE(x, o); buf.writeInt16LE(z, o + 2); o += 4; }
  let f = 0;
  for (let i = 0; i <= N; i++) { buf.writeUInt32LE(f, o); o += 4; if (i < N) f += adj[i].length; }
  for (const a of adj) for (const e of a) { buf.writeUInt16LE(e, o); o += 2; }
  o = (o + 3) & ~3;
  for (const e of gEdges) {
    buf.writeUInt16LE(e.a, o); buf.writeUInt16LE(e.b, o + 2); buf.writeUInt16LE(Math.min(65535, e.len), o + 4);
    buf.writeUInt8(e.n, o + 6); buf.writeUInt8(e.flags, o + 7); buf.writeUInt32LE(e.first, o + 8); o += 12;
  }
  for (const [x, z] of gPoints) { buf.writeInt16LE(x, o); buf.writeInt16LE(z, o + 2); o += 4; }
  f = 0;
  for (let i = 0; i <= cells.length; i++) { buf.writeUInt32LE(f, o); o += 4; if (i < cells.length) f += cells[i].size; }
  for (const c of cells) for (const e of c) { buf.writeUInt16LE(e, o); o += 2; }
  o = (o + 3) & ~3;
  let seaBits = 0;
  for (let j = 0; j < sh; j++) for (let i = 0; i < sw; i++)
    if (coastAt((i + 0.5) * SEA_CELL, (j + 0.5) * SEA_CELL).sea) { buf[o + j * rowBytes + (i >> 3)] |= 1 << (i & 7); seaBits++; }
  o += sh * rowBytes;
  if (o > buf.length) throw new Error('graph blob size mismatch');
  stats.graph = { nodes: N, edges: E, points: gPoints.length, oneway: gEdges.filter((e) => e.flags & 4).length, cellEntries: nEntries, seaCells: seaBits, bytes: buf.length };
  return buf;
}

function globalBlob() {
  const text = names.map((n) => Buffer.from(n + '\0', 'latin1'));
  const textBytes = text.reduce((sum, b) => sum + b.length, 0);
  const atlas = signs.length ? signAtlas(signs) : null;
  const signAt = (8 + places.length * 12 + names.length * 2 + textBytes + 3) & ~3;
  const size = signAt + (atlas ? 32 + atlas.bytes.length : 0);
  const buf = Buffer.alloc((size + 3) & ~3);
  let o = 0;
  for (const v of [places.length, names.length, signs.length, atlas ? signAt / 4 : 0]) { buf.writeUInt16LE(v, o); o += 2; }
  for (const p of places) { buf.writeInt32LE(p.x, o); buf.writeInt32LE(p.z, o + 4); buf.writeUInt16LE(p.name, o + 8); buf.writeUInt8(p.kind, o + 10); o += 12; }
  let off = 0;
  for (const b of text) { buf.writeUInt16LE(off, o); o += 2; off += b.length; }
  for (const b of text) { b.copy(buf, o); o += b.length; }
  if (atlas) {
    o = signAt;
    SIGN_PALETTE.forEach((c, i) => buf.writeUInt16LE(sign15(c), o + i * 2));
    atlas.bytes.copy(buf, o + 32);
  }
  return buf;
}
const global = globalBlob();
const graph = graphBlob();
const MAX_GRAPH_BYTES = 192 * 1024;  // must match GRAPH_BYTES in src/route.h
if (graph.length > MAX_GRAPH_BYTES) throw new Error(`the road graph takes ${graph.length} bytes (max ${MAX_GRAPH_BYTES})`);
const MAX_GLOBAL_BYTES = 64 * 1024;  // must match GLOBAL_BYTES in src/world.c
if (global.length > MAX_GLOBAL_BYTES) throw new Error(`places/roads/names take ${global.length} bytes (max ${MAX_GLOBAL_BYTES})`);

// ---- Write ----------------------------------------------------------------------

// One tile (near or far) as bytes; see the layout at the top of the file.
let maxVerts = 0, maxBytes = 0, worst = null, maxFar = 0;
function encodeTile(t, near) {
  const flat = t.flat.sort((a, b) => a.layer - b.layer);
  if (t.solid.length) t.groups.push({ fine: t.solid, coarse: t.solid, cx: (t.tx + 0.5) * TILE_M, cz: (t.tz + 0.5) * TILE_M, r: TILE_M, top: 100, bottom: 0 });
  const faces = [...flat];
  // New vertex order: the flat faces' vertices, then each group's in turn.
  const order = [], newIndex = new Map();
  const take = (fs) => { for (const f of fs) for (const v of f.v) if (v !== 0xffff && v !== undefined && !newIndex.has(v)) { newIndex.set(v, order.length); order.push(v); } };
  take(flat);
  const nflatVerts = order.length;
  const groupRecs = t.groups.map((g) => {
    const first = faces.length;
    const shared = g.coarse === g.fine;
    faces.push(...g.fine, ...(shared ? [] : g.coarse));
    const vfirst = order.length;
    take(g.fine);
    if (!shared) take(g.coarse);
    return [Math.round((g.cx - t.tx * TILE_M) * UNIT), Math.round((g.cz - t.tz * TILE_M) * UNIT), Math.ceil(g.r * UNIT) + 1,
      Math.round(-g.top * UNIT), Math.round(-g.bottom * UNIT), first, g.fine.length, shared ? 0 : g.coarse.length, vfirst, order.length - vfirst, g.nroof || 0];
  });
  if (order.length !== t.verts.length) throw new Error(`tile ${t.tx},${t.tz}: ${t.verts.length - order.length} vertices shared between groups`);
  const verts = order.map((i) => t.verts[i]);
  const remap = (v) => (v === 0xffff || v === undefined ? 0xffff : newIndex.get(v));
  if (faces.length > 65535) throw new Error(`tile ${t.tx},${t.tz} has too many faces`);
  if (t.verts.length > MAX_TILE_VERTS) throw new Error(`tile ${t.tx},${t.tz} has ${t.verts.length} vertices (max ${MAX_TILE_VERTS}): lower the detail`);
  // Near tiles: ground height every 8 m (9 x 9, units, Y down) for the car.
  const hm = [];
  if (near) for (let j = 0; j <= 8; j++) for (let i = 0; i <= 8; i++) hm.push(Math.round(-elev(t.tx * TILE_M + i * 8, t.tz * TILE_M + j * 8) * UNIT));
  const segs = near ? t.segs : [];
  const size = 28 + t.verts.length * 8 + faces.length * 16 + t.edges.length * 8 + hm.length * 2 + groupRecs.length * 24 + segs.length * 12;
  const limit = near ? MAX_TILE_BYTES : MAX_FAR_BYTES;
  if (size > limit) throw new Error(`${near ? '' : 'far '}tile ${t.tx},${t.tz} is ${size} bytes (max ${limit}): lower the detail`);
  const buf = Buffer.alloc(size);
  let minX = 32767, minZ = 32767, maxX = -32768, maxZ = -32768, minY = 0;
  for (const [x, y, z] of t.verts) { minX = Math.min(minX, x); maxX = Math.max(maxX, x); minZ = Math.min(minZ, z); maxZ = Math.max(maxZ, z); minY = Math.min(minY, y); }
  if (!t.verts.length) minX = minZ = maxX = maxZ = 0;
  let o = 0;
  for (const v of [t.verts.length, faces.length, t.edges.length, flat.length]) { buf.writeUInt16LE(v, o); o += 2; }
  for (const v of [minX, minZ, maxX, maxZ, minY, near ? 1 : 0]) { buf.writeInt16LE(v, o); o += 2; }   // flags 1: has a height grid
  buf.writeUInt16LE(groupRecs.length, o); buf.writeUInt16LE(segs.length, o + 2); buf.writeUInt16LE(nflatVerts, o + 4); o += 8;
  for (const [x, y, z] of verts) { buf.writeInt16LE(x, o); buf.writeInt16LE(y, o + 2); buf.writeInt16LE(z, o + 4); o += 8; }
  for (const f of faces) {
    for (let k = 0; k < 4; k++) buf.writeUInt16LE(remap(f.v[k]), o + k * 2);
    f.uv.forEach((u, k) => buf.writeUInt8(u, o + 8 + k));
    buf.writeUInt8(f.tex | (f.layer << 6), o + 12);
    f.rgb.forEach((c, k) => buf.writeUInt8(clampByte(c), o + 13 + k));
    o += 16;
  }
  for (const e of t.edges) { e.forEach((v, k) => buf.writeInt16LE(v, o + k * 2)); o += 8; }
  for (const v of hm) { buf.writeInt16LE(v, o); o += 2; }
  for (const [gx, gz, r, top, bottom, first, nfine, ncoarse, vfirst, vcount, nroof] of groupRecs) {
    for (const v of [gx, gz, r, top, bottom]) { buf.writeInt16LE(v, o); o += 2; }
    buf.writeUInt16LE(first, o); buf.writeUInt16LE(nfine, o + 2); buf.writeUInt16LE(ncoarse, o + 4);   // ncoarse 0: use the fine ones
    buf.writeUInt16LE(vfirst, o + 6); buf.writeUInt16LE(vcount, o + 8);
    buf.writeUInt16LE(nroof, o + 10);   // the last nroof faces of fine and of coarse are the roof
    o += 14;
  }
  for (const sg of segs) { sg.c.forEach((v, k) => buf.writeInt16LE(v, o + k * 2)); buf.writeUInt16LE(sg.name, o + 8); buf.writeUInt8(sg.cls, o + 10); o += 12; }
  if (near) {
    maxVerts = Math.max(maxVerts, t.verts.length);
    if (size > maxBytes) { maxBytes = size; worst = `${t.tx},${t.tz}`; }
  } else maxFar = Math.max(maxFar, size);
  return buf;
}

// Each tile on the disc: its far blob, then its near blob, both from a sector
// start. Index entry (u32): first sector << 10 | far sectors << 5 | near sectors.
const nTiles = tiles.length;
const headerBytes = 64 + nTiles * 4;
const headerSectors = Math.ceil(headerBytes / SECTOR);
const header = Buffer.alloc(headerSectors * SECTOR);
const parts = [];
let sector = headerSectors;
for (let i = 0; i < nTiles; i++) {
  const far = encodeTile(farTiles[i], false), near = encodeTile(tiles[i], true);
  const fs = Math.ceil(far.length / SECTOR), ns = Math.ceil(near.length / SECTOR);
  if (fs > 31 || ns > 31 || sector >= 1 << 22) throw new Error('tile too big for the index');
  header.writeUInt32LE(((sector << 10) | (fs << 5) | ns) >>> 0, 64 + i * 4);
  parts.push([sector, far], [sector + fs, near]);
  sector += fs + ns;
}
const globalSector = sector, globalSectors = Math.ceil(global.length / SECTOR);
sector += globalSectors;
const graphSectors = Math.ceil(graph.length / SECTOR);   // the graph follows the global section
sector += graphSectors;
header.write('VMAP', 0, 'ascii');
header.writeUInt16LE(4, 4);
header.writeUInt16LE(tilesX, 6);
header.writeUInt16LE(tilesZ, 8);
header.writeUInt16LE(TILE, 10);
header.writeUInt16LE(headerSectors, 12);
header.writeUInt16LE(graphSectors, 14);
header.writeInt32LE(Math.round(spawn[0] * UNIT), 16);
header.writeInt32LE(Math.round(spawn[1] * UNIT), 20);
header.writeInt16LE(Math.round(((spawn.heading ?? 0) / (2 * Math.PI)) * 4096) & 4095, 24);
header.writeUInt16LE(globalSectors, 26);
header.write(mapLabel.toUpperCase().slice(0, 31), 28, 'latin1');
header.writeUInt32LE(globalSector, 60);

const out = Buffer.alloc(sector * SECTOR);
header.copy(out, 0);
for (const [sec, blob] of parts) blob.copy(out, sec * SECTOR);
global.copy(out, globalSector * SECTOR);
graph.copy(out, (globalSector + globalSectors) * SECTOR);
writeFileSync(outPath, out);

const report = {
  source: inPath, areaMetres: [Math.round(widthM), Math.round(heightM)], tiles: [tilesX, tilesZ],
  buildings: stats.buildings, buildingParts: stats.buildingParts, buildingsWithDefaultHeight: stats.defaultHeights,
  buildingsWithColour: stats.colouredBuildings, roads: stats.roads, roadKm: +stats.roadKm.toFixed(2), areas: stats.areas,
  streetNames: stats.names.size, spawn: { from: spawnFrom, metres: spawn.map((v) => +v.toFixed(1)) },
  largestTile: { at: worst, bytes: maxBytes, maxVerts }, largestFarTile: maxFar, fileBytes: out.length,
  places: places.length, fuelStations: places.filter((p) => p.kind === 1).length, mechanics: places.filter((p) => p.kind === 2).length,
  minimapSegments: segs.length, globalBytes: global.length, roadGraph: stats.graph, landmarks: landmarkStats, signs: stats.signs, signList: signs.map((x) => `${x.lines.join(' / ')} @ ${x.at}`), buildingsReplacedByLandmarks: stats.replacedByLandmarks,
  terrain: dem ? dem.source : 'flat', extraFiles: extraPaths,
  guessedHeights: levelStats,
  coastKm: +stats.coastKm.toFixed(2), seaGroundCells: stats.seaCells, featuresOutsideBox: stats0.outside,
};
if (reportPath) writeFileSync(reportPath, JSON.stringify({ ...report, streetNameList: [...stats.names].sort(), placeList: places.map((p) => names[p.name]) }, null, 2));
console.log(`osm2map: ${report.buildings} buildings (${report.buildingsWithDefaultHeight} with a guessed height), ${report.roads} roads (${report.roadKm} km), ${report.areas} areas, ${report.places} places (${report.fuelStations} fuel, ${report.mechanics} mechanics), ${tilesX}x${tilesZ} tiles, largest tile ${maxBytes} bytes (far ${maxFar}), ${out.length} bytes total`);
