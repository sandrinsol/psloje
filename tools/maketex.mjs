#!/usr/bin/env node
// Generates the city's textures: assets/city.tim (8-bit, one shared
// 256-colour CLUT; CITY.TIM on the disc) and src/textures.h (each texture's
// average colour, used for far-away untextured polygons).
//
//   node tools/maketex.mjs
//
// Layout: up to 32 textures of 64x64 in two 256x256 texture pages side by
// side at VRAM (640,256) and (768,256); texture i is in page i >> 4 at
// u = (i & 3) * 64, v = (i >> 2 & 3) * 64. CLUT at (640,248). The third page
// (896,256) holds the map's name signs (tools/osm2map.mjs).
// Order (ids used by tools/osm2map.mjs): asphalt, paving, wallA, wallB, wallC,
// ground, grass, sand.
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
export const TPAGE_X = 640, TPAGE_Y = 256, CLUT_X = 640, CLUT_Y = 248;
const S = 64;
const main = process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1]);

let seed = 12345;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8) / 16777216;
const clamp = (v) => Math.max(0, Math.min(255, Math.round(v)));
const shade = (c, f) => c.map((v) => clamp(v * f));
const noisy = (c, amount, levels = 4) => shade(c, 1 + (Math.floor(rnd() * levels) / (levels - 1) - 0.5) * amount);

function texture(fn) {
  const px = [];
  for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) px.push(fn(x, y));
  return px;
}

// Wall with 2 x 2 window bays (each 32 x 32 = one floor, 4 m wide).
function wall(base, frame, glass, opts = {}) {
  return texture((x, y) => {
    const bx = x % 32, by = y % 32;
    if (opts.seams && (bx === 0 || by === 31)) return shade(base, 0.8);
    if (opts.balcony && by >= 27 && by <= 28 && bx >= 4 && bx <= 27) return shade(frame, 0.7);
    if (bx >= 9 && bx <= 22 && by >= 7 && by <= 24) {
      if (bx === 9 || bx === 22 || by === 7 || by === 24) return frame;
      if (bx === 16 || by === 15) return shade(frame, 0.9);
      return by < 11 ? shade(glass, 1.25) : glass;
    }
    return noisy(base, 0.08);
  });
}

// ---- Landmark textures, hand-painted from reference photos (tools/fetch-refs.py) ----

// Pointed (Ottoman) arch test: inside an arch of half-width w whose straight
// sides end at height spring (y measured up from the sill), apex at top.
const inArch = (dx, y, w, spring, top) => {
  if (Math.abs(dx) > w) return false;
  if (y <= spring) return true;
  // Two circle arcs meeting at the apex.
  const t = (y - spring) / (top - spring);
  return Math.abs(dx) <= w * Math.sqrt(Math.max(0, 1 - t * t)) * (1 - t * 0.35);
};

// 5 x 7 digits for the monument's "1912".
const DIGITS = { 1: ['00100', '01100', '00100', '00100', '00100', '00100', '01110'], 9: ['01110', '10001', '10001', '01111', '00001', '00010', '01100'], 2: ['01110', '10001', '00001', '00010', '00100', '01000', '11111'] };

const LANDMARK_TEXTURES = {
  // Bronze of the Independence Monument's figures: dark olive with streaks of
  // patina and a few bright edges.
  bronze: texture((x, y) => {
    const streak = (x * 7 + Math.floor(y / 9) * 13) % 11 < 2;
    const c = streak ? [86, 90, 70] : [64, 58, 46];
    return y % 16 === 0 ? shade(c, 1.35) : noisy(c, 0.3, 4);
  }),
  // The monument's pillar: rough grey stone in big blocks, with the bronze
  // "1912" in the middle of the upper part (texture top = pillar top).
  monstone: texture((x, y) => {
    const digits = '1912';
    const dx = x - 12, dy = y - 10;
    if (dx >= 0 && dx < 40 && dy >= 0 && dy < 14) {
      const d = Math.floor(dx / 10), cx = Math.floor((dx % 10) / 2), cy = Math.floor(dy / 2);
      if (cx < 5 && DIGITS[digits[d]][cy][cx] === '1') return [70, 64, 50];
    }
    const block = (Math.floor(y / 21) * 3 + Math.floor((x + (Math.floor(y / 21) % 2) * 17) / 34)) % 4;
    if (y % 21 === 0 || (x + (Math.floor(y / 21) % 2) * 17) % 34 === 0) return [96, 94, 90];
    return noisy([[138, 136, 130], [128, 126, 122], [146, 142, 134], [132, 130, 126]][block], 0.14, 4);
  }),
  // Muradie Mosque walls: courses of pale limestone with thin red brick
  // between them (64 px = 4 m), one tall pointed-arch window in the middle.
  mosquewin: texture((x, y) => {
    const up = 63 - y;   // from the floor
    const dx = x - 31.5;
    if (inArch(dx, up - 14, 9, 26, 40)) {
      if (inArch(dx, up - 14, 7, 26, 37)) return (Math.round(dx) === 0 || (up - 14) % 9 === 0) ? [40, 34, 30] : [26, 30, 38];
      return [226, 214, 186];   // stone frame
    }
    if (inArch(dx, up - 14, 12, 26, 44) && up > 40) return [176, 104, 76];   // relieving arch in brick
    if (y % 8 === 7) return [172, 120, 92];
    return noisy(y % 8 < 2 ? [188, 166, 128] : [214, 198, 160], 0.12, 4);
  }),
  mosquewall: texture((x, y) => {
    if (y % 8 === 7) return [172, 120, 92];
    if (y % 8 === 3 && (x + Math.floor(y / 8) * 11) % 16 === 0) return [176, 158, 124];
    return noisy(y % 8 < 2 ? [196, 178, 140] : [214, 198, 160], 0.12, 4);
  }),
  // Lead-grey dome / drum roof with ribs.
  lead: texture((x, y) => (x % 16 === 0 ? [96, 100, 106] : noisy([132, 136, 140], 0.12, 4))),
  // Minaret shaft: pale stone with thin bands.
  minaret: texture((x, y) => (y % 16 === 0 ? [176, 160, 130] : x % 16 === 0 ? [190, 176, 148] : noisy([222, 210, 182], 0.1, 4))),
  // Terracotta roof tiles (museum, older houses).
  tiles: texture((x, y) => {
    const row = Math.floor(y / 6), off = (row % 2) * 4;
    if (y % 6 === 5) return [118, 52, 38];
    if ((x + off) % 8 === 0) return [140, 62, 44];
    return noisy([184, 86, 58], 0.14, 4);
  }),
  // Independence Museum: cream plaster, one window a floor (4 m x 3.5 m per
  // 64 px... the window in a white frame with brown louvred shutters).
  museumwin: texture((x, y) => {
    const up = 63 - y;
    if (up < 4) return [176, 168, 150];                    // plinth
    if (x >= 23 && x <= 40 && up >= 14 && up <= 50) {
      if (x === 23 || x === 40 || up === 14 || up === 50) return [244, 240, 228];
      return (up % 3 === 0) ? [86, 54, 32] : [118, 74, 42];   // closed shutters
    }
    if (up === 12 && x >= 21 && x <= 42) return [240, 236, 222];   // sill
    return noisy([228, 210, 168], 0.06, 4);
  }),
  // Museum ground floor door with the balcony above it (the front's middle).
  museumdoor: texture((x, y) => {
    const up = 63 - y;
    if (up < 4) return [176, 168, 150];
    if (x >= 24 && x <= 39 && up >= 4 && up <= 48) {
      if (x === 24 || x === 39 || up === 48) return [244, 240, 228];
      return x === 31 || x === 32 ? [70, 40, 22] : noisy([110, 62, 34], 0.1, 3);
    }
    return noisy([228, 210, 168], 0.06, 4);
  }),
  // Balcony railing: dark iron bars, see-through where the colour is 0.
  railing: texture((x, y) => (y < 3 || y > 60 || x % 6 === 0 ? [44, 40, 36] : [228, 210, 168])),
  // Schools: warm pastel plaster (tinted per school), wide classroom windows
  // in white frames, a band between floors (64 px = 4 m x one 3.5 m floor... x2).
  school: texture((x, y) => {
    const by = y % 32;
    if (by >= 28) return [236, 232, 220];                                 // floor band
    if (x >= 4 && x <= 59 && by >= 7 && by <= 22) {
      if (x === 4 || x === 59 || by === 7 || by === 22 || x === 22 || x === 41) return [244, 244, 240];
      return by < 11 ? [92, 116, 140] : [62, 82, 104];
    }
    return noisy([230, 196, 120], 0.06, 4);
  }),
  // Hospitals and clinics: white with a blue band under the windows.
  hospital: texture((x, y) => {
    const by = y % 32;
    if (by >= 25 && by <= 27) return [70, 120, 180];
    if (x % 32 >= 6 && x % 32 <= 25 && by >= 8 && by <= 21) return (x % 32 === 6 || x % 32 === 25 || by === 8 || by === 21) ? [210, 214, 220] : [70, 96, 120];
    return noisy([236, 236, 232], 0.05, 4);
  }),
  // Teatri Petro Marko: a 4 m bay, 8 m tall: white stone pilasters, crimson
  // plaster, a tall wooden door with a transom, a framed panel above.
  theatredoor: texture((x, y) => {
    const up = 63 - y;   // 8 px a metre
    if (x < 7 || x > 56) return (x === 6 || x === 57) ? [200, 196, 186] : noisy([236, 232, 222], 0.05, 3);
    if (up < 2) return [210, 204, 194];
    if (x >= 20 && x <= 43 && up >= 2 && up <= 38) {
      if (x === 20 || x === 43 || up === 38 || up === 29) return [240, 236, 226];
      if (up > 29) return [70, 60, 50];                       // transom glass
      return x === 31 || x === 32 ? [70, 40, 22] : (up % 6 === 0 ? [96, 56, 30] : [128, 76, 40]);
    }
    if (x >= 17 && x <= 46 && up >= 44 && up <= 58) return (x === 17 || x === 46 || up === 44 || up === 58) ? [240, 236, 226] : [178, 30, 54];
    return noisy([196, 36, 62], 0.06, 3);
  }),
  theatrewall: texture((x, y) => {
    const up = 63 - y;
    if (x < 7 || x > 56) return (x === 6 || x === 57) ? [200, 196, 186] : noisy([236, 232, 222], 0.05, 3);
    if (up < 2) return [210, 204, 194];
    if (x >= 17 && x <= 46 && ((up >= 8 && up <= 36) || (up >= 44 && up <= 58))) {
      const edge = x === 17 || x === 46 || up === 8 || up === 36 || up === 44 || up === 58;
      return edge ? [240, 236, 226] : [178, 30, 54];
    }
    return noisy([196, 36, 62], 0.06, 3);
  }),
  // Flag Square paving: big light slabs with darker joints.
  square: texture((x, y) => (x % 32 === 0 || y % 32 === 0 ? [150, 146, 136] : (x + y) % 32 === 16 && x % 32 > 12 && x % 32 < 20 ? [176, 170, 160] : noisy([204, 200, 190], 0.08, 4))),
};

const textures = [
  // asphalt with a dashed centre line
  texture((x, y) => (x >= 31 && x <= 32 && y % 32 >= 4 && y % 32 < 20 ? [225, 220, 200] : noisy([78, 78, 82], 0.25, 5))),
  // paving slabs
  texture((x, y) => (x % 16 === 0 || y % 16 === 0 ? [140, 134, 124] : noisy([186, 178, 164], 0.12))),
  // wallA: light plaster (tinted by building:colour)
  wall([210, 206, 196], [235, 235, 230], [60, 72, 88], { balcony: true }),
  // wallB: warm ochre plaster
  wall([214, 176, 128], [240, 230, 210], [58, 66, 80]),
  // wallC: grey concrete panels
  wall([168, 166, 160], [200, 198, 192], [52, 60, 70], { seams: true }),
  // ground: dusty concrete / earth
  texture(() => noisy([158, 150, 134], 0.16, 5)),
  // grass
  texture(() => noisy([92, 132, 64], 0.3, 5)),
  // sand
  texture(() => noisy([216, 198, 156], 0.12, 4)),
  // then the landmarks' (ids from TEXTURE_NAMES below)
  ...Object.values(LANDMARK_TEXTURES),
];
// Texture ids by name, in order: tools/osm2map.mjs and tools/landmarks.mjs use these.
export const TEXTURE_NAMES = ['asphalt', 'paving', 'wallA', 'wallB', 'wallC', 'ground', 'grass', 'sand', ...Object.keys(LANDMARK_TEXTURES)];

// 15-bit colours, shared palette.
const to15 = ([r, g, b]) => (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
// More than 256 colours: keep the most used 256 and map the rest to the
// nearest of those.
const counts = new Map();
for (const t of textures) for (const c of t) { const k = to15(c) || 0x0421; counts.set(k, (counts.get(k) || 0) + 1); }   // 0x0000 is transparent on the PS1
const keep = [...counts.keys()].sort((a, b) => counts.get(b) - counts.get(a)).slice(0, 256);
const palette = new Map(keep.map((k, i) => [k, i]));
const rgbOf = (k) => [k & 31, (k >> 5) & 31, (k >> 10) & 31];
const nearest = new Map();
const indexOf = (k) => {
  if (palette.has(k)) return palette.get(k);
  if (!nearest.has(k)) {
    const [r, g, b] = rgbOf(k);
    let best = 0, bd = Infinity;
    for (const [q, i] of palette) { const [r2, g2, b2] = rgbOf(q); const d = (r - r2) ** 2 + (g - g2) ** 2 + (b - b2) ** 2; if (d < bd) { bd = d; best = i; } }
    nearest.set(k, best);
  }
  return nearest.get(k);
};
const indexed = textures.map((t) => t.map((c) => indexOf(to15(c) || 0x0421)));

if (textures.length > 32) throw new Error('more than 32 textures');
const W = 512, H = 256;   // pixels: two pages
const clutBytes = 12 + 512, imageBytes = 12 + W * H;
const tim = Buffer.alloc(8 + clutBytes + imageBytes);
tim.writeUInt32LE(0x10, 0);
tim.writeUInt32LE(0x09, 4); // 8 bpp, has CLUT
let o = 8;
tim.writeUInt32LE(clutBytes, o); tim.writeUInt16LE(CLUT_X, o + 4); tim.writeUInt16LE(CLUT_Y, o + 6); tim.writeUInt16LE(256, o + 8); tim.writeUInt16LE(1, o + 10);
for (const [k, i] of palette) tim.writeUInt16LE(k, o + 12 + i * 2);
o += clutBytes;
tim.writeUInt32LE(imageBytes, o); tim.writeUInt16LE(TPAGE_X, o + 4); tim.writeUInt16LE(TPAGE_Y, o + 6); tim.writeUInt16LE(W / 2, o + 8); tim.writeUInt16LE(H, o + 10);
o += 12;
indexed.forEach((t, i) => {
  const u0 = (i >> 4) * 256 + (i & 3) * 64, v0 = (i >> 2 & 3) * 64;
  t.forEach((p, j) => { tim[o + (v0 + Math.floor(j / S)) * W + u0 + (j % S)] = p; });
});
if (main) writeFileSync(path.join(root, 'assets', 'city.tim'), tim);

const avg = textures.map((t) => [0, 1, 2].map((c) => Math.round(t.reduce((s, p) => s + p[c], 0) / t.length)));
if (main) writeFileSync(path.join(root, 'src', 'textures.h'), `/* Generated by tools/maketex.mjs: do not edit. */
#pragma once
#define CITY_TPAGE_X ${TPAGE_X}
#define CITY_TPAGE_Y ${TPAGE_Y}
#define CITY_CLUT_X  ${CLUT_X}
#define CITY_CLUT_Y  ${CLUT_Y}
#define NUM_TEXTURES ${textures.length}
/* Average colour of each texture (for far, untextured polygons). */
static const uint8_t texture_avg[NUM_TEXTURES][3] = {
${avg.map((c) => `\t{ ${c.join(', ')} },`).join('\n')}
};
`);
if (main) console.log(`city.tim: ${textures.length} textures, ${counts.size} colours (${palette.size} kept)`);
