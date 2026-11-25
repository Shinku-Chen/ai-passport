#!/usr/bin/env node
// Source engine.js traces for the synthetic semantic cases. Writes the same
// compact binary format as tools/nine_trace.cjs (kind/node/index/digest/text),
// plus kind 9 for a locate error.
//
// Usage: node tests/nine_host/synth_trace.cjs --source <repo> --cases <cases.json> --out <dir>
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const args = process.argv.slice(2);
function arg(name) {
  const i = args.indexOf('--' + name);
  if (i < 0 || i + 1 >= args.length) throw Error('missing --' + name);
  return args[i + 1];
}
const cases = JSON.parse(fs.readFileSync(arg('cases'), 'utf8')).cases;
const outDir = arg('out');
const packDir = arg('packs');
fs.mkdirSync(outDir, { recursive: true });

const source = path.resolve(arg('source'));
let code = fs.readFileSync(path.join(source, 'src/common/engine.js'), 'utf8');
code = code.replace(/^export /gm, '').replace(/export \{[^}]+\};?/, '');
code += ';globalThis.api={create,locate,advance,choose};';
const box = { globalThis: null };
box.globalThis = box;
vm.runInNewContext(code, box);
const engine = box.api;

function fnv(bytes) {
  let h = 0x811c9dc5;
  for (const b of bytes) {
    h ^= b;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

for (const original of cases) {
  const c = JSON.parse(JSON.stringify(original));
  if (c.chain) {
    c.nodes = {};
    for (let i = 0; i < c.chain; ++i) {
      c.nodes['n' + i] = { count: 0, nexts: [{ to: i + 1 < c.chain ? 'n' + (i + 1) : '@end' }] };
    }
  }
  const names = Object.keys(c.nodes);
  const index = Object.fromEntries(names.map((n, i) => [n, i]));
  const graph = { version: 1, start: c.start, nodes: {}, migrations: {} };
  for (const name of names) {
    const n = c.nodes[name];
    const frames = n.frames || [];
    graph.nodes[name] = {
      count: n.count !== undefined ? n.count : frames.length,
      chunks: frames.length ? [{ file: name, count: frames.length }] : [],
      ops: [],
      effects: n.effects || [],
      complete: n.complete || [],
      choices: (n.choices || []).map((x) => Object.assign({}, x)),
      nexts: (n.nexts || []).map((x) => Object.assign({}, x)),
    };
  }
  const state = engine.create(graph, {});
  if (c.preview) state.progression = 'chapter';
  if (c.start !== graph.start) state.node = c.start;
  const policy = c.choose || [];
  const steps = [];
  let choiceAt = 0;
  let error = null;
  for (let guard = 0; guard < 2000 && !error; ++guard) {
    let view;
    try {
      view = engine.locate(graph, state);
    } catch (e) {
      error = e.message;
      break;
    }
    const node = state.node === '@end' ? 0xffff : index[state.node];
    const digestParts = [];
    for (const v of c.vars) {
      const value = c.persistent.indexOf(v) >= 0
        ? Number(state.progress[v] || 0) : Number(state.flag[v] || 0);
      digestParts.push(v + '=' + value + ';');
    }
    const h = fnv(Buffer.from(digestParts.join(''), 'utf8'));
    if (view.type === 'end') {
      steps.push({ kind: 3, node, index: state.index, h, text: '' });
      break;
    }
    if (view.type === 'choice') {
      steps.push({ kind: 2, node, index: state.index, h,
        text: view.items.map((x) => x.text).join('\n') });
      const pick = policy[choiceAt++] || 0;
      engine.choose(graph, state, pick);
      continue;
    }
    const frames = c.nodes[state.node].frames || [];
    steps.push({ kind: 0, node, index: state.index, h, text: frames[state.index] || '' });
    engine.advance(graph, state);
  }
  if (error) {
    steps.push({ kind: 9, node: state.node === '@end' ? 0xffff : index[state.node],
      index: state.index, h: 0, text: error });
  }
  const bufs = [];
  const packBytes = fs.readFileSync(path.join(packDir, c.name + '.bin'));
  const head = Buffer.alloc(16);
  head.write('NINETR1\0', 0, 'latin1');
  head.writeUInt32LE(steps.length, 8);
  head.writeUInt32LE(packBytes.readUInt32LE(16), 12);
  bufs.push(head);
  for (const s of steps) {
    const text = Buffer.from(s.text, 'utf8');
    const rec = Buffer.alloc(13);
    rec.writeUInt8(s.kind, 0);
    rec.writeUInt16LE(s.node, 1);
    rec.writeUInt32LE(s.index, 3);
    rec.writeUInt32LE(s.h, 7);
    rec.writeUInt16LE(text.length, 11);
    bufs.push(rec, text);
  }
  fs.writeFileSync(path.join(outDir, c.name + '.trace'), Buffer.concat(bufs));
  console.log(c.name, steps.length, steps[steps.length - 1].kind);
}
