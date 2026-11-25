#!/usr/bin/env node
// Source-JS reference trace for the C VM differential test.
// Loads the pinned engine.js/graph.js/chunk-codec.js directly, so the expected
// trajectory is produced by the original source implementation.
'use strict';
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
function arg(name) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
}
const source = arg('--source');
const choices = (arg('--choices') || '').split(',').filter((x) => x !== '').map(Number);
const limit = Number(arg('--limit') || 20000);
if (!source) throw Error('--source required');
const root = fs.existsSync(path.join(source, 'nine-sorairo-rw5'))
  ? path.join(source, 'nine-sorairo-rw5') : source;

function loadModule(file, exportName) {
  let text = fs.readFileSync(path.join(root, 'src/common', file), 'utf8');
  text = text.replace(/export\s+function/g, 'function')
    .replace(/export\s+const/g, 'const')
    .replace(/export\s*\{[^}]*\}\s*;?/g, '');
  const body = `const module={exports:{}};${text};return ${exportName};`;
  return new Function(body)();
}
const engine = loadModule('engine.js',
  '{create,checkpoint,restore,locate,advance,choose,skipChapter,previous,flags,matches}');
const codec = loadModule('chunk-codec.js', '{decodeChunk}');
let graphText = fs.readFileSync(path.join(root, 'src/common/graph.js'), 'utf8').trim();
graphText = graphText.replace(/^export const GRAPH\s*=\s*/, '').replace(/;\s*$/, '');
const graph = JSON.parse(graphText);

const PERSISTENT = new Set(['loop1', 'clear_true', 'titleBg', 'loop_my', 'loop_sr', 'cf_sr',
  'loop_hk', 'cf_hk', 'loop_na', 'cf_na', 'true']);
const keys = new Set(PERSISTENT);
for (const ending of graph.endings || []) keys.add(ending.id);
for (const node of Object.values(graph.nodes)) {
  for (const effect of node.effects || []) keys.add(effect[0]);
  for (const choice of node.choices || []) {
    for (const effect of choice.effects || []) keys.add(effect[0]);
    addWhen(choice.when);
  }
  for (const next of node.nexts || []) addWhen(next.when);
}
function addWhen(when) {
  if (!when) return;
  if (when[0] === 'all') { for (const item of when.slice(1)) addWhen(item); return; }
  keys.add(when[0]);
}
const sortedKeys = [...keys].sort();

function fnv(bytes) {
  let h = 2166136261;
  for (const b of bytes) { h ^= b; h = Math.imul(h, 16777619) >>> 0; }
  return h >>> 0;
}
function hashText(value) {
  return fnv(Buffer.from(String(value || ''), 'utf8')).toString(16).padStart(8, '0');
}
function hashMap(flag, progress) {
  const bytes = [];
  for (const key of sortedKeys) {
    bytes.push(...Buffer.from(key, 'utf8'));
    for (const value of [Number(flag[key] || 0), Number(progress[key] || 0)]) {
      const v = value >>> 0;
      bytes.push(v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255);
    }
  }
  return fnv(bytes).toString(16).padStart(8, '0');
}

const cache = new Map();
function framesFor(file) {
  if (!cache.has(file)) {
    const raw = fs.readFileSync(path.join(root, 'src/common/story', file), 'utf8');
    cache.set(file, codec.decodeChunk(JSON.parse(raw)));
  }
  return cache.get(file);
}

const session = engine.create(graph, {});
session.progression = 'story';
let choicePos = 0;
for (let step = 0; step < limit; step++) {
  const view = engine.locate(graph, session);
  let kind = 4, text = '', speaker = '', labels = [];
  if (view.type === 'end') {
    kind = 3;
  } else if (view.type === 'choice') {
    kind = 2;
    labels = view.items.map((item) => item.text || '');
  } else if (view.type === 'op') {
    kind = 1;
  } else if (view.type === 'text') {
    kind = 0;
    const frame = framesFor(view.file)[view.index];
    text = frame && frame.text || '';
    speaker = frame && frame.speaker || '';
  }
  const labelHash = hashText(labels.join('\u0000'));
  console.log([kind, session.node, session.index, hashText(text), hashText(speaker),
    hashMap(session.flag, session.progress), labelHash, labels.length].join(' '));
  if (kind === 3) break;
  if (kind === 2) {
    const pick = choicePos < choices.length ? choices[choicePos++] : 0;
    engine.choose(graph, session, pick);
  } else if (kind === 0 || kind === 1) {
    engine.advance(graph, session);
  } else {
    break;
  }
}
