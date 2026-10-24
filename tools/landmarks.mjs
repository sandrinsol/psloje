// Hand-made models of Vlora's landmarks, modelled from reference photos
// (tools/fetch-refs.py; credits in data/landmark-refs.json). tools/osm2map.mjs
// puts each one where OpenStreetMap has it, in place of its plain building.
//
// Model space: metres, x to the right of the front, z away from the viewer
// standing in front (so the front faces -z), y up from the ground. Faces:
//   quad(tl, tr, bl, br) corners as seen from outside;
//   tri(a, b, c) clockwise as seen from outside.
// Face colours tint the texture (128 = as painted); the converter adds the
// sun's shading after turning the model into place.

import { TEXTURE_NAMES } from './maketex.mjs';

export const TEX = Object.fromEntries(TEXTURE_NAMES.map((n, i) => [n, i]));
TEX.none = 63;
const GREY = [128, 128, 128];

export class Model {
  constructor() { this.faces = []; }

  // A textured rectangle, split so no piece repeats its texture more than 4
  // times (the GPU's texture window); texW / texH: metres per texture repeat.
  // The texture's bottom sits at the bottom edge.
  quad(tl, tr, bl, br, o = {}) {
    const tex = o.tex ?? TEX.none, rgb = o.rgb || GREY;
    if (tex === TEX.none || o.whole) {
      this.faces.push({ p: [tl, tr, bl, br], tex, uv: o.uv || [0, 0, 63, 63], rgb });
      return;
    }
    const w = dist(tl, tr), h = dist(tl, bl), tw = o.texW || w, th = o.texH || h;
    const cols = Math.max(1, Math.ceil(w / tw / 4 - 1e-6)), rows = Math.max(1, Math.ceil(h / th / 4 - 1e-6));
    for (let c = 0; c < cols; c++) for (let r = 0; r < rows; r++) {
      const a = c / cols, b = (c + 1) / cols, top = 1 - r / rows, bot = 1 - (r + 1) / rows;
      // bilinear corners: s along tl->tr, t from bottom (0) to top (1)
      const at = (s, t) => lerp(lerp(bl, br, s), lerp(tl, tr, s), t);
      const uSpan = Math.max(1, Math.min(255, Math.round(w / cols / tw * 64) - 1));
      const vSpan = Math.max(1, Math.min(255, Math.round(h / rows / th * 64) - 1));
      this.faces.push({ p: [at(a, top), at(b, top), at(a, bot), at(b, bot)], tex, uv: [0, 255 - vSpan, uSpan, 255], rgb });
    }
  }

  tri(a, b, c, o = {}) {
    this.faces.push({ p: [a, b, c], tex: TEX.none, uv: [0, 0, 0, 0], rgb: o.rgb || GREY });
  }

  // A name sign (tools/signs.mjs: up to 2 lines of 16 letters) on a
  // rectangle; kind picks its colours.
  sign(tl, tr, bl, br, text, kind = 'culture') {
    this.faces.push({ p: [tl, tr, bl, br], tex: TEX.none, sign: { text, kind }, uv: [0, 0, 63, 15], rgb: GREY });
  }

  // Seen from both sides.
  tri2(a, b, c, o = {}) {
    this.tri(a, b, c, o);
    this.tri(a, c, b, o);
  }

  // Walls of a prism over a counter-clockwise (seen from above) outline,
  // from y0 to y1, the top outline scaled by `taper` towards its centre;
  // `top` adds a flat top (fan of triangles), `topRgb` its colour.
  prism(poly, y0, y1, o = {}) {
    const taper = o.taper ?? 1;
    const cx = poly.reduce((s, p) => s + p[0], 0) / poly.length, cz = poly.reduce((s, p) => s + p[1], 0) / poly.length;
    const topP = poly.map(([x, z]) => [cx + (x - cx) * taper, cz + (z - cz) * taper]);
    for (let i = 0; i < poly.length; i++) {
      const j = (i + 1) % poly.length;
      // Walking a counter-clockwise outline the outside is on the right; seen
      // from outside, vertex i is on the left: TL = i top, TR = j top.
      this.quad([topP[i][0], y1, topP[i][1]], [topP[j][0], y1, topP[j][1]], [poly[i][0], y0, poly[i][1]], [poly[j][0], y0, poly[j][1]], o);
    }
    if (o.top) for (let i = 1; i + 1 < topP.length; i++)
      this.tri([topP[0][0], y1, topP[0][1]], [topP[i + 1][0], y1, topP[i + 1][1]], [topP[i][0], y1, topP[i][1]], { rgb: o.topRgb || o.rgb });
  }

  box(x0, x1, y0, y1, z0, z1, o = {}) {
    this.prism([[x0, z0], [x1, z0], [x1, z1], [x0, z1]], y0, y1, { top: true, ...o });
  }

  // n-sided column around (cx, cz).
  column(cx, cz, r0, r1, y0, y1, n, o = {}) {
    const poly = ring(cx, cz, r0, n);
    this.prism(poly, y0, y1, { taper: r1 / r0, ...o });
  }

  cone(cx, cz, r, y0, h, n, o = {}) {
    const poly = ring(cx, cz, r, n), apex = [cx, y0 + h, cz];
    for (let i = 0; i < n; i++) {
      const j = (i + 1) % n;
      this.tri(apex, [poly[j][0], y0, poly[j][1]], [poly[i][0], y0, poly[i][1]], o);
    }
  }

  // Dome: `rings` bands of quads and a cap of triangles.
  dome(cx, cz, r, y0, h, n, rings, o = {}) {
    const pt = (k, i) => {
      const a = (k / rings) * Math.PI / 2, rr = r * Math.cos(a), y = y0 + h * Math.sin(a);
      const t = (i / n) * Math.PI * 2;
      return [cx + rr * Math.cos(t), y, cz + rr * Math.sin(t)];
    };
    for (let k = 0; k < rings - 1; k++) for (let i = 0; i < n; i++)
      this.quad(pt(k + 1, i), pt(k + 1, i + 1), pt(k, i), pt(k, i + 1), { ...o, whole: true });
    const apex = [cx, y0 + h, cz];
    for (let i = 0; i < n; i++) this.tri(apex, pt(rings - 1, i + 1), pt(rings - 1, i), o);
  }

  // A standing figure (statue) about h tall at (x, z), facing `face`
  // radians (0 = towards the viewer, -z); arm: 'up' (raised), 'gun' (rifle
  // on the shoulder) or none.
  figure(x, z, y0, h, face = 0, o = {}) {
    const m = new Model(), s = h / 1.8;
    const B = { tex: o.tex ?? TEX.bronze, texW: 1, texH: 1, rgb: o.rgb || GREY };
    m.box(-0.2 * s, 0.2 * s, 0, 0.85 * s, -0.12 * s, 0.12 * s, { ...B, top: false });   // legs (top hidden)
    m.prism([[-0.27 * s, -0.15 * s], [0.27 * s, -0.15 * s], [0.27 * s, 0.15 * s], [-0.27 * s, 0.15 * s]], 0.85 * s, 1.5 * s, { ...B, top: true, taper: 0.9 });   // coat
    m.box(-0.11 * s, 0.11 * s, 1.5 * s, 1.78 * s, -0.11 * s, 0.11 * s, B);         // head
    if (o.arm === 'up') m.box(0.27 * s, 0.4 * s, 1.2 * s, 2.15 * s, -0.07 * s, 0.07 * s, B);
    if (o.arm === 'gun') m.box(-0.36 * s, -0.3 * s, 0.9 * s, 2.0 * s, -0.04 * s, 0.04 * s, B);
    this.add(m, x, y0, z, face);
  }

  // Another model, turned by `angle` (radians, clockwise seen from above) and moved.
  add(m, x, y, z, angle = 0) {
    const c = Math.cos(angle), s = Math.sin(angle);
    const tf = ([px, py, pz]) => [x + px * c + pz * s, y + py, z - px * s + pz * c];
    for (const f of m.faces) this.faces.push({ ...f, p: f.p.map(tf) });
  }
}

const lerp = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
// Counter-clockwise seen from above (x right, z up the page).
const ring = (cx, cz, r, n) => Array.from({ length: n }, (_, i) => [cx + r * Math.cos((i / n) * Math.PI * 2), cz + r * Math.sin((i / n) * Math.PI * 2)]);

// ---- The landmarks ----------------------------------------------------------------
//
// match: OSM tags to find it (name) and the kind of element; place:
//   'point'     at the node (or the outline's centre), facing `facing`
//               (degrees clockwise from north = the side the front looks to)
//   'footprint' fitted to the building outline (its long side across the
//               front), the front towards the nearest street
//   frontRoad   the street (any highway, by name) the front faces, instead
//               of the nearest drivable one; frontSide 'short' puts the front
//               on a short side of the outline
// build(size) gets { w, d } (footprint: front width and depth) and returns { model, far, collide }:
// collide is a local outline the taxi can't drive into.

export const LANDMARKS = [
  {
    key: 'independence-monument',
    name: 'Monumenti i Pavarësisë',
    refs: 'Category:Independence Monument Vlorë',
    place: 'point', facing: 200,
    build() {
      const m = new Model();
      const STONE = { tex: TEX.paving, texW: 4, texH: 4 };
      // Three wide steps, then the dark stone base.
      [[8, 0], [6.8, 0.4], [5.6, 0.8]].forEach(([r, y]) => m.box(-r, r, y, y + 0.4, -r * 0.8, r * 0.8, STONE));
      m.box(-3.6, 3.6, 1.2, 3.0, -2.6, 2.6, { tex: TEX.monstone, texW: 4, texH: 4, rgb: [92, 92, 92] });
      // The pillar: rough stone, slightly tapering, "1912" near the top of its front.
      m.prism([[-1.3, -1.1], [1.3, -1.1], [1.2, 1.1], [-1.2, 1.1]], 3.0, 13.5, { tex: TEX.monstone, texW: 2.6, texH: 2.2, taper: 0.72, top: true, topRgb: [110, 108, 102] });
      // Bronze figures on the base: the group of 1912, larger than life.
      const Y = 3.0;
      m.figure(-2.7, -1.6, Y, 3.0, 0.3, { arm: 'gun' });
      m.figure(-1.6, -2.0, Y, 3.1, 0.1);
      m.figure(-0.4, -2.1, Y, 3.2, 0, { arm: 'up' });
      m.figure(0.9, -2.0, Y, 3.1, -0.1);
      m.figure(2.2, -1.7, Y, 3.0, -0.3, { arm: 'gun' });
      m.figure(-2.9, 0.6, Y, 2.9, 1.3);
      m.figure(2.9, 0.4, Y, 2.9, -1.3, { arm: 'gun' });
      m.figure(0.2, 1.8, Y, 2.8, 3.1);
      // On top: the flag bearer with the flag blowing back.
      m.figure(0, -0.1, 13.5, 3.0, 0, { arm: 'up' });
      const fx = 0.7, fy = 16.6;
      const flag = (a, b, c, d) => { m.quad(a, b, c, d, { tex: TEX.bronze, whole: true }); m.quad(b, a, d, c, { tex: TEX.bronze, whole: true }); };
      flag([fx, fy, -0.1], [fx + 1.6, fy + 0.3, 0.6], [fx, fy - 1.5, -0.1], [fx + 1.6, fy - 1.1, 0.6]);
      flag([fx + 1.6, fy + 0.3, 0.6], [fx + 3.0, fy - 0.2, 0.2], [fx + 1.6, fy - 1.1, 0.6], [fx + 3.0, fy - 1.4, 0.2]);
      // Red banners on tall poles in an arc behind it, as around the square.
      for (let i = 0; i < 7; i++) {
        const a = Math.PI * (0.15 + 0.7 * i / 6), px = Math.cos(a) * 15, pz = Math.sin(a) * 12 + 2;
        m.column(px, pz, 0.12, 0.1, 0, 9, 4, { rgb: [70, 70, 74] });
        const b = (p, q) => { m.quad([p[0], 8.6, p[1]], [q[0], 8.6, q[1]], [p[0], 5.2, p[1]], [q[0], 5.2, q[1]], { rgb: [200, 26, 30] }); m.quad([q[0], 8.6, q[1]], [p[0], 8.6, p[1]], [q[0], 5.2, q[1]], [p[0], 5.2, p[1]], { rgb: [200, 26, 30] }); };
        b([px + 0.12, pz], [px + 1.3, pz - 0.2]);
      }
      // Far away: base, pillar and a block for the figures.
      const far = new Model();
      far.box(-8, 8, 0, 1.2, -6.4, 6.4, STONE);
      far.box(-3.6, 3.6, 1.2, 6.0, -2.6, 2.6, { tex: TEX.bronze, texW: 4, texH: 4 });
      far.prism([[-1.3, -1.1], [1.3, -1.1], [1.2, 1.1], [-1.2, 1.1]], 6.0, 16.5, { tex: TEX.monstone, taper: 0.72, top: true });
      return { model: m, far, collide: [[-8, -6.4], [8, -6.4], [8, 6.4], [-8, 6.4]] };
    },
  },
  {
    key: 'muradie-mosque',
    name: 'Xhamia e Muradis',
    refs: 'Category:Muradie Mosque',
    place: 'footprint',
    build({ w, d }) {
      const m = new Model();
      // The prayer hall: a near-square block of banded stone.
      const s = Math.min(Math.max(Math.min(w, d), 9), 16), hw = s / 2, H = 9;
      // Two rows of windows, two a side (a big one low, a smaller one above),
      // like the photos: the texture stretched to half a side.
      const WIN = { tex: TEX.mosquewin, texW: s / 2, texH: 5 };
      m.prism([[-hw, -hw], [hw, -hw], [hw, hw], [-hw, hw]], 0, 5, WIN);
      m.prism([[-hw, -hw], [hw, -hw], [hw, hw], [-hw, hw]], 5, H, { ...WIN, texH: 4 });
      // Octagonal drum stepping in to the dome, the drum's roof in lead.
      const r8 = hw / Math.cos(Math.PI / 8);
      m.column(0, 0, r8 * 0.92, r8 * 0.92, H, H + 2.2, 8, { tex: TEX.mosquewall, texW: 4, texH: 2.2 });
      m.column(0, 0, r8 * 0.92, r8 * 0.78, H + 2.2, H + 2.6, 8, { tex: TEX.lead, texW: 2, texH: 1 });
      // Corner roofs between the square and the drum.
      for (const [sx, sz] of [[-1, -1], [1, -1], [1, 1], [-1, 1]])
        m.tri2([sx * hw * 0.6, H + 1.4, sz * hw], [sx * hw, H, sz * hw], [sx * hw, H + 1.4, sz * hw * 0.6], { rgb: [128, 132, 138] });
      m.dome(0, 0, hw * 0.88, H + 2.6, hw * 0.62, 8, 3, { tex: TEX.lead, rgb: [124, 128, 134] });
      m.cone(0, 0, 0.25, H + 2.6 + hw * 0.62, 1.2, 4, { rgb: [200, 180, 90] });   // finial
      // The minaret at the front right corner, a little apart.
      const mx = hw + 1.5, mz = -hw + 1.5, MS = { tex: TEX.minaret, texW: 2, texH: 4 };
      m.column(mx, mz, 1.35, 1.25, 0, 3, 8, { ...MS, texH: 3 });
      m.column(mx, mz, 1.1, 1.0, 3, 19, 8, MS);
      m.column(mx, mz, 1.7, 1.7, 19, 19.7, 8, { tex: TEX.minaret, texW: 2, texH: 0.7, top: true, rgb: [150, 140, 120] });   // balcony
      m.column(mx, mz, 0.9, 0.85, 19.7, 23, 8, MS);
      m.cone(mx, mz, 1.0, 23, 4.2, 8, { rgb: [120, 124, 130] });
      const far = new Model();
      far.prism([[-hw, -hw], [hw, -hw], [hw, hw], [-hw, hw]], 0, H, { tex: TEX.mosquewall, top: true });
      far.dome(0, 0, hw * 0.88, H, hw * 0.62 + 2.6, 6, 2, { rgb: [124, 128, 134] });
      far.column(mx, mz, 1.1, 0.9, 0, 23, 6, { tex: TEX.minaret });
      far.cone(mx, mz, 1.0, 23, 4.2, 6, { rgb: [120, 124, 130] });
      return { model: m, far, collide: [[-hw, -hw], [hw + 2.8, -hw], [hw + 2.8, hw], [-hw, hw]] };
    },
  },
  {
    key: 'independence-museum',
    name: 'Muzeu Kombëtar i Pavarësisë',
    refs: 'Category:Independence Museum Vlorë',
    place: 'footprint',
    build({ w, d }) {
      const m = new Model();
      const hw = Math.max(w, 9) / 2, hd = Math.max(d, 7) / 2, H = 7.6, FLOOR = 3.8;
      const W = { tex: TEX.museumwin, texW: 3, texH: FLOOR };
      // Back and sides: windows all round, two floors.
      m.quad([hw, H, hd], [-hw, H, hd], [hw, 0, hd], [-hw, 0, hd], W);
      m.quad([-hw, H, hd], [-hw, H, -hd], [-hw, 0, hd], [-hw, 0, -hd], W);
      m.quad([hw, H, -hd], [hw, H, hd], [hw, 0, -hd], [hw, 0, hd], W);
      // Front: windows either side, the door in the middle, a window above it.
      const dw = 1.5;
      m.quad([-hw, H, -hd], [-dw, H, -hd], [-hw, 0, -hd], [-dw, 0, -hd], W);
      m.quad([dw, H, -hd], [hw, H, -hd], [dw, 0, -hd], [hw, 0, -hd], W);
      m.quad([-dw, FLOOR, -hd], [dw, FLOOR, -hd], [-dw, 0, -hd], [dw, 0, -hd], { tex: TEX.museumdoor, texW: 3, texH: FLOOR });
      m.quad([-dw, H, -hd], [dw, H, -hd], [-dw, FLOOR, -hd], [dw, FLOOR, -hd], W);
      // Balcony on two columns over the door.
      m.box(-2.0, 2.0, FLOOR - 0.15, FLOOR + 0.1, -hd - 1.4, -hd, { tex: TEX.paving, texW: 2, texH: 1, rgb: [150, 150, 150] });
      m.quad([-2.0, FLOOR + 1.1, -hd - 1.4], [2.0, FLOOR + 1.1, -hd - 1.4], [-2.0, FLOOR + 0.1, -hd - 1.4], [2.0, FLOOR + 0.1, -hd - 1.4], { tex: TEX.railing, texW: 4, texH: 1 });
      m.box(-1.9, -1.6, 0, FLOOR - 0.15, -hd - 1.35, -hd - 1.05, { rgb: [230, 226, 214] });
      m.box(1.6, 1.9, 0, FLOOR - 0.15, -hd - 1.35, -hd - 1.05, { rgb: [230, 226, 214] });
      // Hipped tile roof.
      const R = H + 2.6, ov = 0.4, a = [-hw - ov, H, -hd - ov], b = [hw + ov, H, -hd - ov], c = [hw + ov, H, hd + ov], e = [-hw - ov, H, hd + ov];
      const r0 = [-hw + hd, R, 0], r1 = [hw - hd, R, 0];
      const T = { tex: TEX.tiles, texW: 2, texH: 2 };
      m.quad(r0, r1, a, b, T);                // front slope
      m.quad(r1, r0, c, e, T);                // back
      m.tri(r0, a, e, { rgb: [176, 84, 58] });   // ends
      m.tri(r1, c, b, { rgb: [176, 84, 58] });
      const far = new Model();
      far.prism([[-hw, -hd], [hw, -hd], [hw, hd], [-hw, hd]], 0, H, { tex: TEX.museumwin, texW: 3, texH: FLOOR, top: true, topRgb: [176, 84, 58] });
      return { model: m, far, collide: [[-hw, -hd - 1.4], [hw, -hd - 1.4], [hw, hd], [-hw, hd]] };
    },
  },
  {
    key: 'teatri-petro-marko',
    name: 'Teatri Petro Marko',
    refs: 'Teatri Petro Marko Vlore.jpg; Vlorë, Albania 31 December 2022 - Teatri Petro Marko.jpg',
    place: 'footprint', frontRoad: 'Bulevardi Ismail Qemali', frontSide: 'short',
    build({ w, d }) {
      const m = new Model();
      const hw = Math.max(w, 16) / 2, hd = Math.max(d, 16) / 2, H = 10;
      const bays = Math.max(3, Math.round((2 * hw) / 4.4)), bw = (2 * hw) / bays;
      const WALL = { tex: TEX.theatrewall, texW: bw, texH: 8, rgb: [128, 128, 128] };
      // Front: bays of pilasters and crimson panels, the doors in the middle three.
      for (let i = 0; i < bays; i++) {
        const x0 = -hw + i * bw, x1 = x0 + bw, door = Math.abs(i - (bays - 1) / 2) <= 1;
        m.quad([x0, 8, -hd], [x1, 8, -hd], [x0, 0, -hd], [x1, 0, -hd], { ...WALL, tex: door ? TEX.theatredoor : TEX.theatrewall });
      }
      // Sides and back.
      m.quad([-hw, 8, hd], [-hw, 8, -hd], [-hw, 0, hd], [-hw, 0, -hd], { ...WALL, texW: 4.4 });
      m.quad([hw, 8, -hd], [hw, 8, hd], [hw, 0, -hd], [hw, 0, hd], { ...WALL, texW: 4.4 });
      m.quad([hw, 8, hd], [-hw, 8, hd], [hw, 0, hd], [-hw, 0, hd], { ...WALL, texW: 4.4 });
      // The white frieze all round, its name on the front, the cornice.
      const WHITE = { tex: TEX.paving, texW: 4, texH: 2, rgb: [150, 150, 150] };
      m.prism([[-hw, -hd], [hw, -hd], [hw, hd], [-hw, hd]], 8, H, WHITE);
      m.sign([-6, 9.6, -hd - 0.05], [6, 9.6, -hd - 0.05], [-6, 8.4, -hd - 0.05], [6, 8.4, -hd - 0.05], 'TEATRI PETRO MARKO', 'culture');
      m.prism([[-hw - 0.4, -hd - 0.4], [hw + 0.4, -hd - 0.4], [hw + 0.4, hd + 0.4], [-hw - 0.4, hd + 0.4]], H, H + 0.4, { ...WHITE, top: true, topRgb: [120, 122, 126] });
      // The low pediment over the front.
      m.tri2([0, H + 2.4, -hd - 0.4], [hw + 0.4, H + 0.4, -hd - 0.4], [-hw - 0.4, H + 0.4, -hd - 0.4], { rgb: [236, 232, 222] });
      m.quad([0, H + 2.4, -hd - 0.4], [0, H + 2.4, hd * 0.2], [-hw - 0.4, H + 0.4, -hd - 0.4], [-hw - 0.4, H + 0.4, hd * 0.2], { rgb: [120, 122, 126] });
      m.quad([0, H + 2.4, hd * 0.2], [0, H + 2.4, -hd - 0.4], [hw + 0.4, H + 0.4, hd * 0.2], [hw + 0.4, H + 0.4, -hd - 0.4], { rgb: [120, 122, 126] });
      m.tri([0, H + 2.4, hd * 0.2], [-hw - 0.4, H + 0.4, hd * 0.2], [hw + 0.4, H + 0.4, hd * 0.2], { rgb: [120, 122, 126] });
      // Front steps.
      m.box(-hw + bw, hw - bw, 0, 0.35, -hd - 1.6, -hd, { tex: TEX.paving, texW: 2, texH: 2, rgb: [150, 150, 150] });
      const far = new Model();
      far.prism([[-hw, -hd], [hw, -hd], [hw, hd], [-hw, hd]], 0, H, { tex: TEX.theatrewall, texW: 4.4, texH: 8, top: true, topRgb: [120, 122, 126] });
      return { model: m, far, collide: [[-hw, -hd], [hw, -hd], [hw, hd], [-hw, hd]] };
    },
  },
];
