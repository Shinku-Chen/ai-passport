// tests/summer_source_trace.mjs — run the *unmodified* RW5 source engine and
// emit a trace that tests/summer_c_trace.c and summer_trace_compare.py can
// diff against the production C model.  This is a tracked maintenance test; it
// imports the source snapshot via SUMMER_SOURCE and never writes there.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const args = process.argv.slice(2);
const opt = {};
for (let i = 0; i < args.length; ++i) {
  if (args[i].startsWith('--')) opt[args[i].slice(2)] = args[i + 1] && !args[i + 1].startsWith('--') ? args[++i] : true;
}
const source = process.env.SUMMER_SOURCE || opt.source;
if (!source) {
  console.error('SUMMER_SOURCE is required');
  process.exit(2);
}
const root = path.resolve(source);
const catalog = (await import(pathToFileURL(path.join(root, 'src/common/catalog.js')).href)).catalog;
const engine = await import(pathToFileURL(path.join(root, 'src/common/engine.js')).href);
const names = await import(pathToFileURL(path.join(root, 'src/common/names.js')).href);
const progressMod = await import(pathToFileURL(path.join(root, 'src/common/title-progress.js')).href);
const {newState, readFrame, next, choose, previewMap, sceneLabel} = engine;
const {visibleText} = names;
const {withProgress, emptyProgress, FLOW_ENDINGS} = progressMod;

const storyDir = path.join(root, 'src/common/story');
const mapOps = JSON.parse(fs.readFileSync(path.join(root, 'src/common/map-preview.txt'), 'utf8'));
const mode = opt.mode || 'story';
const choiceMode = opt.choice || 'first';
const mapMode = opt.map || 'first';
const sceneArg = opt.scene ? Number(opt.scene) : 0;

function opsFor(chapter) {
  const info = catalog.scenes[String(chapter)];
  return {ops: null, base: 0, total: info ? info.length : 0};
}

function loadChapter(chapter, cursor) {
  const block = Math.floor(cursor / 1024);
  const file = path.join(storyDir, String(chapter), `${block}.txt`);
  return {ops: JSON.parse(fs.readFileSync(file, 'utf8')), base: block * 1024};
}

function frameOf(state) {
  const {ops, base} = loadChapter(state.chapter, state.cursor);
  const total = catalog.scenes[String(state.chapter)].length;
  return {ops, frame: readFrame(ops, state, base, total)};
}

function esc(value) {
  return String(value ?? '').replace(/\\/g, '\\\\').replace(/\t/g, '\\t').replace(/\r?\n/g, '\\n');
}

function emitT(state, speaker, text) {
  const label = visibleText(sceneLabel(state), true);
  console.log(`T\t${state.chapter}\t${state.cursor}\t${esc(visibleText(speaker, true))}\t${esc(visibleText(text))}\t${esc(label)}`);
}

function emitD(state) {
  console.log(`D\t${state.chapter}\t${esc(state.strings?.['106'] || '')}\t${esc(visibleText(sceneLabel(state), true))}`);
}

function makeProgress() {
  if (mode === 'alka' || mode === 'pocket') {
    return {version: 1, cleared: [4, 5, 6, 7, 13, 8, 9], restartStage: 3, globals: {}};
  }
  return emptyProgress();
}

if (mode === 'labels') {
  for (const chapter of catalog.chapters) {
    const state = newState(chapter.id);
    let out = null;
    for (let i = 0; i < 8; ++i) {
      const f = frameOf(state).frame;
      out = f;
      if (f.type === 'text') break;
      if (f.type === 'choice' || f.type === 'map' || f.type === 'end') break;
    }
    const label = visibleText(sceneLabel(state), true);
    if (out && out.type === 'text') {
      console.log(`L\t${chapter.id}\t${esc(label)}\t${esc(visibleText(out.speaker, true))}\t${esc(visibleText(out.text))}\t${esc(state.strings?.['106'] || '')}`);
    } else {
      console.log(`L\t${chapter.id}\t${esc(label)}\t\t\t${esc(state.strings?.['106'] || '')}`);
    }
  }
  process.exit(0);
}

let state;
const progress = makeProgress();
if (mode === 'chapter') {
  state = newState(sceneArg);
} else {
  state = withProgress(newState(catalog.flowScene, true), progress);
  if (mode === 'alka') state.variables['6004'] = 1;
  else if (mode === 'pocket') state.variables['6004'] = 2;
}

let lastDate = null;
for (let guard = 0; guard < 500000; ++guard) {
  if (state.progression === 'story') withProgress(state, progress);
  const {ops, frame} = frameOf(state);
  if (frame.type === 'yield' || frame.type === 'jump') continue;
  if (frame.type === 'text') {
    const date = state.strings?.['106'] || '';
    if (date && date !== lastDate) {
      lastDate = date;
      emitD(state);
    }
    emitT(state, frame.speaker, frame.text);
    next(state);
    continue;
  }
  if (frame.type === 'choice') {
    console.log(`C\t${state.chapter}\t${state.cursor}\t${frame.items.length}`);
    for (const item of frame.items) console.log(`I\t${item.value}\t${esc(visibleText(item.text))}`);
    const index = choiceMode === 'last' ? frame.items.length - 1 : 0;
    choose(state, frame.items[index], index);
    continue;
  }
  if (frame.type === 'map') {
    const items = previewMap(mapOps, state, frame);
    console.log(`M\t${state.chapter}\t${state.cursor}\t${items.length}`);
    for (const item of items) console.log(`I\t${item.value}\t${item.destination}\t${esc(visibleText(item.text))}`);
    if (!items.length) {
      console.log(`E\t${state.chapter}\t${state.cursor}\tMEXHAUST`);
      process.exit(0);
    }
    const index = mapMode === 'last' ? items.length - 1 : 0;
    choose(state, items[index], index);
    continue;
  }
  if (frame.type === 'end') {
    const ending = state.chapter === catalog.flowScene ? (FLOW_ENDINGS?.[state.cursor] || 0) : 0;
    console.log(`E\t${state.chapter}\t${state.cursor}\t${ending}`);
    process.exit(0);
  }
  console.error('unexpected frame', frame.type);
  process.exit(3);
}
console.error('trace limit');
process.exit(3);
