#!/usr/bin/env node
// The floor's slabs: four 64 x 64 sheets of paper, each with a rune
// brushed in ink, in assets/slabs.tim (4-bit, 256 x 64 at VRAM SLAB_X,
// SLAB_Y) with five CLUTs: normal, the rune in red ink (a warning), the
// sheet washed red (about to be hit), and two fainter (further off: the
// page not yet inked, only pencil). Writes the ids to src/slabs.h.
//
//   node tools/makeslabs.mjs
//
// Colour indices: 1-7 paper (dark to light grain), 8 the sheet's inked
// edge and creases, 9-11 the rune's ink (dry edge, stroke, wet middle),
// 12-15 spare.
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const SLAB_X = 704, SLAB_Y = 0, CLUT_X = 0, CLUT_Y = 493;
const S = 64, N = 4;

let seed = 7;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8) / 16777216;

// Runes as strokes on a 16 x 16 grid (scaled x4 onto the slab's middle).
const RUNES = [
  [[8, 2, 8, 14], [4, 5, 12, 5], [4, 5, 8, 9], [12, 5, 8, 9]],              // a cross with a cup
  [[3, 3, 13, 13], [13, 3, 3, 13], [8, 2, 8, 6], [8, 10, 8, 14]],            // the bound X
  [[4, 3, 4, 13], [4, 3, 12, 8], [12, 8, 4, 13], [9, 9, 13, 14]],            // a broken R / thorn
  [[8, 2, 3, 8], [3, 8, 8, 14], [8, 14, 13, 8], [13, 8, 8, 2], [8, 6, 8, 10]], // the eye-diamond
];

const px = new Uint8Array(N * S * S);
for (let k = 0; k < N; k++) {
  const at = (x, y) => k * S + y * S * N + x;   // 256-wide image: slab k at u = k * 64
  // Paper: fine grain, a hand-drawn border that wobbles, a crease or two.
  for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) {
    let v = 5 + (rnd() < 0.5 ? 1 : 0) + (rnd() < 0.12 ? -1 : 0) + (rnd() < 0.05 ? 1 : 0);
    const edge = Math.min(x, y, S - 1 - x, S - 1 - y);
    if (edge < 3) v -= 1;   // the sheet a touch greyer at its edge
    px[at(x, y)] = Math.max(1, Math.min(7, v));
  }
  const wob = (i) => Math.round(Math.sin(i / 7 + k * 2) * 0.8 + Math.sin(i / 3 + k) * 0.4);
  for (let i = 0; i < S; i++) {
    if (rnd() < 0.93) px[at(i, 1 + wob(i))] = 8;
    if (rnd() < 0.93) px[at(i, S - 2 + wob(i + 9))] = 8;
    if (rnd() < 0.93) px[at(1 + wob(i + 5), i)] = 8;
    if (rnd() < 0.93) px[at(S - 2 + wob(i + 13), i)] = 8;
  }
  for (let c = 0; c < 1; c++) {   // a fold across a corner, in pencil
    let x = 4 + Math.floor(rnd() * 20), y = 3;
    for (let i = 0; i < 26; i++) {
      if (x < 0 || x >= S || y < 0 || y >= S) break;
      if (i % 3) px[at(x, y)] = 4;
      x += rnd() < 0.6 ? 1 : 0; y += 1;
    }
  }
  // The rune: brush strokes, thick in the middle, thinning and dry at
  // their ends (wet middle, stroke, dry edge).
  const carve = (x0, y0, x1, y1) => {
    const n = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0)) * 4;
    for (let i = 0; i <= n; i++) {
      const cx = 8 + (x0 + (x1 - x0) * i / n) * 3, cy = 8 + (y0 + (y1 - y0) * i / n) * 3;
      const w = i / n < 0.12 || i / n > 0.88 ? 1 : 2;
      for (let dy = -w; dy <= w; dy++) for (let dx = -w; dx <= w; dx++) {
        if (w === 2 && Math.abs(dx) === 2 && Math.abs(dy) === 2) continue;
        if (w === 1 && rnd() < 0.3) continue;   // dry brush
        const x = Math.round(cx + dx), y = Math.round(cy + dy), d = Math.max(Math.abs(dx), Math.abs(dy)) + (w === 1 ? 1 : 0);
        if (x < 0 || y < 0 || x >= S || y >= S) continue;
        const want = d === 0 ? 11 : d === 1 ? 10 : 9;
        if (px[at(x, y)] < 9 || want > px[at(x, y)]) px[at(x, y)] = Math.max(px[at(x, y)] >= 9 ? px[at(x, y)] : 0, want);
      }
    }
  };
  for (const s of RUNES[k]) carve(...s);
}

const to15 = ([r, g, b]) => ((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)) || 0x0421;
const paper = [[176, 168, 150], [192, 184, 166], [206, 198, 180], [216, 208, 190], [224, 216, 198], [232, 224, 206], [240, 233, 216]];
const ink = [[48, 44, 52], [30, 28, 36], [16, 14, 22]];
const toward = (c, t, k) => c.map((v, i) => Math.round(v + (t[i] - v) * k));
const blank = [236, 229, 212];
const CLUTS = [
  // normal: the rune in ink
  [[0, 0, 0], ...paper, [70, 64, 70], ...ink, ...Array(4).fill([0, 0, 0])],
  // warning: the rune in red ink
  [[0, 0, 0], ...paper, [90, 40, 40], [170, 60, 50], [150, 30, 26], [120, 14, 12], ...Array(4).fill([0, 0, 0])],
  // about to land: the sheet washed red, the rune bleeding
  [[0, 0, 0], ...paper.map((c) => toward(c, [230, 120, 100], 0.55)), [110, 20, 18], [150, 20, 16], [120, 8, 8], [90, 0, 0], ...Array(4).fill([0, 0, 0])],
  // further off: pencil, not ink yet
  [[0, 0, 0], ...paper.map((c) => toward(c, blank, 0.5)), [140, 136, 134], ...ink.map((c) => toward(c, [120, 116, 118], 0.7)), ...Array(4).fill([0, 0, 0])],
  // far off: the page nearly blank
  [[0, 0, 0], ...paper.map((c) => toward(c, blank, 0.85)), [196, 190, 182], ...ink.map((c) => toward(c, [186, 180, 174], 0.85)), ...Array(4).fill([0, 0, 0])],
];

const clutBytes = 12 + 32 * CLUTS.length, imageBytes = 12 + (N * S / 2) * S;
const tim = Buffer.alloc(8 + clutBytes + imageBytes);
tim.writeUInt32LE(0x10, 0);
tim.writeUInt32LE(0x08, 4);
let o = 8;
tim.writeUInt32LE(clutBytes, o); tim.writeUInt16LE(CLUT_X, o + 4); tim.writeUInt16LE(CLUT_Y, o + 6); tim.writeUInt16LE(16, o + 8); tim.writeUInt16LE(CLUTS.length, o + 10);
CLUTS.forEach((cl, j) => cl.forEach((c, i) => tim.writeUInt16LE(i === 0 ? 0 : to15(c), o + 12 + j * 32 + i * 2)));
o += clutBytes;
tim.writeUInt32LE(imageBytes, o); tim.writeUInt16LE(SLAB_X, o + 4); tim.writeUInt16LE(SLAB_Y, o + 6); tim.writeUInt16LE(N * S / 4, o + 8); tim.writeUInt16LE(S, o + 10);
o += 12;
for (let i = 0; i < px.length; i += 2) tim[o + (i >> 1)] = px[i] | (px[i + 1] << 4);
writeFileSync(path.join(root, 'assets', 'slabs.tim'), tim);
writeFileSync(path.join(root, 'src', 'slabs.h'), `/* Generated by tools/makeslabs.mjs: do not edit. */
#pragma once
#define SLAB_TPAGE_X ${SLAB_X}
#define SLAB_TPAGE_Y ${SLAB_Y}
#define SLAB_CLUT_X  ${CLUT_X}
#define SLAB_CLUT_Y  ${CLUT_Y}   /* + 0 normal, + 1 warning, + 2 about to land, + 3 / + 4 further off */
#define NUM_SLABS    ${N}        /* slab k at u = k * 64 */
`);
console.log(`slabs.tim: ${N} slabs, ${CLUTS.length} CLUTs`);
