// PS1 SPU ADPCM encoding, shared by tools/makesfx.mjs and tools/makemusic.mjs.
//
// encode(samples): a one-shot sound (floats, -1..1): a silent block, then the
// end flag (the voice stops). encode(samples, { loop: true }): the whole
// buffer loops forever (its length should be a multiple of 28 samples).
// Output padded to 64 bytes.
const F = [[0, 0], [60, 0], [115, -52], [98, -55], [122, -60]];
export function encode(samples, { loop = false } = {}) {
  const out = [];
  let s1 = 0, s2 = 0;
  const pcm = samples.map((v) => Math.max(-32768, Math.min(32767, Math.round(v * 32000))));
  while (pcm.length % 28) pcm.push(0);
  if (!loop) pcm.push(...Array(28).fill(0));   // a silent block before the end
  // A loop's start is decoded after its own end: start from those samples.
  if (loop) { s1 = pcm[pcm.length - 1]; s2 = pcm[pcm.length - 2]; }
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
  if (loop) {
    out[0][1] = 4;                 // loop start
    out[out.length - 1][1] = 3;    // loop end: back to the start
  } else {
    out[out.length - 1][1] = 1;    // end: the voice stops
  }
  const buf = Buffer.from(out.flat());
  return Buffer.concat([buf, Buffer.alloc((64 - (buf.length % 64)) % 64)]);
}

