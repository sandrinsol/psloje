#!/usr/bin/env node
// The game's music and voices, synthesised: instruments (looped pads and
// drones, one-shot bells, drums, whispers, a sigh, a choir swell) encoded as
// SPU ADPCM in assets/music.bin, and the songs as note lists in
// src/music_data.h for the sequencer (src/music.c).
//
//   node tools/makemusic.mjs [preview dir]
//
// With a directory, also renders each song to a WAV there (a rough mix,
// without the SPU's reverb) to listen to without the console.
import { writeFileSync, mkdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { encode } from './adpcm.mjs';

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const RATE = 22050;
const TICKS = 60;   // sequencer ticks a second (one a frame)

let seed = 7;
const noise = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 2147483648) - 1;
const len = (s) => Math.round(s * RATE);
const midiHz = (n) => 440 * 2 ** ((n - 69) / 12);

// ---- Instruments -------------------------------------------------------------------------

// A looping tone: partials at whole cycles of the loop so it repeats without a
// seam. partials: [freq, amp, phase?].
function loopTone(samples, partials) {
  const out = new Float64Array(samples);
  for (const [f, a] of partials) {
    const cycles = Math.max(1, Math.round(f * samples / RATE)), ph = noise() * Math.PI;
    for (let i = 0; i < samples; i++) out[i] += a * Math.sin(2 * Math.PI * cycles * i / samples + ph);
  }
  return out;
}
// Vowel formants ("ah"): a gain for a frequency.
const formant = (f, peaks) => peaks.reduce((g, [fc, bw, a]) => g + a / (1 + ((f - fc) / bw) ** 2), 0.02);
const AH = [[700, 130, 1], [1220, 120, 0.5], [2600, 200, 0.25]];
const OO = [[400, 100, 1], [800, 120, 0.35], [2400, 200, 0.12]];

// Choir "ah" around A3: three voices a little out of tune with each other.
function choir() {
  const L = 28 * 788, f0 = 220, parts = [];
  for (const det of [1, 1.0045, 0.9957])
    for (let k = 1; k * f0 < 5000; k++) parts.push([k * f0 * det, formant(k * f0, AH) / Math.sqrt(k)]);
  return loopTone(L, parts);
}
// A dark drone around D2: odd-heavy, two voices beating slowly.
function drone() {
  const L = 28 * 788, f0 = midiHz(38), parts = [];
  for (const det of [1, 1.003])
    for (let k = 1; k < 24; k++) parts.push([k * f0 * det, (k % 2 ? 1 : 0.45) / k ** 1.3 * (k > 8 ? 0.4 : 1)]);
  return loopTone(L, parts);
}
// A church organ around A3: 8', 4', 2 2/3', 2' and a 16' below.
function organ() {
  const L = 28 * 394, f0 = 220, parts = [];
  for (const [m, a] of [[0.5, 0.5], [1, 1], [2, 0.6], [3, 0.35], [4, 0.3], [6, 0.12], [8, 0.08]]) parts.push([f0 * m, a]);
  return loopTone(L, parts);
}
// A bell around A4: inharmonic partials, the high ones dying first.
function bell() {
  const n = len(3.2), out = new Float64Array(n), f0 = 440;
  const P = [[0.5, 0.6, 2.4], [1, 1, 2.0], [1.19, 0.5, 1.3], [1.5, 0.45, 1.1], [2, 0.35, 0.8], [2.74, 0.25, 0.5], [3.76, 0.15, 0.35], [5.4, 0.08, 0.2]];
  for (const [m, a, d] of P) for (let i = 0; i < n; i++) { const t = i / RATE; out[i] += a * Math.sin(2 * Math.PI * f0 * m * t) * Math.exp(-t / d) * Math.min(1, t / 0.002); }
  return out;
}
// A heartbeat thump.
function pulse() {
  const n = len(0.45), out = new Float64Array(n);
  let ph = 0;
  for (let i = 0; i < n; i++) { const t = i / RATE; ph += 2 * Math.PI * (58 - 30 * t) / RATE; out[i] = Math.sin(ph) * Math.exp(-t / 0.09) * Math.min(1, t / 0.004); }
  return out;
}
// A big drum (taiko-like) around A1: a pitch drop and a skin noise.
function drum() {
  const n = len(0.9), out = new Float64Array(n);
  let ph = 0, y = 0;
  for (let i = 0; i < n; i++) {
    const t = i / RATE;
    ph += 2 * Math.PI * (55 * (1 + 0.6 * Math.exp(-t / 0.03))) / RATE;
    y += (noise() - y) * 0.08;
    out[i] = (Math.sin(ph) * Math.exp(-t / 0.22) + y * 1.5 * Math.exp(-t / 0.05)) * Math.min(1, t / 0.002);
  }
  return out;
}
// Breath through a vowel: noise through formant resonators.
function breath(secs, vowelAt, ampAt) {
  const n = len(secs), out = new Float64Array(n);
  const st = [];
  for (let i = 0; i < n; i++) {
    const t = i / secs / RATE, v = vowelAt(t), x = noise();
    let y = 0;
    v.forEach(([fc, bw, a], j) => {
      // a two-pole resonator per formant
      const r = Math.exp(-Math.PI * bw / RATE), th = 2 * Math.PI * fc / RATE;
      const s = st[j] || (st[j] = [0, 0]);
      const o = x * (1 - r) * a + 2 * r * Math.cos(th) * s[0] - r * r * s[1];
      s[1] = s[0]; s[0] = o; y += o;
    });
    out[i] = y * ampAt(t);
  }
  return out;
}
const lerpV = (a, b, k) => a.map(([f, bw, g], i) => [f + (b[i][0] - f) * k, bw + (b[i][1] - bw) * k, g + (b[i][2] - g) * k]);
const SS = [[2600, 600, 0.6], [4200, 900, 1], [6000, 1200, 0.6]];
// A whisper: hissed syllables.
const whisper = () => breath(1.3, (t) => lerpV(SS, AH, (Math.sin(t * 19) + 1) / 2 * 0.6),
  (t) => Math.sin(Math.PI * t) ** 0.7 * (0.55 + 0.45 * Math.sin(t * 2 * Math.PI * 5.5)) ** 2);
// A sigh: breath falling from "ah" to "oo".
const sigh = () => breath(1.6, (t) => lerpV(AH, OO, t), (t) => Math.min(1, t * 5) * Math.exp(-t * 2.2) * (1 - t));
// A choir swelling up on a minor chord and dying away.
function swell() {
  const n = len(2.2), out = new Float64Array(n);
  const chord = [57, 60, 64, 69].map(midiHz);
  for (const f0 of chord)
    for (const det of [1, 1.004])
      for (let k = 1; k * f0 < 4500; k++) {
        const a = formant(k * f0, AH) / Math.sqrt(k), f = k * f0 * det, ph = noise() * 3;
        for (let i = 0; i < n; i++) out[i] += a * Math.sin(2 * Math.PI * f * i / RATE + ph);
      }
  for (let i = 0; i < n; i++) { const t = i / n; out[i] *= Math.min(1, t * 2.2) ** 2 * (1 - t) ** 1.5; }
  return out;
}

// Wind and thin rain over the floor, a seamless loop: low rushing noise in
// slow gusts (whole cycles of the loop), a fine hiss, now and then a drop.
function wind() {
  const L = 28 * 3150, fade = 4000, n = L + fade, raw = new Float64Array(n);
  let lo = 0, lo2 = 0, hi = 0;
  for (let i = 0; i < n; i++) {
    const x = noise();
    lo += (x - lo) * 0.02; lo2 += (lo - lo2) * 0.05;   // the wind: twice low-passed
    hi += (x - hi) * 0.5;                               // the rain: what's left above
    const t = (i % L) / L, gust = 0.55 + 0.3 * Math.sin(2 * Math.PI * 2 * t) + 0.15 * Math.sin(2 * Math.PI * 5 * t + 1);
    raw[i] = lo2 * 9 * gust + (x - hi) * 0.18 + (noise() > 0.9993 ? noise() * 0.6 : 0);
  }
  const out = new Float64Array(L);   // the end faded into the start: no seam
  for (let i = 0; i < L; i++) out[i] = i < fade ? raw[i] * (i / fade) + raw[L + i] * (1 - i / fade) : raw[i];
  return out;
}

// name: [samples, loop, base MIDI note, ADSR1, ADSR2]. ADSR (SPU): attack
// rate << 8 | sustain level 15; release shift | exponential.
const ADSR1 = (ar) => (ar << 8) | 0x000f;
const ADSR2 = (rr) => 0x0020 | rr;
const INSTRUMENTS = {
  CHOIR: [choir, true, 57, ADSR1(56), ADSR2(15)],
  DRONE: [drone, true, 38, ADSR1(60), ADSR2(16)],
  ORGAN: [organ, true, 57, ADSR1(36), ADSR2(12)],
  BELL: [bell, false, 69, ADSR1(0), ADSR2(18)],
  PULSE: [pulse, false, 38, ADSR1(0), ADSR2(10)],
  DRUM: [drum, false, 33, ADSR1(0), ADSR2(12)],
  WHISPER: [whisper, false, 60, ADSR1(0), ADSR2(14)],
  SIGH: [sigh, false, 60, ADSR1(0), ADSR2(14)],
  SWELL: [swell, false, 57, ADSR1(0), ADSR2(16)],
  WIND: [wind, true, 60, ADSR1(64), ADSR2(18)],
};
const PEAK = { WHISPER: 0.7, SIGH: 0.75, PULSE: 0.9, DRUM: 0.95, WIND: 0.6 };
const normalise = (x, peak) => { let m = 0; for (const v of x) m = Math.max(m, Math.abs(v)); return Array.from(x, (v) => v * peak / (m || 1)); };

// ---- Songs ---------------------------------------------------------------------------------
// A song: a length in seconds and tracks; a track plays one instrument on its
// own SPU voice: [instrument, volume 0..127, pan -64..64, notes], a note
// [start s, length s, MIDI note, velocity 0..127].
const N = { C: 0, 'C#': 1, D: 2, Eb: 3, E: 4, F: 5, 'F#': 6, G: 7, Ab: 8, A: 9, Bb: 10, B: 11 };
const n = (s) => { const m = s.match(/^([A-G][b#]?)(\d)$/); return 12 * (+m[2] + 1) + N[m[1]]; };
// Chords: a list of [chord notes, seconds], one track per chord voice.
function chordTracks(inst, vol, chords, start = 0, pans = [-40, 0, 40]) {
  const voices = Math.max(...chords.map(([c]) => c.length));
  return Array.from({ length: voices }, (_, v) => {
    let t = start;
    const notes = [];
    for (const [c, secs] of chords) { if (c[v]) notes.push([t, secs - 0.05, n(c[v]), 100]); t += secs; }
    return [inst, vol, pans[v % pans.length], notes];
  });
}
const every = (from, to, step, f) => { const out = []; for (let t = from; t < to - 1e-6; t += step) out.push(...f(t)); return out; };

const SONGS = {
  // The title: the choir alone over a drone, a slow descending line, bells
  // at the turns. D minor: i - VI - iv - V.
  TITLE: { secs: 32, tracks: [
    ['DRONE', 70, 0, [[0, 32, n('D2'), 110]]],
    ...chordTracks('CHOIR', 52, [[['D3', 'F3', 'A3'], 8], [['D3', 'F3', 'Bb3'], 8], [['D3', 'G3', 'Bb3'], 8], [['C#3', 'E3', 'A3'], 8]]),
    ['CHOIR', 60, 10, [[0, 4, n('A4'), 100], [4, 4, n('G4'), 95], [8, 4, n('F4'), 100], [12, 4, n('D4'), 95], [16, 4, n('Eb4'), 100], [20, 4, n('D4'), 95], [24, 8, n('C#4'), 100]]],
    ['BELL', 46, -20, [[0, 3, n('D5'), 100], [8, 3, n('F5'), 90], [16, 3, n('G4'), 95], [24, 3, n('E5'), 90], [28, 3, n('A4'), 80]]],
  ] },
  // Sloth: a drone and a slow heart, the choir far off, bells, whispers.
  SLOTH: { secs: 48, tracks: [
    ['DRONE', 66, 0, [[0, 48, n('D2'), 110]]],
    ['DRONE', 40, 0, [[16, 16, n('A2'), 90], [40, 8, n('A2'), 80]]],
    ['PULSE', 72, 0, every(0, 48, 1.6, (t) => [[t, 0.3, n('D2'), 110], [t + 0.26, 0.3, n('D2'), 70]])],
    ...chordTracks('CHOIR', 38, [[['D3', 'F3', 'A3'], 8], [['D3', 'F3', 'Bb3'], 8], [['D3', 'G3', 'Bb3'], 8], [['C#3', 'E3', 'A3'], 8], [['D3', 'F3', 'A3'], 8], [[], 8]]),
    ['BELL', 34, 30, [[12, 3, n('A4'), 100], [28, 3, n('D5'), 90], [44, 3, n('F4'), 85]]],
    ['WHISPER', 44, -50, [[6, 1.3, n('C4'), 100], [37, 1.3, n('D4'), 90]]],
    ['WHISPER', 40, 50, [[21, 1.3, n('B3'), 100], [45, 1.3, n('C4'), 90]]],
  ] },
  // The boss: organ, drums, choir stabs, 90 beats a minute. Bars of 4 beats.
  BOSS: (() => {
    const beat = 60 / 90, bar = beat * 4, secs = bar * 8;
    const chords = [[['D3', 'F3', 'A3'], 2 * bar], [['D3', 'F3', 'Bb3'], 2 * bar], [['C3', 'E3', 'G3'], 2 * bar], [['C#3', 'E3', 'A3'], 2 * bar]];
    const roots = ['D2', 'Bb1', 'C2', 'A1'];
    return { secs, tracks: [
      ...chordTracks('ORGAN', 50, chords),
      ['ORGAN', 44, 0, roots.map((r, i) => [i * 2 * bar, 2 * bar - 0.05, n(r), 110])],
      ['DRUM', 100, -10, every(0, secs, bar, (t) => [[t, 0.5, n('A1'), 120], [t + beat * 2, 0.5, n('A1'), 105], [t + beat * 3.5, 0.3, n('A1'), 90]])],
      ['DRUM', 70, 20, every(0, secs, bar, (t) => [[t + beat * 1.5, 0.3, n('E2'), 100], [t + beat * 3, 0.3, n('E2'), 110]])],
      ['PULSE', 70, 0, every(0, secs, beat, (t) => [[t, 0.2, n('D2'), 100]])],
      ...chordTracks('CHOIR', 50, every(0, 4, 1, (i) => [[chords[i][0].map((x) => x.replace('3', '4')), bar * 0.75], [[], bar * 1.25]]), 0, [-30, 30, 0]),
      ['BELL', 40, -30, every(0, secs, bar * 2, (t) => [[t, 2.5, n('D5'), 100]])],
    ] };
  })(),
};

// ---- Encode ----------------------------------------------------------------------------------
const instNames = Object.keys(INSTRUMENTS), pcm = {}, blobs = [], offs = [];
let at = 0;
for (const name of instNames) {
  const [make, loop] = INSTRUMENTS[name];
  pcm[name] = normalise(make(), PEAK[name] ?? 0.8);
  const b = encode(pcm[name], { loop });
  offs.push([at, b.length]);
  blobs.push(b);
  at += b.length;
}
writeFileSync(path.join(root, 'assets', 'music.bin'), Buffer.concat(blobs));

const songNames = Object.keys(SONGS);
let h = `/* Generated by tools/makemusic.mjs: do not edit. */
#pragma once
#include <stdint.h>
#define MUSIC_RATE  ${RATE}
#define MUSIC_BYTES ${at}
enum {
${instNames.map((x) => `\tINST_${x},`).join('\n')}
\tNUM_INSTRUMENTS
};
typedef struct {
	uint32_t offset;     /* in music.bin */
	uint8_t base_note;   /* MIDI note the sample plays at its own rate */
	uint16_t adsr1, adsr2;
} Instrument;
static const Instrument instruments[NUM_INSTRUMENTS] = {
${instNames.map((x, i) => `\t{ ${offs[i][0]}, ${INSTRUMENTS[x][2]}, 0x${INSTRUMENTS[x][3].toString(16).padStart(4, '0')}, 0x${INSTRUMENTS[x][4].toString(16).padStart(4, '0')} },   /* ${x} */`).join('\n')}
};
typedef struct {
	uint16_t at, len;   /* ticks (1/${TICKS} s) */
	uint8_t note, vel;
} Note;
typedef struct {
	uint8_t inst, vol;  /* vol 0..127 */
	int8_t pan;         /* -64..64 */
	uint8_t nnotes;
	const Note *notes;
} Track;
typedef struct {
	uint16_t length;    /* ticks */
	uint8_t ntracks;
	const Track *tracks;
} Song;
enum {
${songNames.map((x) => `\tSONG_${x},`).join('\n')}
\tNUM_SONGS
};
`;
const tick = (s) => Math.round(s * TICKS);
for (const sn of songNames) {
  const song = SONGS[sn];
  song.tracks.forEach(([, , , notes], ti) => {
    const sorted = [...notes].sort((a, b) => a[0] - b[0]);
    h += `static const Note ${sn.toLowerCase()}_${ti}[] = {\n${sorted.map(([t, l, nn, v]) => `\t{ ${tick(t)}, ${Math.max(1, tick(l))}, ${nn}, ${v} },`).join('\n')}\n};\n`;
  });
  h += `static const Track ${sn.toLowerCase()}_tracks[] = {\n${song.tracks.map(([inst, vol, pan, notes], ti) => `\t{ INST_${inst}, ${vol}, ${pan}, ${notes.length}, ${sn.toLowerCase()}_${ti} },`).join('\n')}\n};\n`;
}
h += `static const Song songs[NUM_SONGS] = {\n${songNames.map((sn) => `\t{ ${tick(SONGS[sn].secs)}, ${SONGS[sn].tracks.length}, ${sn.toLowerCase()}_tracks },`).join('\n')}\n};\n`;
writeFileSync(path.join(root, 'src', 'music_data.h'), h);
console.log(`music.bin: ${instNames.length} instruments, ${at} bytes; ${songNames.length} songs, ${Math.max(...songNames.map((s) => SONGS[s].tracks.length))} tracks at most`);

// ---- Preview -------------------------------------------------------------------------------
const previewDir = process.argv[2];
if (previewDir) {
  mkdirSync(previewDir, { recursive: true });
  // The SPU's envelope, roughly: attack time from the rate, exponential release.
  const attackSecs = (ar) => (ar < 44 ? 0.005 : 0.106 * 2 ** ((ar >> 2) - 11));
  const releaseTau = (rr) => 0.093 * 2 ** (rr - 11);
  for (const sn of songNames) {
    const song = SONGS[sn], total = len(song.secs * 2), L = new Float64Array(total), R = new Float64Array(total);
    for (const [inst, vol, pan, notes] of song.tracks) {
      const [, loop, base, a1, a2] = INSTRUMENTS[inst], src = pcm[inst];
      const atk = attackSecs(a1 >> 8), tau = releaseTau(a2 & 31);
      for (let rep = 0; rep < 2; rep++) for (const [t0, l, nn, v] of notes) {
        const start = len(t0 + rep * song.secs), step = 2 ** ((nn - base) / 12), g = vol / 127 * v / 127 * 0.45;   // MUSIC_GAIN (music.c)
        const gl = g * Math.min(1, (64 - pan) / 64), gr = g * Math.min(1, (64 + pan) / 64);
        let pos = 0, env = 0;
        for (let i = 0; start + i < total; i++) {
          const t = i / RATE;
          if (t < l) env = Math.min(1, t / atk); else env *= Math.exp(-1 / (tau * RATE));
          if (t > l && env < 0.001) break;
          let p = pos;
          if (p >= src.length) { if (!loop) break; p %= src.length; pos = p; }
          const j = Math.floor(p), f = p - j, s = src[j] + ((src[(j + 1) % src.length] || 0) - src[j]) * f;
          L[start + i] += s * env * gl; R[start + i] += s * env * gr;
          pos += step;
        }
      }
    }
    let peak = 0;
    for (let i = 0; i < total; i++) peak = Math.max(peak, Math.abs(L[i]), Math.abs(R[i]));
    const wav = Buffer.alloc(44 + total * 4);
    wav.write('RIFF', 0); wav.writeUInt32LE(36 + total * 4, 4); wav.write('WAVEfmt ', 8);
    wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(2, 22); wav.writeUInt32LE(RATE, 24);
    wav.writeUInt32LE(RATE * 4, 28); wav.writeUInt16LE(4, 32); wav.writeUInt16LE(16, 34); wav.write('data', 36); wav.writeUInt32LE(total * 4, 40);
    const k = peak > 0.95 ? 0.95 / peak : 1;
    for (let i = 0; i < total; i++) {
      wav.writeInt16LE(Math.round(Math.max(-1, Math.min(1, L[i] * k)) * 32767), 44 + i * 4);
      wav.writeInt16LE(Math.round(Math.max(-1, Math.min(1, R[i] * k)) * 32767), 46 + i * 4);
    }
    writeFileSync(path.join(previewDir, `${sn.toLowerCase()}.wav`), wav);
    console.log(`${sn.toLowerCase()}.wav: ${song.secs.toFixed(1)} s x 2, peak ${peak.toFixed(2)}`);
  }
  // The voices one after another: whisper, sigh, choir swell.
  const parts = ['WHISPER', 'SIGH', 'SWELL'].flatMap((k) => [...pcm[k], ...new Array(len(0.6)).fill(0)]);
  const wav = Buffer.alloc(44 + parts.length * 2);
  wav.write('RIFF', 0); wav.writeUInt32LE(36 + parts.length * 2, 4); wav.write('WAVEfmt ', 8);
  wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(1, 22); wav.writeUInt32LE(RATE, 24);
  wav.writeUInt32LE(RATE * 2, 28); wav.writeUInt16LE(2, 32); wav.writeUInt16LE(16, 34); wav.write('data', 36); wav.writeUInt32LE(parts.length * 2, 40);
  parts.forEach((v, i) => wav.writeInt16LE(Math.round(v * 32000), 44 + i * 2));
  writeFileSync(path.join(previewDir, 'voices.wav'), wav);
}
