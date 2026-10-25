#!/usr/bin/env node
// A bot that plays Grid Souls through the ps1-test harness, for balancing:
// it reads the game's memory each frame and plays a sensible, not perfect,
// game (out of the telegraphed tiles, close in, strike, riposte, drink),
// then reports deaths, time and damage taken per run.
//
//   node tools/playtest.mjs [runs=3] [harness=/home/user/ps1-test/harness]
//
// Struct offsets follow src/game.h (Player, Enemy); keep them in step.
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const RUNS = Number(process.argv[2] ?? 3);
// Reaction time in frames: a blow is only seen this long after its windup
// starts (a person needs a moment); and a slip now and then.
const REACT = Number(process.env.REACT ?? 14), SLIP = Number(process.env.SLIP ?? 0.08);
let rngState = Number(process.env.SEED ?? 1);
const rand = () => ((rngState = (rngState * 1103515245 + 12345) >>> 0) / 4294967296);
const HARNESS = process.argv[3] ?? '/home/user/ps1-test/harness';
const { EmuHarness } = await import(pathToFileURL(path.join(HARNESS, 'driver.mjs')).href);

const N = 9, ENEMY = 128, MAXE = 6, PLAYER = 108;
const P = { x: 0, y: 4, fx: 8, fy: 12, facing: 16, state: 20, t: 24, hp: 28, stamina: 32, flasks: 40, step: 88 };
const E = { state: 0, t: 4, x: 8, y: 12, hp: 24, windup: 28, attack: 32, target: 36, facing: 117, kind: 124 };
const PS = { IDLE: 0, DODGE: 1, ATTACK: 2, DRINK: 3, CHARGE: 4, DEAD: 5, WALK: 6, JUMP: 7, FALL: 8, PUSH: 9 };
const ES = { NONE: 0, IDLE: 1, MOVE: 2, WINDUP: 3, RECOVER: 4, STAGGER: 5, DYING: 6, SLEEP: 7, WAKE: 8, RISE: 9 };
const DIRS = [['UP', 0, -1], ['RIGHT', 1, 0], ['DOWN', 0, 1], ['LEFT', -1, 0]];
const SLOW = (f) => Math.floor(f * 5 / 4), WALK = SLOW(20), DODGE = SLOW(12), DODGE_COST = 800;

const i32 = (b, o) => b.readInt32LE(o);

async function readState(emu) {
  const s = emu.symbols;
  const [pb, eb, glob, area] = await Promise.all([
    emu.readMemory(s.resolve('player'), PLAYER),
    emu.readMemory(s.resolve('enemies'), ENEMY * MAXE),
    Promise.all(['floor_state', 'wave', 'screen', 'floor_t', 'boss'].map((n) => emu.readMemory(s.resolve(n), 4))),
    Promise.all(['area_x0', 'area_y0', 'area_x1', 'area_y1'].map((n) => emu.readMemory(s.resolve(n), 4))),
  ]);
  const p = Object.fromEntries(Object.entries(P).map(([k, o]) => [k, i32(pb, o)]));
  const enemies = [];
  for (let i = 0; i < MAXE; i++) {
    const b = eb.subarray(i * ENEMY, (i + 1) * ENEMY);
    const e = Object.fromEntries(Object.entries(E).filter(([k]) => k !== 'target' && k !== 'facing').map(([k, o]) => [k, i32(b, o)]));
    e.facing = b[E.facing];
    e.target = b.subarray(E.target, E.target + N * N);
    e.i = i;
    enemies.push(e);
  }
  const [floor_state, wave, screen, floor_t, boss] = glob.map((b) => i32(b, 0));
  const [x0, y0, x1, y1] = area.map((b) => i32(b, 0));
  return { p, enemies, floor_state, wave, screen, floor_t, boss, area: { x0, y0, x1, y1 } };
}

// Frames until a blow lands on each tile (Infinity: none coming).
const slips = [];
function dangerMap(st) {
  const d = Array.from({ length: N }, () => new Array(N).fill(Infinity));
  for (const e of st.enemies) {
    if (e.state !== ES.WINDUP || e.t < REACT) continue;   /* not noticed yet */
    const m = slips[e.i];                                  /* misread this one? (once per blow) */
    if (!m || e.t < m.t || m.attack !== e.attack) slips[e.i] = { slip: rand() < SLIP };
    slips[e.i].t = e.t; slips[e.i].attack = e.attack;
    if (slips[e.i].slip) continue;
    const left = e.windup - e.t;
    for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) if (e.target[y * N + x] && left < d[y][x]) d[y][x] = left;
  }
  return d;
}

const inside = (st, x, y) => x >= st.area.x0 && x <= st.area.x1 && y >= st.area.y0 && y <= st.area.y1;
const alive = (e) => e.state !== ES.NONE && e.state !== ES.DYING;
const occupied = (st, x, y) => st.enemies.some((e) => alive(e) && e.x === x && e.y === y);

// The next button press (or hold) to make, from the state.
function decide(st, danger, mem) {
  const { p } = st;
  if (p.state === PS.DEAD) return st.floor_t > 100 ? { tap: ['CROSS'] } : null;
  if (st.floor_state === 0 && st.wave > 0 && p.state === PS.IDLE && (p.x !== 4 || p.y !== 4)) {
    /* a breather: the edge is about to fall, get to the middle */
    const [b] = DIRS.find(([, dx, dy]) => (dx && Math.sign(4 - p.x) === dx) || (dy && Math.sign(4 - p.y) === dy));
    return { hold: [b], frames: 3 };
  }
  if (st.floor_state !== 1 || [PS.DODGE, PS.FALL, PS.PUSH, PS.JUMP].includes(p.state)) return null;
  const here = danger[p.y]?.[p.x] ?? Infinity;
  const busy = p.state === PS.ATTACK || p.state === PS.DRINK || p.state === PS.WALK || p.state === PS.CHARGE;
  // 0. Next to a foe with time to land a swing before its blow: strike first.
  const fd = DIRS[p.facing];
  const adj = st.enemies.find((e) => alive(e) && e.state !== ES.RISE && e.x === p.x + fd[1] && e.y === p.y + fd[2]);
  if (adj && !busy && p.stamina > 0 && here < Infinity && here > 16) return { tap: ['SQUARE'] };
  // 1. Out of a blow's way: walk if there's time, else dodge, else jump.
  if (here < Infinity) {
    const safe = (x, y, by) => inside(st, x, y) && !occupied(st, x, y) && (danger[y][x] === Infinity || danger[y][x] > by);
    if (here > WALK + 6 && !busy) {
      for (const [b, dx, dy] of DIRS) if (safe(p.x + dx, p.y + dy, WALK + 2)) return { hold: [b], frames: 3 };
    }
    if (here <= WALK + 6 && here >= 2) {
      if (p.stamina >= DODGE_COST) {
        for (const [b, dx, dy] of DIRS) {
          const tx = p.x + dx * 2, ty = p.y + dy * 2;
          if (safe(tx, ty, DODGE) && !occupied(st, tx, ty)) return { tap: [b, 'CIRCLE'] };
        }
      }
      if (p.stamina > 0 && here >= 4 && here <= 24) return { tap: ['CROSS'] };
    }
    if (busy) return null;
  }
  if (busy) return null;
  // 2. Low: drink when nothing is coming soon.
  if (p.hp < 45 && p.flasks > 0 && here === Infinity && !st.enemies.some((e) => e.state === ES.WINDUP && e.windup - e.t < 50))
    return { tap: ['TRIANGLE'] };
  const foes = st.enemies.filter((e) => alive(e) && e.state !== ES.RISE);
  if (!foes.length) return null;
  // 3. A foe next to us, along the way we face: strike (a riposte if broken,
  //    a heavy blow if it's recovering for a while).
  const facing = DIRS[p.facing];
  const front = foes.find((e) => e.x === p.x + facing[1] && e.y === p.y + facing[2]);
  if (front && p.stamina > 0) {
    if (front.state === ES.STAGGER) return { tap: ['SQUARE'] };
    const quiet = front.state === ES.RECOVER || front.state === ES.SLEEP || (front.state === ES.WINDUP && front.windup - front.t > 26);
    if (front.state === ES.RECOVER && front.t < 6 && p.stamina > 480) return { hold: ['SQUARE'], frames: 40 };
    if (quiet || front.state === ES.IDLE || front.state === ES.MOVE) return { tap: ['SQUARE'] };
  }
  // 4. Close in on the nearest, to a tile beside it, off the telegraphs.
  const near = foes.sort((a, b) => Math.abs(a.x - p.x) + Math.abs(a.y - p.y) - (Math.abs(b.x - p.x) + Math.abs(b.y - p.y)))[0];
  const goals = DIRS.map(([, dx, dy]) => [near.x + dx, near.y + dy]).filter(([x, y]) => inside(st, x, y) && !occupied(st, x, y) && danger[y][x] === Infinity);
  if (!goals.length) return null;
  const [gx, gy] = goals.sort((a, b) => Math.abs(a[0] - p.x) + Math.abs(a[1] - p.y) - (Math.abs(b[0] - p.x) + Math.abs(b[1] - p.y)))[0];
  if (gx === p.x && gy === p.y) return null;
  // A step along the longer way that isn't about to be struck.
  const steps = DIRS.filter(([, dx, dy]) => Math.sign(gx - p.x) === dx && dx !== 0 || Math.sign(gy - p.y) === dy && dy !== 0)
    .filter(([, dx, dy]) => inside(st, p.x + dx, p.y + dy) && !occupied(st, p.x + dx, p.y + dy) && danger[p.y + dy][p.x + dx] > WALK + 4);
  if (steps.length) return { hold: [steps[0][0]], frames: 3 };
  return null;
}

async function run(n) {
  const emu = await EmuHarness.launch('build/game.cue');
  const sym = (name) => emu.symbols.resolve(name);
  const r32 = async (name) => (await emu.readMemory(sym(name), 4)).readInt32LE(0);
  // Through the title and the opening.
  await emu.waitFor({ address: sym('boot_magic'), as: 'u32', op: 'eq', value: 0x51D50001 }, { maxFrames: 3000 });
  await emu.waitFor({ address: sym('screen'), as: 's32', op: 'eq', value: 0 }, { maxFrames: 3000 });
  await emu.waitFrames(60);
  await emu.tap('START', 4, 4); await emu.waitFrames(20);
  await emu.tap('CROSS', 4, 4);
  await emu.waitFor({ address: sym('screen'), as: 's32', op: 'eq', value: 2 }, { maxFrames: 300 });
  await emu.waitFrames(30); await emu.tap('CROSS', 4, 4);
  await emu.waitFor({ address: sym('screen'), as: 's32', op: 'eq', value: 1 }, { maxFrames: 300 });
  const start = await emu.frameNum();
  let deaths = 0, lastState = -1, waveDeaths = [0, 0, 0, 0], hurt = 0, lastHp = 100, frames = 0;
  const LIMIT = 60 * 60 * Number(process.env.MINUTES ?? 12);   // minutes of game
  let nextLog = 0;
  for (;;) {
    const st = await readState(emu);
    if (st.screen === 3 || st.screen === 0) break;
    frames = (await emu.frameNum()) - start;
    if (frames > LIMIT) break;
    if (st.p.state === PS.DEAD && lastState !== PS.DEAD) { deaths++; waveDeaths[st.boss >= 0 ? 3 : Math.min(st.wave, 2)]++; }
    if (st.p.hp < lastHp && st.p.state !== PS.DEAD) hurt += lastHp - st.p.hp;
    lastHp = st.p.state === PS.DEAD ? 100 : st.p.hp;
    lastState = st.p.state;
    if (process.env.VERBOSE && frames >= nextLog) {
      nextLog = frames + 300;
      console.log(frames, 'wave', st.wave, 'fs', st.floor_state, 'p', st.p.x, st.p.y, 'st', st.p.state, 'hp', st.p.hp, 'sta', st.p.stamina, 'en', st.enemies.filter(alive).map((e) => `${e.x},${e.y} s${e.state} hp${e.hp}`).join(' '));
    }
    const act = decide(st, dangerMap(st));
    if (!act) { await emu.waitFrames(2); continue; }
    if (act.tap) await emu.tap(act.tap, 2, 1);
    else { await emu.setButtons(act.hold, true); await emu.waitFrames(act.frames); await emu.setButtons(act.hold, false); }
  }
  const stats = await emu.readMemory(sym('stats'), 28);
  const won = (await r32('screen')) === 3;
  await emu.close();
  const s = (o) => stats.readInt32LE(o);
  return { run: n, won, minutes: +(frames / 3600).toFixed(1), deaths, byWave: waveDeaths, ripostes: s(16), hurt: s(20), flasks: s(24) };
}

const results = [];
for (let i = 1; i <= RUNS; i++) {
  const r = await run(i);
  results.push(r);
  console.log(JSON.stringify(r));
}
const won = results.filter((r) => r.won).length;
console.log(`won ${won}/${RUNS}; deaths avg ${(results.reduce((a, r) => a + r.deaths, 0) / RUNS).toFixed(1)}; ` +
  `deaths by wave (I, II, III, boss): ${[0, 1, 2, 3].map((w) => results.reduce((a, r) => a + r.byWave[w], 0)).join(', ')}`);
process.exit(0);
