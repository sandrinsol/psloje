#!/usr/bin/env node
// Writes testtown.osm: a small made-up town (NOT a real place) used to test
// tools/osm2map.mjs and the game until real OpenStreetMap data is added.
// A 5x5 street grid 80 m apart, one wider main road, a pedestrian street, a
// park, and buildings of different heights and colours in every block.
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const LAT0 = 0.001, LON0 = 0.001;            // near 0,0: obviously not a real city
const mPerLat = 110574, mPerLon = 111320;
let id = 1;
const nodes = [], ways = [];
const node = (x, z, tags = {}) => { const n = { id: id++, lat: LAT0 + z / mPerLat, lon: LON0 + x / mPerLon, tags }; nodes.push(n); return n.id; };
const way = (refs, tags) => ways.push({ id: id++, refs, tags });
const rect = (x0, z0, x1, z1, tags) => { const a = node(x0, z0), b = node(x1, z0), c = node(x1, z1), d = node(x0, z1); way([a, b, c, d, a], tags); };

const N = 5, STEP = 80, W = (N - 1) * STEP;
const grid = [];
for (let j = 0; j < N; j++) { grid.push([]); for (let i = 0; i < N; i++) grid[j].push(node(20 + i * STEP, 20 + j * STEP)); }
for (let j = 0; j < N; j++) way(grid[j], { highway: j === 2 ? 'primary' : 'residential', name: j === 2 ? 'Main Road' : `Test Street ${j + 1}` });
for (let i = 0; i < N; i++) way(grid.map((row) => row[i]), { highway: i === 2 ? 'pedestrian' : 'residential', name: i === 2 ? 'Walk Street' : `Test Avenue ${i + 1}` });

const colours = ['#e8dcc0', '#d9a77a', '#c8c8c0', '#e0c070', undefined];
let k = 0;
for (let j = 0; j + 1 < N; j++) for (let i = 0; i + 1 < N; i++) {
  const x0 = 20 + i * STEP, z0 = 20 + j * STEP;
  if (i === 1 && j === 1) { rect(x0 + 8, z0 + 8, x0 + STEP - 8, z0 + STEP - 8, { leisure: 'park', name: 'Test Park' }); continue; }
  for (let b = 0; b < 4; b++) {
    const bx = x0 + 9 + (b % 2) * 32, bz = z0 + 9 + Math.floor(b / 2) * 32;
    const tags = { building: b === 3 ? 'house' : 'apartments' };
    if (b !== 3) tags['building:levels'] = String(2 + ((k * 7) % 8));
    const c = colours[k % colours.length];
    if (c) tags['building:colour'] = c;
    rect(bx, bz, bx + 26 - (b % 2) * 4, bz + 24, tags);
    k++;
  }
}
node(20 + 2 * STEP, 20 + 2 * STEP, { name: 'Test Square' });

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
const tagXml = (t) => Object.entries(t).map(([k, v]) => `    <tag k="${esc(k)}" v="${esc(v)}"/>\n`).join('');
let xml = `<?xml version="1.0" encoding="UTF-8"?>\n<osm version="0.6" generator="make-testtown">\n`;
xml += `  <bounds minlat="${LAT0}" minlon="${LON0}" maxlat="${LAT0 + (W + 40) / mPerLat}" maxlon="${LON0 + (W + 40) / mPerLon}"/>\n`;
for (const n of nodes) xml += Object.keys(n.tags).length ? `  <node id="${n.id}" lat="${n.lat.toFixed(7)}" lon="${n.lon.toFixed(7)}">\n${tagXml(n.tags)}  </node>\n` : `  <node id="${n.id}" lat="${n.lat.toFixed(7)}" lon="${n.lon.toFixed(7)}"/>\n`;
for (const w of ways) xml += `  <way id="${w.id}">\n${w.refs.map((r) => `    <nd ref="${r}"/>\n`).join('')}${tagXml(w.tags)}  </way>\n`;
xml += '</osm>\n';
writeFileSync(path.join(path.dirname(fileURLToPath(import.meta.url)), 'testtown.osm'), xml);
console.log(`testtown.osm: ${nodes.length} nodes, ${ways.length} ways`);
