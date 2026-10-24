#!/usr/bin/env node
// Synthesises the game's sound effects and encodes them as PS1 SPU ADPCM:
// assets/sfx.bin (all of them, each a whole number of 16-byte blocks padded
// to 64 bytes) and src/sfx.h (ids, offsets, sizes). 22050 Hz mono.
//
//   node tools/makesfx.mjs
import { writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const RATE = 22050;

let seed = 99;
const noise = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 2147483648) - 1;
const len = (s) => Math.round(s * RATE);
const env = (t, a, d) => (t < a ? t / a : Math.exp(-(t - a) / d));   // attack, decay (s)

// One-pole low-pass / high-pass over a buffer.
const lowpass = (x, k) => { let y = 0; return x.map((v) => (y += (v - y) * k)); };
const highpass = (x, k) => { let y = 0, p = 0; return x.map((v) => { y = k * (y + v - p); p = v; return y; }); };
const mix = (...bufs) => { const n = Math.max(...bufs.map((b) => b.length)); return Array.from({ length: n }, (_, i) => bufs.reduce((s, b) => s + (b[i] || 0), 0)); };
const gen = (secs, f) => Array.from({ length: len(secs) }, (_, i) => f(i / RATE, i));
const sine = (secs, freq, a, d, amp = 1, sweep = 0) => { let ph = 0; return gen(secs, (t) => { ph += 2 * Math.PI * (freq + sweep * t) / RATE; return Math.sin(ph) * env(t, a, d) * amp; }); };
const noiseEnv = (secs, a, d, amp = 1) => gen(secs, (t) => noise() * env(t, a, d) * amp);
// A whoosh: noise through a sweeping low-pass, swelling then fading.
const whoosh = (secs, from, to, amp) => {
  let y = 0;
  return gen(secs, (t) => { const k = from + (to - from) * t / secs; y += (noise() - y) * k; return y * Math.sin(Math.PI * t / secs) * amp * 3; });
};
const ring = (secs, freqs, d, amp) => mix(...freqs.map((f, i) => sine(secs, f, 0.002, d / (1 + i * 0.4), amp / freqs.length)));

const SOUNDS = {
  SWING: () => whoosh(0.2, 0.05, 0.35, 0.7),
  DODGE: () => whoosh(0.16, 0.03, 0.12, 0.6),
  HIT: () => mix(sine(0.25, 110, 0.002, 0.06, 0.9, -200), lowpass(noiseEnv(0.2, 0.001, 0.04, 0.8), 0.4)),
  RIPOSTE: () => mix(sine(0.5, 80, 0.002, 0.12, 1, -60), lowpass(noiseEnv(0.3, 0.001, 0.07, 0.9), 0.35), ring(0.5, [520, 780], 0.15, 0.3)),
  PARRY: () => mix(ring(0.9, [1250, 1870, 2690, 3810], 0.25, 0.9), highpass(noiseEnv(0.05, 0.0005, 0.01, 0.6), 0.6)),
  BLOCK: () => mix(ring(0.3, [410, 640, 980], 0.06, 0.6), lowpass(noiseEnv(0.12, 0.001, 0.03, 0.5), 0.5)),
  HURT: () => mix(lowpass(noiseEnv(0.3, 0.002, 0.06, 0.8), 0.25), sine(0.3, 160, 0.004, 0.08, 0.6, -150)),
  WINDUP: () => mix(sine(0.5, 46, 0.25, 0.3, 0.7, 70), lowpass(noiseEnv(0.5, 0.3, 0.2, 0.25), 0.03)),
  SLAM: () => mix(sine(0.7, 62, 0.002, 0.18, 1, -40), lowpass(noiseEnv(0.6, 0.001, 0.15, 1), 0.06)),
  KILL: () => mix(lowpass(gen(0.6, (t) => (noise() > 0.84 ? noise() : 0) * env(t, 0.01, 0.2) * 2), 0.5), lowpass(noiseEnv(0.5, 0.01, 0.15, 0.5), 0.08)),
  DEATH: () => mix(sine(1.6, 98, 0.01, 0.6, 0.8, -30), sine(1.6, 147, 0.01, 0.5, 0.4, -45), lowpass(noiseEnv(1.2, 0.05, 0.4, 0.3), 0.03)),
  HEAL: () => mix(sine(0.7, 660, 0.05, 0.25, 0.5), sine(0.7, 990, 0.1, 0.25, 0.35), sine(0.7, 1320, 0.15, 0.2, 0.2)),
  RECOVER: () => mix(sine(0.25, 784, 0.005, 0.12, 0.5), [...Array(len(0.08)).fill(0), ...sine(0.3, 1046, 0.005, 0.12, 0.5)], [...Array(len(0.16)).fill(0), ...sine(0.5, 1568, 0.005, 0.2, 0.45)]),
  TURN: () => lowpass(noiseEnv(0.05, 0.002, 0.012, 0.25), 0.2),
};

// ---- PS1 ADPCM -------------------------------------------------------------------------
const F = [[0, 0], [60, 0], [115, -52], [98, -55], [122, -60]];
function encode(samples) {
  const out = [];
  let s1 = 0, s2 = 0;
  const pcm = samples.map((v) => Math.max(-32768, Math.min(32767, Math.round(v * 32000))));
  while (pcm.length % 28) pcm.push(0);
  pcm.push(...Array(28).fill(0));   // a silent block before the end
  for (let b = 0; b < pcm.length; b += 28) {
    const block = pcm.slice(b, b + 28);
    let best = null;
    for (let f = 0; f < 5; f++) for (let shift = 0; shift <= 12; shift++) {
      let o1 = s1, o2 = s2, err = 0;
      const nib = [];
      for (const x of block) {
        const pred = Math.floor((o1 * F[f][0] + o2 * F[f][1] + 32) / 64);
        let n = Math.round(((x - pred) * (1 << shift)) / 4096);
        n = Math.max(-8, Math.min(7, n));
        let y = ((n << 12) >> shift) + pred;   // what the SPU will decode
        y = Math.max(-32768, Math.min(32767, y));
        err += (y - x) ** 2;
        nib.push(n & 15);
        o2 = o1; o1 = y;
      }
      if (!best || err < best.err) best = { err, f, shift, nib, o1, o2 };
    }
    s1 = best.o1; s2 = best.o2;
    const bytes = [best.shift | (best.f << 4), 0];
    for (let i = 0; i < 28; i += 2) bytes.push(best.nib[i] | (best.nib[i + 1] << 4));
    out.push(bytes);
  }
  out[out.length - 1][1] = 1;   // end: the voice stops
  const buf = Buffer.from(out.flat());
  return Buffer.concat([buf, Buffer.alloc((64 - (buf.length % 64)) % 64)]);
}

// Each sound's loudest point (1 = full scale).
const PEAK = { TURN: 0.2, DODGE: 0.55, WINDUP: 0.6, SWING: 0.7, HEAL: 0.6, RECOVER: 0.6 };
const normalise = (x, peak) => { const m = Math.max(...x.map(Math.abs)) || 1; return x.map((v) => v * peak / m); };

const names = Object.keys(SOUNDS), blobs = [], offs = [];
let at = 0;
for (const n of names) {
  const b = encode(normalise(SOUNDS[n](), PEAK[n] ?? 0.85));
  offs.push([at, b.length]);
  blobs.push(b);
  at += b.length;
}
writeFileSync(path.join(root, 'assets', 'sfx.bin'), Buffer.concat(blobs));
writeFileSync(path.join(root, 'src', 'sfx.h'), `/* Generated by tools/makesfx.mjs: do not edit. */
#pragma once
#define SFX_RATE  ${RATE}
#define SFX_BYTES ${at}
enum {
${names.map((n) => `\tSFX_${n},`).join('\n')}
\tNUM_SFX
};
/* Offset and size of each in sfx.bin (SPU ADPCM). */
static const uint32_t sfx_table[NUM_SFX][2] = {
${offs.map(([o, l], i) => `\t{ ${o}, ${l} },   /* ${names[i]} */`).join('\n')}
};
`);
console.log(`sfx.bin: ${names.length} sounds, ${at} bytes`);
