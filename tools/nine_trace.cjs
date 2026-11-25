#!/usr/bin/env node
// Source-graph trajectory trace for the C reader differential test.
//
// Runs the pinned source engine.js (the authority for conditions, effects,
// ordered nexts, choice filtering and the 500-hop limit) over the real graph
// and chunk files, and writes a compact binary trace. The C test replays the
// same walk through nine_vm.c and compares every step, including the permanent
// / per-run variable digest.
//
// Usage: node tools/nine_trace.cjs --source <nine-haruiro-rw5> --report <pack report> --out <trace.bin>
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
const source = path.resolve(arg('source'));
const reportPath = arg('report');
const report = JSON.parse(fs.readFileSync(reportPath, 'utf8'));
const scriptBytes = fs.readFileSync(path.join(path.dirname(reportPath), 'nine_script.bin'));
const scriptCrc = scriptBytes.readUInt32LE(16);
const out = arg('out');
const graphText = fs.readFileSync(path.join(source, 'src/common/graph.js'), 'utf8');
const graph = JSON.parse(graphText.slice(graphText.indexOf('=') + 1).trim().replace(/;$/, ''));

function load(file, names, expression) {
  let code = fs.readFileSync(path.join(source, 'src/common', file), 'utf8');
  code = code.replace(/^export /gm, '').replace(/export \{[^}]+\};?/, '');
  code += ';globalThis.api={' + names.join(',') + '};';
  const box = { globalThis: null };
  box.globalThis = box;
  vm.runInNewContext(code, box);
  return box.api;
}
const engine = load('engine.js',
  ['create', 'locate', 'advance', 'choose']);
const codec = load('chunk-codec.js', ['decodeChunk']);

const chunks = new Map();
function frames(file) {
  if (!chunks.has(file)) {
    const raw = JSON.parse(fs.readFileSync(path.join(source, 'src/common/story', file), 'utf8'));
    chunks.set(file, codec.decodeChunk(raw));
  }
  return chunks.get(file);
}

function fnv(bytes) {
  let h = 0x811c9dc5;
  for (const b of bytes) {
    h ^= b;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}
function digest(state) {
  const parts = [];
  for (const v of report.variables) {
    const value = v.persistent ? Number(state.progress[v.name] || 0) : Number(state.flag[v.name] || 0);
    parts.push(v.name + '=' + value + ';');
  }
  return fnv(Buffer.from(parts.join(''), 'utf8'));
}

const state = engine.create(graph, {});
const steps = [];
for (;;) {
  const view = engine.locate(graph, state);
  const node = state.node === '@end' ? 0xffff : report.nodeOrder.indexOf(state.node);
  if (node < 0) throw Error('node not in report: ' + state.node);
  const index = state.index;
  const h = digest(state);
  if (view.type === 'end') {
    steps.push({ kind: 3, node, index, h, text: '' });
    break;
  }
  if (view.type === 'op') {
    steps.push({ kind: 1, node, index, h, text: '' });
    engine.advance(graph, state);
    continue;
  }
  if (view.type === 'choice') {
    const labels = view.items.map((x) => x.text);
    if (view.items.length !== 1) throw Error('unexpected choice count at ' + node);
    steps.push({ kind: 2, node, index, h, text: labels.join('\n') });
    engine.choose(graph, state, 0);
    continue;
  }
  const f = frames(view.file)[view.index];
  if (!f || f.kind === 'op') throw Error('bad frame at ' + node + ':' + index);
  steps.push({ kind: 0, node, index, h, text: f.text });
  engine.advance(graph, state);
  if (steps.length > 20000) throw Error('trajectory did not terminate');
}

const bufs = [];
const head = Buffer.alloc(16);
head.write('NINETR1\0', 0, 'latin1');
head.writeUInt32LE(steps.length, 8);
head.writeUInt32LE(scriptCrc, 12);
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
fs.writeFileSync(out, Buffer.concat(bufs));
const counts = steps.reduce((a, s) => (a[s.kind] = (a[s.kind] || 0) + 1, a), {});
console.log(JSON.stringify({ steps: steps.length, kinds: counts, bytes: fs.statSync(out).size }));
