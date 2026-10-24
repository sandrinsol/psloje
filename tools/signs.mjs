// Name signs for known buildings (schools, hospitals, hotels...): each a
// 64 x 16 picture (4 m x 1 m on the wall) with up to two lines of 16
// letters in a 3 x 5 pixel font, packed four to a 64 x 64 texture slot in a
// 4-bit texture page (16-colour CLUT). tools/osm2map.mjs stores the atlas in
// MAP.BIN's global section; the game loads it into the third texture page.

// 3 x 5 glyphs, rows top to bottom.
const FONT = {
  A: ['010', '101', '111', '101', '101'], B: ['110', '101', '110', '101', '110'], C: ['011', '100', '100', '100', '011'],
  D: ['110', '101', '101', '101', '110'], E: ['111', '100', '110', '100', '111'], F: ['111', '100', '110', '100', '100'],
  G: ['011', '100', '101', '101', '011'], H: ['101', '101', '111', '101', '101'], I: ['111', '010', '010', '010', '111'],
  J: ['001', '001', '001', '101', '010'], K: ['101', '101', '110', '101', '101'], L: ['100', '100', '100', '100', '111'],
  M: ['101', '111', '111', '101', '101'], N: ['110', '101', '101', '101', '101'], O: ['010', '101', '101', '101', '010'],
  P: ['110', '101', '110', '100', '100'], Q: ['010', '101', '101', '110', '011'], R: ['110', '101', '110', '101', '101'],
  S: ['011', '100', '010', '001', '110'], T: ['111', '010', '010', '010', '010'], U: ['101', '101', '101', '101', '111'],
  V: ['101', '101', '101', '101', '010'], W: ['101', '101', '111', '111', '101'], X: ['101', '101', '010', '101', '101'],
  Y: ['101', '101', '010', '010', '010'], Z: ['111', '001', '010', '100', '111'],
  0: ['111', '101', '101', '101', '111'], 1: ['010', '110', '010', '010', '111'], 2: ['110', '001', '010', '100', '111'],
  3: ['110', '001', '010', '001', '110'], 4: ['101', '101', '111', '001', '001'], 5: ['111', '100', '110', '001', '110'],
  6: ['011', '100', '111', '101', '111'], 7: ['111', '001', '010', '010', '010'], 8: ['111', '101', '111', '101', '111'],
  9: ['111', '101', '111', '001', '110'], '-': ['000', '000', '111', '000', '000'], '.': ['000', '000', '000', '000', '010'],
  ',': ['000', '000', '000', '010', '100'], '"': ['101', '101', '000', '000', '000'], "'": ['010', '010', '000', '000', '000'],
  '&': ['010', '101', '010', '101', '011'], '/': ['001', '001', '010', '100', '100'], '(': ['010', '100', '100', '100', '010'],
  ')': ['010', '001', '001', '001', '010'], ':': ['000', '010', '000', '010', '000'], '!': ['010', '010', '010', '000', '010'],
};
const COLS = 16;   // letters a line

// The 16-colour palette (index 0 unused: black 0x0000 is see-through).
export const SIGN_PALETTE = [
  [8, 8, 8], [250, 250, 245], [36, 70, 150], [130, 28, 34], [196, 34, 34], [96, 62, 36], [24, 24, 28], [226, 186, 90],
  [26, 96, 60], [240, 200, 40], [28, 40, 92], [150, 150, 150], [210, 210, 204], [60, 60, 66], [180, 120, 60], [255, 255, 255],
];
// Sign colours by kind: [background, text, frame].
export const SIGN_STYLE = {
  school: [2, 1, 12], university: [3, 1, 12], hospital: [1, 4, 4], worship: [5, 12, 14], hotel: [6, 7, 7], bank: [8, 1, 12],
  culture: [5, 1, 14], government: [3, 1, 7], police: [10, 1, 12], post: [9, 6, 6], shop: [6, 1, 11],
};

// Up to two lines of COLS letters: whole words where they fit, common long
// words shortened.
const SHORT = [[/9[- ]?VJECARE/g, '9-VJ.'], [/UNIVERSITETI/g, 'UNIV.'], [/FAKULTETI/g, 'FAK.'], [/SHKOLLA/g, 'SHK.'], [/GJIMNAZI/g, 'GJIMN.'], [/SPITALI/g, 'SPITALI'], [/QENDRA/g, 'QENDRA'], [/INSTITUCIONET/g, 'INST.']];
export function signLines(name) {
  let words = name.replace(/["“”„]/g, '"').replace(/\s+/g, ' ').trim().split(' ');
  const fit = (ws) => {
    const lines = [''];
    for (const w of ws) {
      const cur = lines[lines.length - 1];
      if (!cur) lines[lines.length - 1] = w;
      else if ((cur + ' ' + w).length <= COLS) lines[lines.length - 1] = cur + ' ' + w;
      else lines.push(w);
    }
    return lines;
  };
  let lines = fit(words);
  for (const [re, to] of SHORT) {
    if (lines.length <= 2 && lines.every((l) => l.length <= COLS)) break;
    words = words.join(' ').replace(re, to).split(' ');
    lines = fit(words);
  }
  return lines.slice(0, 2).map((l) => l.slice(0, COLS));
}

// One sign: 64 x 16 palette indices.
function renderSign(lines, [bg, fg, frame]) {
  const px = new Uint8Array(64 * 16).fill(bg);
  for (let x = 0; x < 64; x++) { px[x] = frame; px[15 * 64 + x] = frame; }
  for (let y = 0; y < 16; y++) { px[y * 64] = frame; px[y * 64 + 63] = frame; }
  const ys = lines.length === 1 ? [6] : [3, 9];
  lines.forEach((line, k) => {
    const x0 = Math.floor((64 - (line.length * 4 - 1)) / 2);
    [...line].forEach((ch, i) => {
      const g = FONT[ch];
      if (!g) return;
      for (let r = 0; r < 5; r++) for (let c = 0; c < 3; c++) if (g[r][c] === '1') px[(ys[k] + r) * 64 + x0 + i * 4 + c] = fg;
    });
  });
  return px;
}

// The atlas for a list of { lines, kind }: { rows, bytes } with bytes the
// 4-bit image (256 pixels = 128 bytes a row, low nibble first), sign i in
// slot i >> 2 (u = (slot & 3) * 64, v = (slot >> 2) * 64), band i & 3 (16 rows).
export function signAtlas(signs) {
  const slots = Math.ceil(signs.length / 4), rows = Math.ceil(slots / 4) * 64;
  const img = new Uint8Array(256 * rows);
  signs.forEach((s, i) => {
    const px = renderSign(s.lines, SIGN_STYLE[s.kind] || SIGN_STYLE.shop);
    const slot = i >> 2, u0 = (slot & 3) * 64, v0 = (slot >> 2) * 64 + (i & 3) * 16;
    for (let y = 0; y < 16; y++) for (let x = 0; x < 64; x++) img[(v0 + y) * 256 + u0 + x] = px[y * 64 + x];
  });
  const bytes = Buffer.alloc(128 * rows);
  for (let i = 0; i < img.length; i += 2) bytes[i >> 1] = img[i] | (img[i + 1] << 4);
  return { rows, bytes };
}

export const to15 = ([r, g, b]) => (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) || 0x0421;
