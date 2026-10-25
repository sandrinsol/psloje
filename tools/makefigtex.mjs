#!/usr/bin/env node
// Textures for the figures and effects: assets/figtex.tim, 4-bit, 256 x 128
// at VRAM (FIG_X, FIG_Y), one CLUT per material (rows from FIG_CLUT_Y), and
// src/figtex.h.
//
//   node tools/makefigtex.mjs
//
// Row 0 (v 0..63): the materials, 64 x 64 each: robe, ash skin, stone, metal.
// Row 1 (v 64..127): effects: cloud, the eye in the sky, a stone chunk, a
// soft glow.
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const FIG_X = 832, FIG_Y = 256, CLUT_X = 0, CLUT_Y = 484;
const W = 256, H = 128;

let seed = 1234;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8) / 16777216;
// Smooth value noise in 0..1, tiling every `period` pixels.
function noiseField(period, octaves = 3) {
  const grids = [];
  for (let o = 0; o < octaves; o++) {
    const n = period >> o || 1;
    grids.push({ n, g: Array.from({ length: n * n }, rnd) });
  }
  return (x, y) => {
    let v = 0, amp = 1, sum = 0;
    for (const { n, g } of grids) {
      const s = 64 / n, fx = x / s, fy = y / s, x0 = Math.floor(fx), y0 = Math.floor(fy), tx = fx - x0, ty = fy - y0;
      const at = (i, j) => g[((j % n + n) % n) * n + ((i % n + n) % n)];
      const sm = (t) => t * t * (3 - 2 * t);
      const a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sm(tx);
      const b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sm(tx);
      v += (a + (b - a) * sm(ty)) * amp;
      sum += amp;
      amp /= 2;
    }
    return v / sum;
  };
}

const img = new Uint8Array(W * H);
const put = (tile, row, fn) => {
  for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) img[(row * 64 + y) * W + tile * 64 + x] = fn(x, y);
};
const q = (v, lo = 1, hi = 15) => Math.max(lo, Math.min(hi, Math.round(v)));

// Robe: wool with vertical folds and a ragged, darker hem.
const robeN = noiseField(8);
put(0, 0, (x, y) => {
  const fold = Math.sin((x / 64) * Math.PI * 6 + robeN(x, y) * 3) * 0.5 + 0.5;
  let v = 4 + fold * 7 + robeN(x * 3, y * 3) * 3;
  if (y > 54 && ((x * 7) % 11) < 4) v -= 4;   // ragged hem
  return q(v);
});
// Ash skin: grey with dark cracks, a few ember-lit cracks (indices 14-15).
const ashN = noiseField(4), crackN = noiseField(16, 2);
put(1, 0, (x, y) => {
  const c = Math.abs(crackN(x, y) - 0.5);
  if (c < 0.025) return (x + y) % 9 === 0 ? 15 : 14;   // ember glow in deep cracks
  if (c < 0.05) return 2;
  return q(5 + ashN(x * 2, y * 2) * 8);
});
// Stone: blocks with lichen.
const stN = noiseField(8);
put(2, 0, (x, y) => (y % 32 === 0 || (x + (y >> 5) * 21) % 42 === 0 ? 2 : q(5 + stN(x, y) * 8 - (stN(x * 4, y * 4) > 0.72 ? 3 : 0))));
// Metal: bright streaks along the blade.
put(3, 0, (x, y) => q(8 + Math.sin(y / 3 + x / 20) * 3 + (x % 16 < 2 ? 4 : 0)));

// Cloud: a soft blob of noise, darker towards its edges.
const clN = noiseField(4, 4);
put(0, 1, (x, y) => {
  const dx = (x - 31.5) / 32, dy = (y - 31.5) / 32, r = Math.sqrt(dx * dx + dy * dy);
  const v = clN(x, y) * (1 - r) * 22;
  return v < 1.5 ? 0 : q(v);
});
// The eye in the sky: a pale almond with a dark slit.
put(1, 1, (x, y) => {
  const dx = (x - 31.5) / 30, dy = (y - 31.5) / 14;
  const inAlmond = Math.abs(dy) < 1 - dx * dx;
  if (!inAlmond) return 0;
  const slit = Math.abs(x - 31.5) < 3 - Math.abs(y - 31.5) / 8;
  if (slit) return 1;
  const iris = Math.hypot(x - 31.5, (y - 31.5) * 1.6) < 13;
  return iris ? q(6 + Math.hypot(x - 31.5, y - 31.5) / 2) : q(12 + rnd() * 2);
});
// Stone chunk: an irregular shard.
put(2, 1, (x, y) => {
  const a = Math.atan2(y - 32, x - 32), r = Math.hypot(x - 32, y - 32);
  const edge = 22 + Math.sin(a * 3) * 6 + Math.cos(a * 5) * 4;
  if (r > edge) return 0;
  return q(4 + stN(x, y) * 8 + (x < y ? 2 : 0));
});
// Glow: a soft round light.
put(3, 1, (x, y) => {
  const r = Math.hypot(x - 31.5, y - 31.5) / 32;
  return r >= 1 ? 0 : q((1 - r) * (1 - r) * 15, 0);
});

// CLUTs: index 0 see-through, then the material's ramp.
const ramp = (from, to, n = 15) => Array.from({ length: n }, (_, i) => from.map((c, k) => Math.round(c + (to[k] - c) * i / (n - 1))));
const CLUTS = {
  robe: ramp([30, 28, 30], [200, 194, 186]),
  ash: [...ramp([26, 24, 24], [190, 184, 176], 13), [210, 110, 40], [255, 190, 90]],
  stone: ramp([28, 28, 30], [170, 166, 160]),
  metal: ramp([60, 64, 72], [250, 252, 255]),
  cloud: ramp([20, 20, 22], [150, 146, 150]),
  eye: [[8, 4, 4], ...ramp([90, 30, 20], [240, 230, 200], 14)],
  glow: ramp([0, 0, 0], [255, 255, 255]),
};
// Bit 15 (STP) on every colour: textured polygons are only semi-transparent
// (additive clouds and glows) where it's set; opaque ones ignore it.
const to15 = ([r, g, b]) => (((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)) || 0x0421) | 0x8000;
const names = Object.keys(CLUTS);
const clutBytes = 12 + 32 * names.length, imageBytes = 12 + (W / 2) * H;
const tim = Buffer.alloc(8 + clutBytes + imageBytes);
tim.writeUInt32LE(0x10, 0);
tim.writeUInt32LE(0x08, 4);
let o = 8;
tim.writeUInt32LE(clutBytes, o); tim.writeUInt16LE(CLUT_X, o + 4); tim.writeUInt16LE(CLUT_Y, o + 6); tim.writeUInt16LE(16, o + 8); tim.writeUInt16LE(names.length, o + 10);
names.forEach((n, j) => { tim.writeUInt16LE(0, o + 12 + j * 32); CLUTS[n].forEach((c, i) => tim.writeUInt16LE(to15(c), o + 12 + j * 32 + (i + 1) * 2)); });
o += clutBytes;
tim.writeUInt32LE(imageBytes, o); tim.writeUInt16LE(FIG_X, o + 4); tim.writeUInt16LE(FIG_Y, o + 6); tim.writeUInt16LE(W / 4, o + 8); tim.writeUInt16LE(H, o + 10);
o += 12;
for (let i = 0; i < img.length; i += 2) tim[o + (i >> 1)] = img[i] | (img[i + 1] << 4);
writeFileSync(path.join(root, 'assets', 'figtex.tim'), tim);
writeFileSync(path.join(root, 'src', 'figtex.h'), `/* Generated by tools/makefigtex.mjs: do not edit. */
#pragma once
#define FIG_TPAGE_X ${FIG_X}
#define FIG_TPAGE_Y ${FIG_Y}
#define FIG_CLUT_X  ${CLUT_X}
#define FIG_CLUT_Y  ${CLUT_Y}
/* Materials (u = index * 64, v = 0) and their CLUT rows (FIG_CLUT_Y + id). */
enum { MAT_FLAT = -1, MAT_ROBE = 0, MAT_ASH, MAT_STONE, MAT_METAL };
/* Effects (u = index * 64, v = 64) and CLUTs. */
enum { FX_CLOUD, FX_EYE, FX_CHUNK, FX_GLOW };
enum { CLUT_ROBE, CLUT_ASH, CLUT_STONE, CLUT_METAL, CLUT_CLOUD, CLUT_EYE, CLUT_GLOW };
`);
console.log(`figtex.tim: ${W} x ${H}, ${names.length} CLUTs`);
