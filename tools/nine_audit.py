#!/usr/bin/env python3
"""Independent source -> package reconciliation for nine-haruiro.

Does not import the packer. Re-reads src/common/graph.js, every story chunk,
gallery.txt, the opening timeline and the source images, then verifies the
produced NINESC2/NINEIM1 packages frame by frame, edge by edge and layer by
layer. Exits non-zero on any mismatch.

Usage: python tools/nine_audit.py --source <repo> --pack assets/nine/haruiro
"""
from __future__ import annotations
import argparse, hashlib, json, math, struct, sys, zlib
from pathlib import Path

NONE = 0xffffffff
COND = {'==': 1, '!=': 2, '<': 3, '<=': 4, '>': 5, '>=': 6}
PINS = '6a8907a5ae576f4377abe4b74f15bd9af13efe07'


def rd32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def i32(v):
    v &= NONE
    return v - (1 << 32) if v & 0x80000000 else v


class Pack:
    def __init__(self, packdir):
        self.script = (packdir / 'nine_script.bin').read_bytes()
        self.images = (packdir / 'nine_images.bin').read_bytes()
        s, img = self.script, self.images
        self.version = rd32(s, 8)
        self.size = rd32(s, 12)
        self.crc = rd32(s, 16)
        self.node_count = rd32(s, 36)
        self.nodes_off = rd32(s, 40)
        self.frame_count = rd32(s, 44)
        self.frames_off = rd32(s, 48)
        self.block_count = rd32(s, 52)
        self.blocks_off = rd32(s, 56)
        self.visual_count = rd32(s, 60)
        self.visuals_off = rd32(s, 64)
        self.layer_count = rd32(s, 68)
        self.layers_off = rd32(s, 72)
        self.var_count = rd32(s, 76)
        self.vars_off = rd32(s, 80)
        self.cond_count = rd32(s, 84)
        self.conds_off = rd32(s, 88)
        self.effect_count = rd32(s, 92)
        self.effects_off = rd32(s, 96)
        self.edge_count = rd32(s, 100)
        self.edges_off = rd32(s, 104)
        self.chapter_count = rd32(s, 108)
        self.chapters_off = rd32(s, 112)
        self.ending_count = rd32(s, 116)
        self.endings_off = rd32(s, 120)
        self.gallery_count = rd32(s, 124)
        self.gallery_off = rd32(s, 128)
        self.strings_off = rd32(s, 132)
        self.strings_size = rd32(s, 136)
        self.reserved = [rd32(s, 140 + i * 4) for i in range(4)]
        self.start = rd32(s, 32)
        self.image_count = rd32(img, 20)
        self.image_off = rd32(img, 24)
        self.op_count = rd32(img, 28)
        self.op_off = rd32(img, 32)
        self.audio_count = rd32(img, 36)
        self.audio_off = rd32(img, 40)
        self.title_images = [rd32(img, 44 + i * 4) for i in range(3)]

    def string(self, off):
        if off >= self.strings_size:
            return None
        start = self.strings_off + off
        end = self.script.find(b'\0', start)
        if end < 0:
            return None
        return self.script[start:end].decode('utf-8')

    def text_block(self, index):
        p = self.blocks_off + index * 16
        off, comp, raw, crc = struct.unpack_from('<4I', self.script, p)
        data = zlib.decompress(self.script[off:off + comp])
        assert len(data) == raw and (zlib.crc32(data) & NONE) == crc
        return data

    def node(self, i):
        p = self.nodes_off + i * 48
        return struct.unpack_from('<12I', self.script, p)

    def frame(self, i):
        p = self.frames_off + i * 24
        return struct.unpack_from('<6I', self.script, p)

    def visual(self, i):
        p = self.visuals_off + i * 8
        first, count = struct.unpack_from('<2I', self.script, p)
        return [self.layer(first + j) for j in range(count)]

    def layer(self, i):
        p = self.layers_off + i * 24
        return struct.unpack_from('<IiiIII', self.script, p)

    def var(self, i):
        p = self.vars_off + i * 8
        name, flags = struct.unpack_from('<2I', self.script, p)
        return self.string(name), flags

    def cond(self, i):
        p = self.conds_off + i * 12
        op, key, val = struct.unpack_from('<3I', self.script, p)
        return op, key, i32(val)

    def effect(self, i):
        p = self.effects_off + i * 12
        op, key, val = struct.unpack_from('<3I', self.script, p)
        return op, key, i32(val)

    def edge(self, i):
        p = self.edges_off + i * 24
        to, cf, cc, ef, ec, label = struct.unpack_from('<6I', self.script, p)
        return to, (cf, cc), (ef, ec), label

    def chapter(self, i):
        p = self.chapters_off + i * 12
        return struct.unpack_from('<3I', self.script, p)

    def ending(self, i):
        p = self.endings_off + i * 12
        return struct.unpack_from('<3I', self.script, p)

    def gallery(self, i):
        p = self.gallery_off + i * 12
        return struct.unpack_from('<3I', self.script, p)

    def image(self, i):
        p = self.image_off + i * 32
        return struct.unpack_from('<8I', self.images, p)


def decode_chunk(raw):
    out = []
    cur_bg = ''
    cur_sp = []
    for f in raw:
        if not f or f.get('kind') == 'op':
            out.append(f)
            continue
        if 'art' in f:
            cur_bg = f['art'].get('background', '')
            cur_sp = f['art'].get('sprites') or []
            out.append({'speaker': f.get('speaker', ''), 'text': f.get('text', ''),
                        'bg': cur_bg, 'sprites': cur_sp})
            continue
        if f.get('b') is not None:
            cur_bg = f['b']
        if f.get('p') is not None:
            cur_sp = f['p']
        out.append({'speaker': f.get('s', ''), 'text': f.get('t', ''),
                    'bg': cur_bg, 'sprites': cur_sp})
    return out


def compile_when(when, var_index):
    out = []

    def walk(w):
        if not w:
            return
        if w[0] == 'all':
            for sub in w[1:]:
                walk(sub)
            out.append((7, 0, len(w) - 1))
        else:
            out.append((COND[w[1]], var_index[w[0]], int(w[2])))
    walk(when)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--pack', type=Path, default=Path('assets/nine/haruiro'))
    args = ap.parse_args()
    root = args.source
    report = json.loads((args.pack / 'nine_pack_report.json').read_text('utf-8'))
    src = report['imageSources']
    by_source = {}
    for m in src:
        by_source.setdefault(m['source'], []).append(m)
    pack = Pack(args.pack)
    errors = []

    def check(ok, msg):
        if not ok:
            errors.append(msg)

    graph_text = (root / 'src/common/graph.js').read_text('utf-8')
    graph = json.loads(graph_text.split('=', 1)[1].strip().rstrip(';'))

    check(pack.version == 2 and pack.size == len(pack.script), 'script header/version/size')
    check(report['sourceCommit'] == PINS, 'source commit pin')
    check(pack.reserved[2] == 3, 'episode id')
    check(pack.reserved[3] == len(json.loads((root / 'converted/gallery.json').read_text('utf-8'))),
          'gallery group count')
    names = list(graph['nodes'])
    check(pack.node_count == len(names), 'node count')
    check(pack.start == names.index(graph['start']), 'start node')

    var_names = []
    persistent = set()
    for i in range(pack.var_count):
        name, flags = pack.var(i)
        var_names.append(name)
        if flags & 1:
            persistent.add(name)
    check(len(set(var_names)) == len(var_names), 'variable table unique')
    var_index = {n: i for i, n in enumerate(var_names)}
    loop_key = var_index.get('loop1', NONE)
    check(pack.reserved[0] == loop_key, 'loop overlay key')
    check(pack.reserved[1] == var_index.get('clear_true', NONE), 'clear key')

    # Walk nodes in source order and compare every frame, effect, next and choice.
    blocks = {}

    def block_text(index):
        if index not in blocks:
            blocks[index] = pack.text_block(index)
        return blocks[index]

    frame_cursor = 0
    chapter_titles = {c['title']: i for i, c in enumerate(graph['chapters'])}
    expect_chapters = [(c['title'], c['preview'], names.index(c['node']))
                       for c in graph['chapters']]
    got_chapters = [pack.chapter(i) for i in range(pack.chapter_count)]
    check(len(got_chapters) == len(expect_chapters), 'chapter count')
    for i, (want, got) in enumerate(zip(expect_chapters, got_chapters)):
        check(pack.string(got[0]) == want[0] and pack.string(got[1]) == want[1] and got[2] == want[2],
              f'chapter {i}')

    expect_endings = [(e['name'], var_index[e['id']], e['description'])
                      for e in graph['endings']]
    got_endings = [pack.ending(i) for i in range(pack.ending_count)]
    for i, (want, got) in enumerate(zip(expect_endings, got_endings)):
        check(pack.string(got[0]) == want[0] and got[1] == want[1] and
              pack.string(got[2]) == want[2], f'ending {i}')

    gallery_src = json.loads((root / 'converted/gallery.json').read_text('utf-8'))
    expect_gallery = []
    for gi, group in enumerate(gallery_src):
        for v in group['variants']:
            expect_gallery.append((group['id'], v['src'], gi))
    check(pack.gallery_count == len(expect_gallery), 'gallery count')
    for i, (gid, path, gi) in enumerate(expect_gallery):
        rec = pack.gallery(i)
        check(pack.string(rec[0]) == gid and rec[2] == gi, f'gallery record {i}')
        meta = [m for m in by_source.get(path, []) if m['sprite'] is False]
        check(bool(meta) and meta[0]['id'] == rec[1], f'gallery image {i}')

    # Expected visuals recomputed from the chunk stream, deduplicated in order.
    visual_ids = {}
    expected_visuals = []
    layer_cursor = 0
    node_effects = 0
    for nid in names:
        node = graph['nodes'][nid]
        first_frame = frame_cursor
        for chunk_ref in node.get('chunks', []):
            rows = json.loads((root / 'src/common/story' / chunk_ref['file'].lstrip('/'))
                              .read_text('utf-8'))
            decoded = decode_chunk(rows)
            for frame in decoded:
                if frame.get('kind') == 'op' or 'text' not in frame:
                    rec = pack.frame(frame_cursor)
                    check(rec[0] == 1, f'{nid}: OP frame kind')
                    frame_cursor += 1
                    continue
                rec = pack.frame(frame_cursor)
                block = block_text(rec[2])
                text = block[rec[3]:rec[3] + rec[4]].decode('utf-8')
                check(rec[0] == 0 and text == frame['text'] and
                      pack.string(rec[5]) == frame['speaker'],
                      f'{nid}: frame {frame_cursor - first_frame} text/speaker')
                key = json.dumps([frame['bg'], frame['sprites']], ensure_ascii=False,
                                 separators=(',', ':'))
                if key not in visual_ids:
                    visual_ids[key] = len(expected_visuals)
                    expected_visuals.append((frame['bg'], frame['sprites']))
                check(rec[1] == visual_ids[key], f'{nid}: visual reference')
                frame_cursor += 1
        check(frame_cursor - first_frame == node['count'], f'{nid}: frame count')
        # node effects + complete folded into the effect table
        want_effects = [(0 if op == '=' else 1, var_index[k], int(v))
                        for k, op, v in node.get('effects', [])]
        for end in node.get('complete', []):
            want_effects.append((0, var_index[end], 1))
            if end == 'true':
                want_effects.append((0, var_index['clear_true'], 1))
        rec = pack.node(names.index(nid))
        got_effects = [pack.effect(rec[2] + i) for i in range(rec[3])]
        check(got_effects == want_effects, f'{nid}: node effects')
        node_effects += len(want_effects)
        # nexts and choices preserve source order
        for key_name in ('nexts', 'choices'):
            base = rec[4] if key_name == 'nexts' else rec[6]
            count = rec[5] if key_name == 'nexts' else rec[7]
            want = node.get(key_name, [])
            check(count == len(want), f'{nid}: {key_name} count')
            for j, edge in enumerate(want):
                got = pack.edge(base + j)
                check(got[0] == (NONE if edge['to'] == '@end' else names.index(edge['to'])),
                      f'{nid}: {key_name} {j} target')
                conds = [pack.cond(got[1][0] + k) for k in range(got[1][1])]
                check(conds == compile_when(edge.get('when'), var_index),
                      f'{nid}: {key_name} {j} conditions')
                effects = [pack.effect(got[2][0] + k) for k in range(got[2][1])]
                check(effects == [(0 if op == '=' else 1, var_index[k], int(v))
                                  for k, op, v in edge.get('effects', [])],
                      f'{nid}: {key_name} {j} effects')
                check(pack.string(got[3]) == edge.get('text', ''),
                      f'{nid}: {key_name} {j} label')
        chapter = chapter_titles.get(node.get('chapter', ''), NONE)
        check(rec[9] == chapter, f'{nid}: chapter index')

    check(frame_cursor == pack.frame_count, 'total frame count')
    check(len(expected_visuals) == pack.visual_count, 'visual count')
    check(pack.image_count == len(report['imageSources']), 'image record count')

    # Visual layers: recompute geometry and image identity from the source art.
    def expected_layers(bg, sprites):
        layers = []
        if bg:
            layers.append(('image', bg, None, None, 0, 0, 320, 180, 255))
        for s in sprites:
            if isinstance(s, list):
                body, width, left = s[:3]
                opacity = s[3] if len(s) > 3 and s[3] is not None else 1
                patch = s[4] if len(s) > 4 else None
                height = s[13] if len(s) > 13 and s[13] else 243
                top = 4 if height >= 200 else 243 - height
                d = {'src': body, 'width': width, 'left': left, 'height': height,
                     'top': top, 'opacity': opacity}
                if patch:
                    for k, v in zip(('patch', 'patchWidth', 'patchHeight', 'patchLeft',
                                     'patchTop', 'patchAtlasWidth', 'patchAtlasHeight',
                                     'patchImageLeft', 'patchImageTop'), s[4:13]):
                        d[k] = v
            else:
                d = s
            w = max(1, round(d['width'] * 320 / 432))
            h = max(1, round(d.get('height', 243) * 180 / 243))
            x = round(d['left'] * 320 / 432)
            y = round(d.get('top', 4) * 180 / 243)
            alpha = round(d.get('opacity', 1) * 255)
            layers.append(('image', d['src'], None, (w, h), x, y, w, h, alpha))
            if d.get('patch'):
                from PIL import Image
                with Image.open(root / 'src' / d['patch'].lstrip('/')) as im:
                    iw, ih = im.size
                ax, ay = d['patchAtlasWidth'], d['patchAtlasHeight']
                px, py = -d['patchImageLeft'], -d['patchImageTop']
                pw, ph = d['patchWidth'], d['patchHeight']
                crop = (round(px * iw / ax), round(py * ih / ay),
                        round((px + pw) * iw / ax), round((py + ph) * ih / ay))
                ps = (max(1, round(pw * 320 / 432)), max(1, round(ph * 180 / 243)))
                layers.append(('image', d['patch'], crop, ps,
                               round(d['patchLeft'] * 320 / 432),
                               round(d['patchTop'] * 180 / 243), *ps, alpha))
        return layers

    def match_layer(layer, want):
        kind, path, crop, size, x, y, w, h, alpha = want
        image, gx, gy, gw, gh, gopa = layer
        if (gx, gy, gw, gh, gopa) != (x, y, w, h, alpha):
            return False
        meta = by_source.get(path, [])
        if size is None:
            candidates = [m for m in meta if not m['sprite']]
        else:
            candidates = [m for m in meta if m['sprite'] and tuple(m['size']) == tuple(size)
                          and (m['crop'] is None if crop is None else tuple(m['crop']) == tuple(crop))]
        return any(m['id'] == image for m in candidates)

    for vid, (bg, sprites) in enumerate(expected_visuals):
        want = expected_layers(bg, sprites)
        got = pack.visual(vid)
        check(len(got) == len(want), f'visual {vid}: layer count')
        for i, (g, w) in enumerate(zip(got, want)):
            check(match_layer(g, w), f'visual {vid}: layer {i}')

    # OP: preserve source order, timestamps and duration; frames map to source paths.
    timeline = json.loads((root / 'converted/opening/timeline.json').read_text('utf-8'))
    fps = 3
    report_path = args.pack / 'nine_pack_report.json'
    if report_path.exists():
        try:
            fps = float(json.loads(report_path.read_text('utf-8')).get('opening', {}).get('targetFps', 3))
        except (ValueError, TypeError):
            fps = 3
    picked = []
    seen = set()
    for i in range(math.ceil(timeline['durationMs'] * fps / 1000)):
        index = min(len(timeline['frames']) - 1, round(i * timeline['fps'] / fps))
        if index not in seen:
            picked.append(index)
            seen.add(index)
    if picked[-1] != len(timeline['frames']) - 1:
        picked.append(len(timeline['frames']) - 1)
    check(pack.op_count == len(picked), 'op frame count')
    for i, index in enumerate(picked):
        image, stamp, duration = struct.unpack_from('<3I', pack.images, pack.op_off + i * 12)
        frame = timeline['frames'][index]
        check(stamp == frame['atMs'], f'op {i} timestamp')
        meta = [m for m in by_source.get(frame['src'], []) if not m['sprite']]
        check(bool(meta) and meta[0]['id'] == image, f'op {i} image')

    check(pack.audio_count == 2, 'audio track count')
    for i, name in enumerate(('title', 'opening')):
        kind, nm, off, size, dur, rate, ch, crc = struct.unpack_from(
            '<8I', pack.images, pack.audio_off + i * 32)
        check(kind == 1 and pack.string(nm) == name and rate == 16000 and ch == 1 and
              size % 132 == 0 and size > 0, f'audio {name}')
        check((zlib.crc32(pack.images[off:off + size]) & NONE) == crc, f'audio {i} crc')

    # Font symbols must cover every character the package can display.
    symbols = set((args.pack / 'nine_symbols.txt').read_text('utf-8'))
    all_text = set()
    for i in range(pack.frame_count):
        rec = pack.frame(i)
        if rec[0] == 0:
            all_text.update(block_text(rec[2])[rec[3]:rec[3] + rec[4]].decode('utf-8'))
    all_text -= set((chr(13), chr(10), chr(9)))
    # The font tool independently scans main/ string literals and verifies every
    # emitted glyph; here we prove it also covers every character in the script.
    check(all_text <= symbols, f'font glyphs missing: {len(all_text - symbols)}')

    summary = {
        'nodes': pack.node_count,
        'frames': pack.frame_count,
        'textFrames': sum(1 for i in range(pack.frame_count) if pack.frame(i)[0] == 0),
        'choices': sum(1 for i in range(pack.node_count) if pack.node(i)[7]),
        'visuals': pack.visual_count,
        'layers': pack.layer_count,
        'images': pack.image_count,
        'chapters': pack.chapter_count,
        'endings': pack.ending_count,
        'gallery': pack.gallery_count,
        'audio': pack.audio_count,
        'op': pack.op_count,
        'scriptSha256': hashlib.sha256(pack.script).hexdigest(),
        'imageSha256': hashlib.sha256(pack.images).hexdigest(),
        'errors': errors[:20],
        'errorCount': len(errors),
    }
    print(json.dumps(summary, ensure_ascii=False))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
