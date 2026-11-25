#!/usr/bin/env node
// Emit a deterministic source-JS -> C VM differential trace for 9-nine Episode 4.
// Uses the unmodified source engine.js/chunk-codec.js and the generated pack's
// variable table. No source file is written.
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

function arg(name, fallback = null) {
  const i = process.argv.indexOf(name);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
}

const source = path.resolve(arg('--source'));
const packDir = path.resolve(arg('--pack'));
const outPath = path.resolve(arg('--out', path.join(packDir, 'nine_trace.bin')));
const episode = arg('--episode', 'yukiiro');
const src = path.join(source, `nine-${episode}-rw5`);

const graphText = fs.readFileSync(path.join(src, 'src/common/graph.js'), 'utf8');
const GRAPH = JSON.parse(graphText.split('=').slice(1).join('=').trim().replace(/;\s*$/, ''));
const engine = await import(pathToFileURL(path.join(src, 'src/common/engine.js')).href);
const codec = await import(pathToFileURL(path.join(src, 'src/common/chunk-codec.js')).href);

// Variable table from the generated pack: the C VM indexes state arrays by this.
const pack = fs.readFileSync(path.join(packDir, 'nine_script.bin'));
const rd32 = (o) => pack.readUInt32LE(o);
const varCount = rd32(76);
const varOff = rd32(80);
const strOff = rd32(132);
const string = (o) => {
  const end = pack.indexOf(0, strOff + o);
  return pack.subarray(strOff + o, end).toString('utf8');
};
const vars = [];
for (let i = 0; i < varCount; i++) vars.push(string(rd32(varOff + i * 8)));
const keyIndex = new Map(vars.map((v, i) => [v, i]));
const nodeIds = Object.keys(GRAPH.nodes);
const nodeIndex = new Map(nodeIds.map((n, i) => [n, i]));

const chunkCache = new Map();
function frameAt(file, index) {
  if (!chunkCache.has(file)) {
    const rows = JSON.parse(fs.readFileSync(path.join(src, 'src/common/story', file), 'utf8'));
    chunkCache.set(file, codec.decodeChunk(rows));
  }
  return chunkCache.get(file)[index];
}

function fnvBytes(h, bytes) {
  for (const b of bytes) {
    h ^= b;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}
function fnvU32(h, v) {
  return fnvBytes(h, [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]);
}
function hashString(s) {
  return fnvBytes(0x811c9dc5, Buffer.from(s || '', 'utf8'));
}
function stateHash(state) {
  let hf = 0x811c9dc5;
  let hp = 0x811c9dc5;
  for (let i = 0; i < vars.length; i++) {
    const name = vars[i];
    hf = fnvU32(hf, Number(state.flag[name] || 0) >>> 0);
    hp = fnvU32(hp, Number(state.progress[name] || 0) >>> 0);
  }
  return [hf, hp];
}
function choiceHash(items) {
  let h = 0x811c9dc5;
  for (const item of items) {
    h = fnvU32(h, hashString(item.text || ''));
    const to = item.to === '@end' ? 0xffffffff : nodeIndex.get(item.to);
    h = fnvU32(h, (to === undefined ? 0xffffffff : to) >>> 0);
    for (const ef of item.effects || []) {
      h = fnvU32(h, keyIndex.get(ef[0]) >>> 0);
      h = fnvU32(h, ef[1] === '=' ? 0 : 1);
      h = fnvU32(h, Number(ef[2]) >>> 0);
    }
  }
  return h;
}

function makeStep(state, p) {
  const [fh, ph] = stateHash(state);
  let type = 3;
  let textHash = 0;
  let ch = 0;
  if (p.type === 'text') {
    type = 0;
    const f = frameAt(p.file, p.index);
    textHash = hashString(f.text || '');
  } else if (p.type === 'op') {
    type = 1;
  } else if (p.type === 'choice') {
    type = 2;
    ch = choiceHash(p.items);
  }
  return {
    type, node: (nodeIndex.get(state.node) ?? 0xffff) >>> 0, index: state.index >>> 0,
    textHash, flagHash: fh, progressHash: ph, choiceHash: ch, action: 0, arg: 0,
  };
}

function progressPairs(progress) {
  const out = [];
  for (const [name, value] of Object.entries(progress || {})) {
    if (keyIndex.has(name)) out.push([keyIndex.get(name), Number(value) | 0]);
  }
  return out;
}

function runTrace(initialProgress, strategy, force) {
  const start = engine.create(GRAPH, initialProgress);
  let p = engine.locate(GRAPH, start);
  const steps = [];
  let forceUsed = false;
  for (let guard = 0; guard < 200000; guard++) {
    const step = makeStep(start, p);
    if (p.type === 'end') {
      steps.push(step);
      if (start.progress && start.progress.true) endingKinds.add('true');
      else if (start.progress && start.progress.loop1) endingKinds.add('loop1');
      else endingKinds.add('none');
      break;
    }
    if (p.type === 'text' || p.type === 'op') {
      steps.push(step);
      p = engine.advance(GRAPH, start);
      continue;
    }
    // choice
    let pick = strategy(p.items, start);
    if (Array.isArray(force)) {
      const at = force.findIndex((f) => !f.used && f.node === start.node);
      if (at >= 0) {
        const want = force[at].text;
        const found = p.items.findIndex((it) => (it.text || '') === want);
        if (found >= 0) {
          pick = found;
          force[at].used = true;
        }
      }
    } else if (force && !forceUsed && start.node === force.node) {
      pick = Math.min(force.option, p.items.length - 1);
      forceUsed = true;
    }
    step.action = 1;
    step.arg = pick;
    steps.push(step);
    const chosen = p.items[pick];
    if (chosen) chosenEdges.add(start.node + '|' + (chosen.text || ''));
    p = engine.choose(GRAPH, start, pick);
  }
  return { initial: progressPairs(initialProgress), final: progressPairs(start.progress), steps };
}

const strategies = [
  (items) => 0,
  (items) => items.length - 1,
  (items, state) => (items.length ? (hashString(state.node) + state.index) % items.length : 0),
  (items, state) => (items.length ? (hashString(state.node + ':' + state.index) >>> 3) % items.length : 0),
];

const traces = [];
const seen = new Set();
const chosenEdges = new Set();
const endingKinds = new Set();
function add(initialProgress, strategy, force, label) {
  const t = runTrace(initialProgress, strategy, force);
  if (t.steps.length < 2) return null;
  const sig = JSON.stringify([t.initial, t.final, t.steps.map((s) => [s.node, s.index, s.action, s.arg])]);
  if (seen.has(sig)) return null;
  seen.add(sig);
  t.label = label;
  traces.push(t);
  return t;
}

// Baseline fresh runs; capture the permanent progress they reach.
const base = add([], strategies[0], null, 'fresh-first') || add([], strategies[1], null, 'fresh-last');
const cleared = base ? base.final : [];
add([], strategies[1], null, 'fresh-last');
for (let seed = 2; seed < 70; seed++) {
  const st = strategies[seed % strategies.length];
  add([], st, null, `fresh-seed-${seed}`);
}
if (cleared.length) {
  for (let seed = 0; seed < 24; seed++) {
    const st = strategies[seed % strategies.length];
    add(cleared, st, null, `cleared-seed-${seed}`);
  }
}

// Multi-branch nodes: prove each branch edge at each occurrence with a forced
// branch sequence, because a chosen branch is hidden from later occurrences.
const BRANCHES = new Map();
for (const [nid, n] of Object.entries(GRAPH.nodes)) {
  if ((n.choices || []).length > 1) {
    BRANCHES.set(nid, n.choices.map((c) => c.text));
  }
}
for (const [nid, texts] of BRANCHES) {
  const nodeList = [...BRANCHES.keys()];
  const idx = nodeList.indexOf(nid);
  for (const target of texts) {
    const seq = [];
    const others = texts.filter((t) => t !== target);
    for (let p = 0; p < idx; p++) {
      seq.push({ node: nodeList[p], text: others[p % others.length] });
    }
    seq.push({ node: nid, text: target });
    add([], strategies[0], seq.map((x) => ({ ...x })), `branch-${nid}-${target}`);
    if (cleared.length) add(cleared, strategies[0], seq.map((x) => ({ ...x })), `branch-cleared-${nid}-${target}`);
  }
}

// Route-aware fallback edges: after all branches are set, visibleChoices falls
// back to every item. Reach these nodes through the preceding branch chain.
const BR = Object.fromEntries([...BRANCHES].map(([k, v]) => [k, v]));
if (BR.n53 && BR.n53.length === 3) {
  add([], strategies[0], [
    { node: 'n30', text: BR.n30[0] }, { node: 'n37', text: BR.n37[0] },
    { node: 'n45', text: BR.n45[2] }, { node: 'n53', text: BR.n53[2] },
  ], 'fallback-n53-3');
}
if (BR.n177 && BR.n177.length === 4) {
  add([], strategies[0], [
    { node: 'n146', text: BR.n146[0] }, { node: 'n153', text: BR.n153[0] },
    { node: 'n161', text: BR.n161[0] }, { node: 'n169', text: BR.n169[0] },
    { node: 'n177', text: BR.n177[3] },
  ], 'fallback-n177-4');
}

// Force every visible option at every choice node reached by the broad walks.
// This is what proves each of the 70 option edges, not just a sample.
const reachable = new Map(); // node -> max visible count
function recordReach(items, state) {
  const n = reachable.get(state.node) || 0;
  if (items.length > n) reachable.set(state.node, items.length);
  return 0;
}
// Collect from all already-generated traces by replaying their strategy is not
// possible, so walk broadly with every strategy and both progress states.
for (let seed = 0; seed < 160; seed++) {
  const st = strategies[seed % strategies.length];
  runTrace([], recordReach, null);
  runTrace(cleared, recordReach, null);
  runTrace([], st, null);
  runTrace(cleared, st, null);
}
for (let round = 0; round < 3; round++) {
  for (const [node, max] of [...reachable]) {
    for (let option = 0; option < max; option++) {
      for (let s = 0; s < strategies.length; s++) {
        add([], strategies[s], { node, option }, `force-fresh-${node}-${option}-${s}`);
        if (cleared.length) add(cleared, strategies[s], { node, option }, `force-cleared-${node}-${option}-${s}`);
      }
    }
  }
  // Newly forced runs may reach further nodes; record those too.
  for (let seed = 160 + round * 80; seed < 240 + round * 80; seed++) {
    const st = strategies[seed % strategies.length];
    runTrace([], recordReach, null);
    runTrace(cleared, recordReach, null);
    add([], st, null, `expand-${seed}`);
    if (cleared.length) add(cleared, st, null, `expand-cleared-${seed}`);
  }
}
for (let seed = 500; seed < 620; seed++) {
  add(strategies[seed % strategies.length] ? (cleared.length ? cleared : []) : [], strategies[seed % strategies.length], null, `tail-${seed}`);
}

// Binary trace: magic/version/count; per trace progress pairs then 36-byte steps.
const chunks = [];
let totalSteps = 0;
for (const t of traces) {
  const head = Buffer.alloc(8);
  head.writeUInt32LE(t.steps.length, 0);
  head.writeUInt32LE(t.initial.length, 4);
  chunks.push(head);
  for (const [k, v] of t.initial) {
    const b = Buffer.alloc(8);
    b.writeUInt32LE(k, 0);
    b.writeInt32LE(v, 4);
    chunks.push(b);
  }
  for (const s of t.steps) {
    const b = Buffer.alloc(36);
    b.writeUInt32LE(s.type, 0);
    b.writeUInt32LE(s.node >>> 0, 4);
    b.writeUInt32LE(s.index >>> 0, 8);
    b.writeUInt32LE(s.textHash >>> 0, 12);
    b.writeUInt32LE(s.flagHash >>> 0, 16);
    b.writeUInt32LE(s.progressHash >>> 0, 20);
    b.writeUInt32LE(s.choiceHash >>> 0, 24);
    b.writeUInt32LE(s.action >>> 0, 28);
    b.writeUInt32LE(s.arg >>> 0, 32);
    chunks.push(b);
    totalSteps++;
  }
}
const header = Buffer.alloc(12);
header.write('NVT1', 0, 'ascii');
header.writeUInt32LE(1, 4);
header.writeUInt32LE(traces.length, 8);
fs.writeFileSync(outPath, Buffer.concat([header, ...chunks]));

const allEdges = new Set();
for (const [nid, n] of Object.entries(GRAPH.nodes)) for (const c of n.choices || []) allEdges.add(nid + '|' + (c.text || ''));
const missing = [...allEdges].filter((x) => !chosenEdges.has(x));
const totalOptions = allEdges.size;
console.log(JSON.stringify({
  traces: traces.length, steps: totalSteps, bytes: fs.statSync(outPath).size,
  choiceSelections: chosenEdges.size, totalOptions, endingKinds: [...endingKinds],
  vars: vars.length, clearedKeys: cleared.length, missing, baseProgress: base ? base.final : [], out: outPath,
}));
