#!/usr/bin/env node
// Builds the low-poly Mercedes-Benz W124 taxi and writes src/car_model.h.
//
//   node tools/makecar.mjs
//
// Real W124 saloon size: 4.74 m long, 1.74 m wide, 1.42 m tall, 2.80 m
// wheelbase. Units are 1/16 m; car space is X right, Y down, Z forward.
// Faces are given as point loops (any direction); the script orders them
// for the GPU (Z order, clockwise seen from outside) and bakes simple
// lighting into each face's colour.
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const M = 16;
const P = (x, y, z) => [Math.round(x * M), Math.round(-y * M), Math.round(z * M)]; // metres, y up

const verts = [], vkey = new Map(), faces = [];
const v = (p) => { const k = p.join(); if (!vkey.has(k)) { vkey.set(k, verts.length); verts.push(p); } return vkey.get(k); };
const sub = (a, b) => a.map((x, i) => x - b[i]);
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const norm = (a) => { const l = Math.hypot(...a) || 1; return a.map((x) => x / l); };

// centre: a point inside the part the face belongs to (to find "outside").
function face(loop, rgb, centre, glow = GLOW.has(rgb)) {
  const fc = loop.reduce((s, p) => s.map((x, i) => x + p[i] / loop.length), [0, 0, 0]);
  let n = cross(sub(loop[1], loop[0]), sub(loop[2], loop[0]));
  if (dot(n, sub(fc, centre)) > 0) { loop = loop.slice().reverse(); n = n.map((x) => -x); }
  // Clockwise from outside means n points inwards; outward normal is -n.
  const out = norm(n.map((x) => -x));
  // Light from above (-Y), a little from the front-left.
  const light = norm([-0.3, -1, 0.4]);
  const k = 0.55 + 0.45 * Math.max(0, dot(out, light));
  const col = rgb.map((c) => Math.min(255, Math.round(c * k)));
  const idx = loop.map(v);
  faces.push({ v: idx.length === 4 ? [idx[0], idx[1], idx[3], idx[2]] : [idx[0], idx[1], idx[2], idx[2]], tri: idx.length === 3, glow, col: glow ? rgb : col });
}

// Convex prism between two cross-sections given at z0 and z1 (lists of [x,y] in metres).
function prism(section0, z0, section1, z1, rgb, { capFront = true, capBack = true, skip = [] } = {}) {
  const c = [0, 0, 0];
  const pts = [...section0.map(([x, y]) => P(x, y, z0)), ...section1.map(([x, y]) => P(x, y, z1))];
  pts.forEach((p) => p.forEach((x, i) => (c[i] += x / pts.length)));
  const n = section0.length;
  for (let i = 0; i < n; i++) {
    if (skip.includes(i)) continue;
    const j = (i + 1) % n;
    face([P(...section0[i], z0), P(...section0[j], z0), P(...section1[j], z1), P(...section1[i], z1)], typeof rgb === 'function' ? rgb(i) : rgb, c);
  }
  const cap = (sec, z) => {
    if (sec.length === 4) face(sec.map(([x, y]) => P(x, y, z)), typeof rgb === 'function' ? rgb('cap') : rgb, c);
    else for (let i = 1; i + 1 < sec.length; i++) face([P(...sec[0], z), P(...sec[i], z), P(...sec[i + 1], z)], typeof rgb === 'function' ? rgb('cap') : rgb, c);
  };
  if (capBack) cap(section0, z0);
  if (capFront) cap(section1, z1);
}

const WHITE = [228, 226, 218], GLASS = [46, 58, 72], DARK = [44, 44, 46], TYRE = [24, 24, 26], CHROME = [170, 172, 176];
const HEAD = [250, 245, 210], TAIL = [190, 30, 30], SIGN = [250, 205, 40], PLATE = [235, 235, 235];
// Lights and the roof sign glow: not darkened at night.
const GLOW = new Set([HEAD, TAIL, SIGN]);

// Lower body: from the rear bumper (z -2.37) to the front bumper (z +2.37),
// sills at 0.30 m, belt line at 1.00 m. Slight taper at both ends.
const body = (w, yb, yt) => [[-w, yb], [w, yb], [w, yt], [-w, yt]];
prism(body(0.84, 0.30, 0.98), -2.30, body(0.87, 0.30, 1.00), -1.40, WHITE);       // trunk section
prism(body(0.87, 0.30, 1.00), -1.40, body(0.87, 0.30, 1.00), 1.25, WHITE, { capBack: false, capFront: false, skip: [2] });
prism(body(0.87, 0.30, 0.98), 1.25, body(0.84, 0.30, 0.93), 2.30, WHITE);         // engine bay
// Cabin floor top between the screens (under the glass house) is hidden.
// Bumpers (dark plastic, W124 style)
prism(body(0.86, 0.26, 0.48), 2.30, body(0.84, 0.26, 0.46), 2.37, DARK);
prism(body(0.84, 0.26, 0.46), -2.37, body(0.86, 0.26, 0.48), -2.30, DARK);
// Glass house: windscreen base z +1.25, rear screen base z -1.40, roof 1.42 m.
const gh = (w, y) => [[-w, 1.0], [w, 1.0], [w * 0.83, y], [-w * 0.83, y]];
// Side windows between the roof's ends (the slopes get triangles below).
prism(gh(0.86, 1.42), -0.85, gh(0.86, 1.42), 0.55, GLASS, { capBack: false, capFront: false, skip: [0, 2] });
// Roof panel, rear screen and windscreen (the slopes).
{
  const c = P(0, 1.2, 0);
  const rw = 0.86 * 0.83;
  face([P(-rw, 1.42, -0.85), P(rw, 1.42, -0.85), P(rw, 1.42, 0.55), P(-rw, 1.42, 0.55)], WHITE, c);          // roof
  face([P(-0.86, 1.0, 1.25), P(0.86, 1.0, 1.25), P(rw, 1.42, 0.55), P(-rw, 1.42, 0.55)], GLASS, c);           // windscreen
  face([P(-0.86, 1.0, -1.40), P(0.86, 1.0, -1.40), P(rw, 1.42, -0.85), P(-rw, 1.42, -0.85)], GLASS, c);       // rear screen
  // Side windows as triangles filling the slopes.
  for (const s of [-1, 1]) {
    face([P(s * 0.86, 1.0, 1.25), P(s * rw, 1.42, 0.55), P(s * 0.86, 1.0, 0.55)], GLASS, c);
    face([P(s * 0.86, 1.0, -1.40), P(s * rw, 1.42, -0.85), P(s * 0.86, 1.0, -0.85)], GLASS, c);
  }
}
// TAXI roof sign.
prism(body(0.30, 1.42, 1.58), -0.20, body(0.30, 1.42, 1.58), 0.10, SIGN);
// Lights and plates (slightly proud of the body).
for (const s of [-1, 1]) {
  face([P(s * 0.40, 0.70, 2.31), P(s * 0.80, 0.70, 2.31), P(s * 0.80, 0.86, 2.31), P(s * 0.40, 0.86, 2.31)], HEAD, P(0, 0.7, 0));
  face([P(s * 0.45, 0.62, -2.31), P(s * 0.84, 0.62, -2.31), P(s * 0.84, 0.88, -2.31), P(s * 0.45, 0.88, -2.31)], TAIL, P(0, 0.7, 0));
}
face([P(-0.22, 0.70, 2.31), P(0.22, 0.70, 2.31), P(0.22, 0.86, 2.31), P(-0.22, 0.86, 2.31)], CHROME, P(0, 0.7, 0)); // grille
face([P(-0.26, 0.50, -2.375), P(0.26, 0.50, -2.375), P(0.26, 0.61, -2.375), P(-0.26, 0.61, -2.375)], PLATE, P(0, 0.7, 0));
// Wheels: octagonal outer faces, 0.62 m tyres, wheelbase 2.80 m.
for (const s of [-1, 1]) for (const z of [-1.40, 1.40]) {
  const r = 0.31, cy = 0.31;
  const ring = Array.from({ length: 8 }, (_, k) => [s * 0.88, cy + r * Math.sin(k * Math.PI / 4 + Math.PI / 8), z + r * Math.cos(k * Math.PI / 4 + Math.PI / 8)]);
  const ctr = P(s * 0.70, cy, z);
  for (const [a, b, c2, d] of [[0, 1, 2, 3], [0, 3, 4, 7], [4, 5, 6, 7]]) face([P(...ring[a]), P(...ring[b]), P(...ring[c2]), P(...ring[d])], TYRE, ctr);
  // hub cap
  const hub = Array.from({ length: 4 }, (_, k) => [s * 0.885, cy + 0.15 * Math.sin(k * Math.PI / 2), z + 0.15 * Math.cos(k * Math.PI / 2)]);
  face(hub.map((p) => P(...p)), CHROME, ctr);
}

if (verts.length > 255) throw new Error('too many vertices');
writeFileSync(path.join(root, 'src', 'car_model.h'), `/* Generated by tools/makecar.mjs: do not edit. Mercedes-Benz W124 taxi, 1/16 m units. */
#pragma once
#define CAR_NVERTS ${verts.length}
#define CAR_NFACES ${faces.length}
static const SVECTOR car_verts[CAR_NVERTS] = {
${verts.map((p) => `\t{ ${p.join(', ')}, 0 },`).join('\n')}
};
/* v[4] in GPU Z order (a triangle repeats its last vertex), flags (1 = triangle, 2 = glows), colour. */
static const uint8_t car_faces[CAR_NFACES][8] = {
${faces.map((f) => `\t{ ${f.v.join(', ')}, ${(f.tri ? 1 : 0) | (f.glow ? 2 : 0)}, ${f.col.join(', ')} },`).join('\n')}
};
`);
console.log(`car_model.h: ${verts.length} vertices, ${faces.length} faces`);
