#!/usr/bin/env node
// Source-side transcript for the C differential test. Loads the pinned Watch
// graph and the *unmodified* engine.js / chunk-codec.js through node:vm.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const src = process.argv[2];
const scenarioPath = process.argv[3];
if (!src || !scenarioPath) {
  console.error('usage: nine_source_trace.cjs <nine-kokoiro-rw5> <scenario>');
  process.exit(2);
}
const root = path.resolve(src);
function loadModule(file, exportNames) {
  let code = fs.readFileSync(path.join(root, file), 'utf8');
  code = code.replace(/export \{[^}]+\};/g, '');
  code = code.replace(/export\s+(function|const|let|class)/g, '$1');
  code += `;globalThis.api={${exportNames.join(',')}}`;
  const box = {};
  vm.runInNewContext(code, box);
  return box.api;
}
const engine = loadModule('src/common/engine.js',
  ['create', 'advance', 'choose', 'locate', 'previous', 'skipChapter', 'checkpoint', 'restore']);
const codec = loadModule('src/common/chunk-codec.js', ['decodeChunk']);
const graphText = fs.readFileSync(path.join(root, 'src/common/graph.js'), 'utf8');
const GRAPH = JSON.parse(graphText.slice(graphText.indexOf('{'), graphText.lastIndexOf('}') + 1));
const chunkCache = new Map();
function framesOf(file) {
  if (!chunkCache.has(file)) {
    const raw = JSON.parse(fs.readFileSync(path.join(root, 'src/common/story', file), 'utf8'));
    chunkCache.set(file, codec.decodeChunk(raw));
  }
  return chunkCache.get(file);
}
function hex(s) {
  if (s === null || s === undefined) return '-';
  return Buffer.from(String(s), 'utf8').toString('hex');
}
function frameAt(loc) {
  const frames = framesOf(loc.file);
  return frames[loc.index] || { speaker: '', text: '' };
}
function stateOf(s) {
  const pairs = [];
  for (const k of Object.keys(s.flag || {}).sort()) pairs.push(`${k}=${Number(s.flag[k])}`);
  let out = pairs.join(';') + '|';
  pairs.length = 0;
  for (const k of Object.keys(s.progress || {}).sort()) pairs.push(`${k}=${Number(s.progress[k])}`);
  return out + pairs.join(';');
}
function nodeIdOf(s) {
  return s.node === '@end' ? '@end' : s.node;
}
function emit(session) {
  let loc;
  try {
    loc = engine.locate(GRAPH, session);
  } catch (e) {
    console.log(`S error ${nodeIdOf(session)} ${session.index} - ${hex(e.message)} ${stateOf(session)}`);
    return;
  }
  if (loc.type === 'text') {
    const f = frameAt(loc);
    console.log(`S text ${nodeIdOf(session)} ${session.index} ${hex(f.speaker || '')} ${hex(f.text || '')} ${stateOf(session)}`);
  } else if (loc.type === 'op') {
    console.log(`S op ${nodeIdOf(session)} ${session.index} - - ${stateOf(session)}`);
  } else if (loc.type === 'choice') {
    const labels = loc.items.map((x) => hex(x.text)).join(',');
    console.log(`S choice ${nodeIdOf(session)} ${session.index} - ${labels} ${stateOf(session)}`);
  } else if (loc.type === 'end') {
    console.log(`S end @end ${session.index} - - ${stateOf(session)}`);
  } else {
    console.log(`S error ${nodeIdOf(session)} ${session.index} - ${hex('unknown')} ${stateOf(session)}`);
  }
}

let progress = {};
let session = null;
const lines = fs.readFileSync(scenarioPath, 'utf8').split(/\r?\n/);
for (const raw of lines) {
  const line = raw.trim();
  if (!line || line.startsWith('#')) continue;
  const parts = line.split(/\s+/);
  const cmd = parts[0];
  if (cmd === 'progress') {
    for (const kv of parts.slice(1)) {
      const at = kv.indexOf('=');
      if (at > 0) progress[kv.slice(0, at)] = Number(kv.slice(at + 1));
    }
  } else if (cmd === 'start' || cmd === 'newgame') {
    session = engine.create(GRAPH, progress);
    emit(session);
  } else if (cmd === 'preview') {
    const ch = (GRAPH.chapters || []).find((c) => c.id === parts[1] || c.node === parts[1]);
    if (!ch) { console.log('E unknown chapter'); continue; }
    session = engine.create(GRAPH, progress);
    session.node = ch.node;
    session.index = 0;
    session.progression = 'chapter';
    emit(session);
  } else if (cmd === 'advance') {
    const count = Number(parts[1] || 0);
    for (let i = 0; i < count; i++) {
      try { engine.advance(GRAPH, session); } catch (e) { break; }
      emit(session);
      const t = engine.locate(GRAPH, session).type;
      if (t !== 'text' && t !== 'op') break;
    }
  } else if (cmd === 'choose') {
    try { engine.choose(GRAPH, session, Number(parts[1] || 0)); } catch (e) { /* emit error below */ }
    emit(session);
  } else if (cmd === 'previous') {
    engine.previous(GRAPH, session);
    emit(session);
  } else if (cmd === 'skip') {
    try { engine.skipChapter(GRAPH, session); } catch (e) { /* ignore */ }
    emit(session);
  } else if (cmd === 'auto') {
    const count = Number(parts[1] || 0);
    const policy = (parts[2] || '').split(',').filter((x) => x !== '').map(Number);
    let pick = 0;
    for (let i = 0; i < count; i++) {
      emit(session);
      const loc = engine.locate(GRAPH, session);
      if (loc.type === 'text' || loc.type === 'op') {
        engine.advance(GRAPH, session);
      } else if (loc.type === 'choice') {
        let sel = policy.length ? policy[pick % policy.length] : 0;
        pick++;
        if (sel >= loc.items.length) sel = 0;
        engine.choose(GRAPH, session, sel);
      } else break;
    }
  } else {
    console.log('E unknown command');
  }
}
