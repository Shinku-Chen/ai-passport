#!/usr/bin/env python3
"""Independent source -> pack audit for 9-nine- 九次九日九重色 (Ep1 only).

This reader does not import tools/nine_pack.py. It re-parses the pinned Watch
graph and story chunks, then checks every graph decision (text, speaker, first
matching next, conditions, effects, choices, chapters, endings) and every visual
layer against the compiled pack. Image *pixels* are checked by nine_image_audit.

Usage:
  python tools/nine_audit.py --source <9-nine-rw5> --episode kokoiro \
      --pack-dir assets/nine/kokoiro --out build/nine/audit-kokoiro.json
"""
from __future__ import annotations
import argparse, hashlib, json, struct, sys, zlib
from pathlib import Path

NONE = 0xffffffff
U32 = struct.Struct('<I')
HDR = 256
NODE = 48
FRAME = 24
BLOCK = 16
VISUAL = 8
LAYER = 24
VAR = 8
COND_SIZE = 12
EFFECT = 12
EDGE = 24
CHAPTER = 8
ENDING = 12
GALLERY = 16
IMG_HDR = 128
IMG_REC = 32
FMT_VERSION = 2
MAGIC_S = b'NINESC1\0'
MAGIC_I = b'NINEIM1\0'
CMP = {'==': 1, '!=': 2, '<': 3, '<=': 4, '>': 5, '>=': 6}


ART_W=256
ART_H=144
ART_SCALE=16/27

def u32(b, off):
    return U32.unpack_from(b, off)[0]


def crc32(b):
    return zlib.crc32(b) & 0xffffffff


def js_graph(path):
    text = path.read_text('utf-8')
    return json.loads(text[text.index('{'):text.rindex('}') + 1])


class Pack:
    def __init__(self, blob, image_blob):
        self.b = blob
        self.images = image_blob
        if blob[:8] != MAGIC_S or u32(blob, 8) != FMT_VERSION or u32(blob, 12) != len(blob):
            raise ValueError('script header')
        zero = blob[:16] + b'\0\0\0\0' + blob[20:]
        if crc32(zero) != u32(blob, 16):
            raise ValueError('script crc')
        self.strings_off, self.strings_size = u32(blob, 132), u32(blob, 136)
        self.episode = self.string(u32(blob, 20))
        self.title = self.string(u32(blob, 24))
        self.source = self.string(u32(blob, 28))
        self.start = u32(blob, 32)
        self.tables = []
        for i in range(12):
            count, off = u32(blob, 36 + i * 8), u32(blob, 40 + i * 8)
            self.tables.append((count, off))
        if image_blob[:8] != MAGIC_I or u32(image_blob, 8) != FMT_VERSION or \
                u32(image_blob, 12) != len(image_blob):
            raise ValueError('image header')
        z = image_blob[:16] + b'\0\0\0\0' + image_blob[20:]
        if crc32(z) != u32(image_blob, 16):
            raise ValueError('image crc')
        self.image_count = u32(image_blob, 20)
        self.image_off = u32(image_blob, 24)
        self.op_count = u32(image_blob, 28)
        self.op_off = u32(image_blob, 32)
        self.audio_count = u32(image_blob, 36)
        self.audio_off = u32(image_blob, 40)
        self.title_images = [u32(image_blob, 44 + i * 4) for i in range(3)]

    def string(self, off):
        if off >= self.strings_size:
            raise ValueError('string offset')
        start = self.strings_off + off
        stop = self.b.index(b'\0', start, self.strings_off + self.strings_size)
        return self.b[start:stop].decode('utf-8')

    def rows(self, index, stride):
        count, off = self.tables[index]
        for i in range(count):
            yield struct.unpack_from('<%dI' % (stride // 4), self.b, off + i * stride)

    def node(self, i):
        r = list(self.rows(0, NODE))[i]
        return dict(first_frame=r[0], frame_count=r[1], first_effect=r[2], effect_count=r[3],
                    first_next=r[4], next_count=r[5], first_choice=r[6], choice_count=r[7],
                    title=r[8], chapter=r[9], name=r[10], source=r[11])

    def frame(self, i):
        r = list(self.rows(1, FRAME))[i]
        return dict(kind=r[0], visual=r[1], block=r[2], off=r[3], length=r[4], speaker=r[5])

    def block_text(self, block, off, length):
        r = list(self.rows(2, BLOCK))[block]
        raw = zlib.decompress(self.b[r[0]:r[0] + r[1]])
        if len(raw) != r[2] or crc32(raw) != r[3]:
            raise ValueError('block crc')
        text = raw[off:off + length]
        if raw[off + length] != 0:
            raise ValueError('missing NUL')
        return text.decode('utf-8')

    def visual(self, i):
        r = list(self.rows(3, VISUAL))[i]
        return r[0], r[1]

    def layer(self, i):
        r = list(self.rows(4, LAYER))[i]
        return dict(image=r[0], x=r[1] - (1 << 32) * (r[1] >> 31), y=r[2] - (1 << 32) * (r[2] >> 31),
                    w=r[3], h=r[4], opacity=r[5])

    def var(self, i):
        r = list(self.rows(5, VAR))[i]
        return self.string(r[0]), r[1]

    def cond(self, i):
        r = list(self.rows(6, COND_SIZE))[i]
        return r[0], r[1], r[2] - (1 << 32) * (r[2] >> 31)

    def effect(self, i):
        r = list(self.rows(7, EFFECT))[i]
        return r[0], r[1], r[2] - (1 << 32) * (r[2] >> 31)

    def edge(self, i):
        r = list(self.rows(8, EDGE))[i]
        return dict(to=None if r[0] == NONE else r[0], first_cond=r[1], cond_count=r[2],
                    first_eff=r[3], eff_count=r[4], label=self.string(r[5]))

    def chapter(self, i):
        r = list(self.rows(9, CHAPTER))[i]
        return self.string(r[0]), r[1]

    def ending(self, i):
        r = list(self.rows(10, ENDING))[i]
        return self.string(r[0]), r[1], self.string(r[2])

    def gallery(self, i):
        r = list(self.rows(11, GALLERY))[i]
        return dict(name=self.string(r[0]), image=r[1], group=r[2], thumb=r[3])

    def image(self, i):
        rec = struct.unpack_from('<8I', self.images, self.image_off + i * IMG_REC)
        return dict(kind=rec[0], w=rec[1], h=rec[2], off=rec[3], size=rec[4], crc=rec[5])

    def image_payload(self, i):
        a = self.image(i)
        payload = self.images[a['off']:a['off'] + a['size']]
        if crc32(payload) != a['crc']:
            raise ValueError('image crc %d' % i)
        if a['kind'] == 0 and (payload[:2] != b'\xff\xd8' or payload[-2:] != b'\xff\xd9'):
            raise ValueError('jpeg markers %d' % i)
        if a['kind'] == 2 and a['size'] != a['w'] * a['h'] * 2:
            raise ValueError('raw size %d' % i)
        if a['kind'] == 1:
            pal, rows, blocks = struct.unpack_from('<3I', payload, 0)
            if pal > 256 or rows == 0 or blocks != (a['h'] + rows - 1) // rows:
                raise ValueError('indexed header %d' % i)
            table = 12 + pal * 4
            for k in range(blocks):
                off, ln = struct.unpack_from('<2I', payload, table + k * 8)
                if not (a['off'] <= off and off + ln <= a['off'] + a['size']):
                    raise ValueError('indexed range %d/%d' % (i, k))
                raw = zlib.decompress(self.images[off:off + ln])
                if len(raw) != a['w'] * min(rows, a['h'] - k * rows):
                    raise ValueError('indexed block %d/%d' % (i, k))
        return payload


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--episode', default='kokoiro')
    ap.add_argument('--pack-dir', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--main', type=Path, default=Path('main'))
    a = ap.parse_args()
    root = a.source / ('nine-%s-rw5' % a.episode)
    graph = js_graph(root / 'src/common/graph.js')
    blob = (a.pack_dir / 'nine_script.bin').read_bytes()
    images = (a.pack_dir / 'nine_images.bin').read_bytes()
    pack = Pack(blob, images)
    chars = set()
    report = {'sourceCommit': graph.get('sourceCommit'), 'scriptBytes': len(blob),
              'imageBytes': len(images), 'scriptSha256': hashlib.sha256(blob).hexdigest(),
              'imageSha256': hashlib.sha256(images).hexdigest(), 'checks': {}, 'errors': []}
    err = report['errors']

    def check(name, cond, detail=''):
        report['checks'][name] = report['checks'].get(name, 0) + 1
        if not cond:
            err.append('%s: %s' % (name, detail))

    node_ids = list(graph['nodes'])
    node_index = {n: i for i, n in enumerate(node_ids)}
    check('start', pack.start == node_index[graph['start']], graph['start'])
    check('nodeCount', pack.tables[0][0] == len(node_ids), len(node_ids))
    check('frameCount', pack.tables[1][0] == sum(n['count'] for n in graph['nodes'].values()),
          pack.tables[1][0])
    check('chapterCount', pack.tables[9][0] == len(graph['chapters']))
    check('endingCount', pack.tables[10][0] == len(graph['endings']))
    check('title', pack.title == json.loads((root / 'src/manifest.json').read_text('utf-8'))['name'],
          pack.title)
    check('sourceCommit', pack.source == '6a8907a5ae576f4377abe4b74f15bd9af13efe07', pack.source)

    # Variable dictionary: every key used by source effects/conditions must exist.
    var_names = [pack.var(i)[0] for i in range(pack.tables[5][0])]
    var_index = {n: i for i, n in enumerate(var_names)}
    key_names = set()

    def cond_tokens(w):
        out = []
        if not w:
            return out
        if w[0] == 'all':
            for sub in w[1:]:
                out.extend(cond_tokens(sub))
            out.append((7, 0, len(w) - 1))
        else:
            out.append((CMP[w[1]], var_index[w[0]], int(w[2])))
            key_names.add(w[0])
        return out

    def effect_tokens(es):
        out = []
        for k, op, v in es:
            out.append((0 if op == '=' else 1, var_index[k], int(v)))
            key_names.add(k)
        return out

    def frame_pointer(n):
        return n['first_frame'], n['frame_count']

    # Per-node text, speaker, visual and choices/nexts.
    for ni, nid in enumerate(node_ids):
        n = graph['nodes'][nid]
        pn = pack.node(ni)
        check('node.name', pack.string(pn['name']) == nid, nid)
        check('node.count', pn['frame_count'] == n['count'], nid)
        first = pn['first_frame']
        local = 0
        for chunk in n.get('chunks', []):
            rows = json.loads((root / 'src/common/story' / chunk['file'].lstrip('/')).read_text('utf-8'))
            for row in rows:
                pf = pack.frame(first + local)
                if row.get('kind') == 'op':
                    check('frame.op', pf['kind'] == 1, '%s/%d' % (nid, local))
                else:
                    check('frame.kind', pf['kind'] == 0, '%s/%d' % (nid, local))
                    text = pack.block_text(pf['block'], pf['off'], pf['length'])
                    check('frame.text', text == row.get('t', ''), '%s/%d' % (nid, local))
                    chars.update(text)
                    sp = row.get('s', '')
                    check('frame.speaker', pack.string(pf['speaker']) == sp, '%s/%d' % (nid, local))
                    chars.update(sp)
                local += 1
        check('node.textFrames', local == n['count'], nid)
        # Node effects: source effects then completion effects (the packer folds
        # complete[] into effects in that order).
        want_eff = effect_tokens(n.get('effects', []))
        for end in n.get('complete', []):
            want_eff.append((0, var_index[end], 1))
            if end == 'loop1' and a.episode == 'kokoiro':
                want_eff.append((0, var_index['loop_my'], 1))
            if end == 'true':
                want_eff.append((0, var_index['clear_true'], 1))
        got_eff = [pack.effect(pn['first_effect'] + i) for i in range(pn['effect_count'])]
        check('node.effects', got_eff == want_eff, '%s %s != %s' % (nid, got_eff, want_eff))

        def check_edges(edges, first, count, tag):
            for i, e in enumerate(edges):
                pe = pack.edge(first + i)
                target = None if e['to'] == '@end' else node_index[e['to']]
                check('edge.to', pe['to'] == target, '%s %s/%d' % (nid, tag, i))
                want = cond_tokens(e.get('when'))
                got = [pack.cond(pe['first_cond'] + k) for k in range(pe['cond_count'])]
                check('edge.cond', got == want, '%s %s/%d %s != %s' % (nid, tag, i, got, want))
                we = effect_tokens(e.get('effects', []))
                ge = [pack.effect(pe['first_eff'] + k) for k in range(pe['eff_count'])]
                check('edge.effect', ge == we, '%s %s/%d' % (nid, tag, i))
                check('edge.label', pe['label'] == e.get('text', ''), '%s %s/%d' % (nid, tag, i))
                chars.update(e.get('text', ''))
        check_edges(n.get('nexts', []), pn['first_next'], pn['next_count'], 'next')
        check_edges(n.get('choices', []), pn['first_choice'], pn['choice_count'], 'choice')

    check('variableSet', set(var_names) <= set(key_names) | {'titleBg', 'clear_true', 'loop1', 'true', 'loop_my'},
          sorted(set(var_names) - key_names))

    # Chapters and endings.
    for i, ch in enumerate(graph['chapters']):
        title, node = pack.chapter(i)
        check('chapter.title', title == ch.get('title', ch.get('name', '')), ch['id'])
        check('chapter.node', node == node_index[ch['node']], ch['id'])
        chars.update(ch.get('title', ch.get('name', '')))
    for i, e in enumerate(graph['endings']):
        name, key, desc = pack.ending(i)
        check('ending.name', name == e['name'], e['id'])
        check('ending.desc', desc == e.get('description', ''), e['id'])
        check('ending.key', var_names[key] == e['id'], e['id'])
        chars.update(e['name']); chars.update(e.get('description', ''))

    # Gallery: groups/variants and image payload integrity.
    gallery = json.loads((root / 'converted/gallery.json').read_text('utf-8'))
    want = [(v['name'], gi) for gi, g in enumerate(gallery) for v in g['variants']]
    got = [(pack.gallery(i)['name'], pack.gallery(i)['group']) for i in range(pack.tables[11][0])]
    check('gallery.count', len(got) == len(want), '%d != %d' % (len(got), len(want)))
    check('gallery.records', got == want, 'group/name order')
    for name, _ in want: chars.update(name)
    for i in range(pack.image_count):
        pack.image_payload(i)
    check('image.payloads', True)
    check('gallery.thumbs', all(pack.image(pack.gallery(i)['thumb'])['w'] == 96 and
                                pack.image(pack.gallery(i)['thumb'])['h'] == 51
                                for i in range(pack.tables[11][0])))
    for i in range(3):
        t = pack.title_images[i]
        check('title.image', pack.image(t)['kind'] == 2 and pack.image(t)['w'] == 320 and
              pack.image(t)['h'] == 240, t)

    # Visual layer audit: rebuild the expected layers from the source stage and
    # compare image identity, position, size and opacity for every text frame.
    pack_report = json.loads((a.pack_dir / 'nine_pack_report.json').read_text('utf-8'))
    image_meta = {}
    for m in pack_report['imageSources']:
        key = (m['source'], tuple(m['size']), tuple(m['crop']) if m.get('crop') else None,
               bool(m.get('sprite')), bool(m.get('raw')))
        image_meta[key] = m['id']

    def image_id(path, size, crop=None, sprite=False):
        return image_meta.get((path, tuple(size), tuple(crop) if crop else None, sprite, False))

    def expected_layers(bg, sprites):
        out = []
        if bg:
            out.append(dict(image=image_id(bg, (ART_W, ART_H)), x=0, y=0, w=ART_W, h=ART_H, opacity=255))
        for sp in sprites or []:
            if isinstance(sp, list):
                src, width, left = sp[:3]
                opacity = sp[3] if len(sp) > 3 and sp[3] is not None else 1
                patch = sp[4] if len(sp) > 4 else None
                height = sp[13] if len(sp) > 13 and sp[13] else 243
                top = 4 if height >= 200 else 243 - height
                meta = dict(src=src, width=width, left=left, height=height, top=top,
                            opacity=opacity, patch=patch)
                if patch:
                    for name, value in zip(('patchWidth', 'patchHeight', 'patchLeft', 'patchTop',
                                            'patchAtlasWidth', 'patchAtlasHeight',
                                            'patchImageLeft', 'patchImageTop'), sp[5:13]):
                        meta[name] = value
            else:
                meta = dict(sp)
            w = max(1, round(meta['width'] * ART_SCALE))
            h = max(1, round(meta.get('height', 243) * ART_SCALE))
            x = round(meta['left'] * ART_SCALE)
            y = round(meta.get('top', 4) * ART_SCALE)
            alpha = round(meta.get('opacity', 1) * 255)
            out.append(dict(image=image_id(meta['src'], (w, h), sprite=True), x=x, y=y,
                            w=w, h=h, opacity=alpha))
            if meta.get('patch'):
                from PIL import Image
                with Image.open(root / 'src' / meta['patch'].lstrip('/')) as im:
                    iw, ih = im.size
                ax, ay = meta['patchAtlasWidth'], meta['patchAtlasHeight']
                px, py = -meta['patchImageLeft'], -meta['patchImageTop']
                pw, ph = meta['patchWidth'], meta['patchHeight']
                crop = (round(px * iw / ax), round(py * ih / ay),
                        round((px + pw) * iw / ax), round((py + ph) * ih / ay))
                ps = (max(1, round(pw * ART_SCALE)), max(1, round(ph * ART_SCALE)))
                out.append(dict(image=image_id(meta['patch'], ps, crop=crop, sprite=True),
                                x=round(meta['patchLeft'] * ART_SCALE),
                                y=round(meta['patchTop'] * ART_SCALE),
                                w=ps[0], h=ps[1], opacity=alpha))
        return out

    report['visuals'] = 0
    visuals_checked = 0
    layer_total = 0
    for ni, nid in enumerate(node_ids):
        n = graph['nodes'][nid]
        first = pack.node(ni)['first_frame']
        local = 0
        for chunk in n.get('chunks', []):
            rows = json.loads((root / 'src/common/story' / chunk['file'].lstrip('/')).read_text('utf-8'))
            cur_bg, cur_sprites = '', []
            for row in rows:
                if row.get('kind') != 'op':
                    pf = pack.frame(first + local)
                    if 'art' in row:
                        cur_bg = row['art'].get('background', '')
                        cur_sprites = row['art'].get('sprites', [])
                    else:
                        cur_bg = row.get('b', cur_bg)
                        cur_sprites = row.get('p', cur_sprites)
                    vfirst, vcount = pack.visual(pf['visual'])
                    layers = [pack.layer(vfirst + k) for k in range(vcount)]
                    want_layers = expected_layers(cur_bg, cur_sprites)
                    check('visual.limit', 0 <= vcount <= 32, '%s/%d' % (nid, local))
                    check('visual.layers', layers == want_layers,
                          '%s/%d %s != %s' % (nid, local, layers, want_layers))
                    for l in layers:
                        check('layer.size', 0 < l['w'] <= 4096 and 0 < l['h'] <= 4096)
                        check('layer.opacity', 0 <= l['opacity'] <= 255)
                    visuals_checked += 1
                    layer_total += vcount
                local += 1
    report['visuals'] = visuals_checked
    report['layers'] = layer_total
    report['variables'] = var_names
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from gal_font import source_chars
    chars |= source_chars(str(a.main))
    chars -= set(map(chr, (13, 10, 9)))
    symbols = set((a.pack_dir / 'nine_symbols.txt').read_text('utf-8'))
    missing = sorted(chars - symbols)
    check('font.symbolCoverage', not missing, ''.join(missing[:20]))
    font_report = json.loads((a.pack_dir / 'nine_font_report.json').read_text('utf-8'))
    check('font.glyphCount', font_report['glyphs'] == len(symbols), font_report['glyphs'])
    check('font.missingZero', all(v['missingGlyphs'] == 0 and v['verifiedGlyphs'] == font_report['glyphs']
                                  for v in font_report['sizes'].values()))
    ascii_needed = set(chr(c) for c in range(32, 127))
    check('font.ascii', ascii_needed <= symbols, ''.join(sorted(ascii_needed - symbols)))
    report['characters'] = len(chars)
    report['symbols'] = len(symbols)
    report['pass'] = not err
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('pass', 'checks', 'errors', 'scriptBytes',
                                            'imageBytes', 'scriptSha256', 'imageSha256',
                                            'visuals', 'layers')}, ensure_ascii=False))
    return 0 if not err else 1


if __name__ == '__main__':
    sys.exit(main())
