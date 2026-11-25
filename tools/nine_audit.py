#!/usr/bin/env python3
"""Source-to-pack audit for the 9-nine- Episode 4 reader.

Reads the pinned upstream graph/chunks and the generated pack, then compares
every node, frame, speaker, condition, effect, edge, chapter, ending and
gallery reference. Image layers are checked against the source art geometry and
the packer's image-source table. Nothing is written except the report path.

python tools/nine_audit.py --source <9-nine-rw5> --episode yukiiro --pack assets/nine/yukiiro
"""
from __future__ import annotations
import argparse, hashlib, json, struct, sys, zlib
from pathlib import Path
from PIL import Image

NONE = 0xffffffff
EPISODES = ('kokoiro', 'sorairo', 'haruiro', 'yukiiro')
COND = {'==': 1, '!=': 2, '<': 3, '<=': 4, '>': 5, '>=': 6}


def rd32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def utf8(b, off):
    end = b.find(b'\0', off)
    if end < 0:
        raise ValueError('unterminated string')
    return b[off:end].decode('utf-8')


class Pack:
    def __init__(self, data):
        self.b = data
        if data[:8] != b'NINESC1\0':
            raise ValueError('bad script magic')
        self.version, self.size, self.crc = struct.unpack_from('<III', data, 8)
        if self.version != 1 or self.size != len(data):
            raise ValueError('bad script header size/version')
        crc = zlib.crc32(data[:16] + b'\0\0\0\0' + data[20:]) & NONE
        if crc != self.crc:
            raise ValueError('bad script CRC')
        self.start = rd32(data, 32)
        self.tables = {}
        for name, i in (('node', 0), ('frame', 1), ('block', 2), ('visual', 3),
                        ('layer', 4), ('var', 5), ('cond', 6), ('effect', 7),
                        ('edge', 8), ('chapter', 9), ('ending', 10), ('gallery', 11)):
            self.tables[name] = (rd32(data, 36 + i * 8), rd32(data, 40 + i * 8))
        self.strings = (rd32(data, 132), rd32(data, 136))
        self.episode = self.string(rd32(data, 20))
        self.title = self.string(rd32(data, 24))
        self.source = self.string(rd32(data, 28))
        self.block_cache = {}
        self.image = None

    def row(self, table, index, stride):
        count, off = self.tables[table]
        if index >= count:
            raise IndexError(table)
        return self.b[off + index * stride: off + (index + 1) * stride]

    def count(self, table):
        return self.tables[table][0]

    def string(self, off):
        return utf8(self.b, self.strings[0] + off)

    def node(self, i):
        r = self.row('node', i, 48)
        v = struct.unpack('<12I', r)
        return {'first_frame': v[0], 'count': v[1], 'first_effect': v[2],
                'effect_count': v[3], 'first_next': v[4], 'next_count': v[5],
                'first_choice': v[6], 'choice_count': v[7], 'title': self.string(v[8]),
                'chapter': v[9], 'name': self.string(v[10]), 'source': v[11]}

    def frame(self, i):
        r = self.row('frame', i, 24)
        v = struct.unpack('<6I', r)
        return {'kind': v[0], 'visual': v[1], 'block': v[2], 'text_offset': v[3],
                'text_length': v[4], 'speaker': self.string(v[5])}

    def text(self, f):
        if f['kind'] != 0:
            return None
        off, comp, raw, crc = struct.unpack('<4I', self.row('block', f['block'], 16))
        if f['block'] not in self.block_cache:
            data = zlib.decompress(self.b[off:off + comp])
            if len(data) != raw or zlib.crc32(data) & NONE != crc:
                raise ValueError('block CRC/size mismatch')
            self.block_cache[f['block']] = data
        data = self.block_cache[f['block']]
        end = f['text_offset'] + f['text_length']
        if end >= len(data) or data[end] != 0:
            raise ValueError('text not NUL terminated')
        return data[f['text_offset']:end].decode('utf-8')

    def visual(self, i):
        first, n = struct.unpack('<2I', self.row('visual', i, 8))
        layers = []
        for k in range(n):
            v = struct.unpack('<IiiIII', self.row('layer', first + k, 24))
            layers.append({'image': v[0], 'x': v[1], 'y': v[2], 'w': v[3],
                           'h': v[4], 'opacity': v[5]})
        return layers

    def cond(self, i):
        op, key, val = struct.unpack('<IIi', self.row('cond', i, 12))
        return op, key, val

    def effect(self, i):
        op, key, val = struct.unpack('<IIi', self.row('effect', i, 12))
        return op, key, val

    def edge(self, i):
        v = struct.unpack('<6I', self.row('edge', i, 24))
        return {'to': v[0], 'first_cond': v[1], 'cond_count': v[2],
                'first_effect': v[3], 'effect_count': v[4], 'label': self.string(v[5])}

    def var(self, i):
        name, flags = struct.unpack('<2I', self.row('var', i, 8))
        return self.string(name), flags

    def chapter(self, i):
        title, node = struct.unpack('<2I', self.row('chapter', i, 8))
        return self.string(title), node

    def ending(self, i):
        name, key, desc = struct.unpack('<3I', self.row('ending', i, 12))
        return self.string(name), key, self.string(desc)

    def gallery(self, i):
        name, image, group = struct.unpack('<3I', self.row('gallery', i, 12))
        return self.string(name), image, group


class Images:
    def __init__(self, data):
        if data[:8] != b'NINEIM1\0':
            raise ValueError('bad image magic')
        if rd32(data, 8) != 1 or rd32(data, 12) != len(data):
            raise ValueError('bad image header')
        crc = zlib.crc32(data[:16] + b'\0\0\0\0' + data[20:]) & NONE
        if crc != rd32(data, 16):
            raise ValueError('bad image CRC')
        self.b = data
        self.count = rd32(data, 20)
        self.table = rd32(data, 24)
        self.op_count = rd32(data, 28)
        self.op_table = rd32(data, 32)
        self.audio_count = rd32(data, 36)
        self.audio_table = rd32(data, 40)
        self.titles = [rd32(data, 44 + i * 4) for i in range(3)]
        self.logo = rd32(data, 56)

    def asset(self, i):
        if i >= self.count:
            raise IndexError('image')
        kind, w, h, off, size, crc, _, _ = struct.unpack_from('<8I', self.b, self.table + i * 32)
        if off + size > len(self.b):
            raise ValueError('image out of range')
        if zlib.crc32(self.b[off:off + size]) & NONE != crc:
            raise ValueError('image CRC mismatch')
        return kind, w, h, off, size

    def decode(self, i):
        kind, w, h, off, size = self.asset(i)
        if kind == 0:
            from io import BytesIO
            with Image.open(BytesIO(self.b[off:off + size])) as im:
                im.load()
                if im.size != (w, h):
                    raise ValueError('JPEG dimension mismatch')
                return im.convert('RGB')
        p = off
        pal_count, rows, blocks = struct.unpack_from('<3I', self.b, p)
        pal = self.b[p + 12:p + 12 + pal_count * 4]
        table = p + 12 + pal_count * 4
        raw = bytearray(w * h)
        for blk in range(blocks):
            boff, blen = struct.unpack_from('<2I', self.b, table + blk * 8)
            sy = blk * rows
            nrows = min(rows, h - sy)
            chunk = zlib.decompress(self.b[boff:boff + blen])
            if len(chunk) != w * nrows:
                raise ValueError('indexed block size mismatch')
            raw[sy * w:(sy + nrows) * w] = chunk
        rgba = Image.frombytes('P', (w, h), bytes(raw))
        rgba.putpalette(pal)
        return rgba.convert('RGBA')


def js_graph(path):
    text = path.read_text('utf-8')
    return json.loads(text.split('=', 1)[1].strip().rstrip(';'))


def expand_sprite(s, i):
    if not isinstance(s, list):
        d = dict(s)
        d['id'] = d.get('id') or (d.get('src', '') + d.get('patch', '') + str(d.get('left')) + ':' + str(i))
        d['opacity'] = 1 if d.get('opacity') is None else d['opacity']
        return d
    src, width, left = s[0], s[1], s[2]
    opacity = 1 if len(s) < 4 or s[3] is None else s[3]
    patch = s[4] if len(s) > 4 else ''
    height = s[13] if len(s) > 13 and s[13] else 243
    top = 4 if height >= 200 else 243 - height
    d = {'id': src + (patch or '') + str(left) + ':' + str(i), 'src': src,
         'width': width, 'height': height, 'left': left, 'top': top,
         'opacity': opacity}
    if patch:
        d.update({'patch': patch, 'patchWidth': s[5], 'patchHeight': s[6],
                  'patchLeft': s[7], 'patchTop': s[8], 'patchAtlasWidth': s[9],
                  'patchAtlasHeight': s[10], 'patchImageLeft': s[11],
                  'patchImageTop': s[12]})
    return d


def decode_chunk(raw):
    out = []
    cur_bg = ''
    cur_sprites = []
    for f in raw:
        if not f or f.get('kind') == 'op':
            out.append(f)
            continue
        if 'art' in f:
            bg = f['art'].get('background', '') or ''
            sp = f['art'].get('sprites') or []
            out.append({'speaker': f.get('speaker', ''), 'text': f.get('text', ''),
                        'art': {'background': bg, 'sprites': [expand_sprite(x, i) for i, x in enumerate(sp)]}})
            cur_bg, cur_sprites = bg, out[-1]['art']['sprites']
            continue
        if 'b' in f:
            cur_bg = f['b']
        if 'p' in f:
            cur_sprites = [expand_sprite(x, i) for i, x in enumerate(f['p'])]
        out.append({'speaker': f.get('s', ''), 'text': f.get('t', ''),
                    'art': {'background': cur_bg, 'sprites': cur_sprites}})
    return out


def expected_layers(art, report_sources, patch_sizes=None):
    layers = []
    if patch_sizes is None:
        patch_sizes = {}
    if art['background']:
        layers.append({'src': art['background'], 'size': [320, 180], 'crop': None,
                       'x': 0, 'y': 0, 'opacity': 255})
    for d in art['sprites']:
        w = max(1, round(d['width'] * 320 / 432))
        h = max(1, round(d.get('height', 243) * 180 / 243))
        x = round(d['left'] * 320 / 432)
        y = round(d.get('top', 4) * 180 / 243)
        a = round(d.get('opacity', 1) * 255)
        layers.append({'src': d['src'], 'size': [w, h], 'crop': None,
                       'x': x, 'y': y, 'opacity': a})
        if d.get('patch'):
            if d['patch'] not in patch_sizes:
                with Image.open(report_sources['root'] / d['patch'].lstrip('/')) as im:
                    patch_sizes[d['patch']] = im.size
            iw, ih = patch_sizes[d['patch']]
            ax, ay = d['patchAtlasWidth'], d['patchAtlasHeight']
            px, py = -d['patchImageLeft'], -d['patchImageTop']
            pw, ph = d['patchWidth'], d['patchHeight']
            crop = (round(px * iw / ax), round(py * ih / ay),
                    round((px + pw) * iw / ax), round((py + ph) * ih / ay))
            ps = [max(1, round(pw * 320 / 432)), max(1, round(ph * 180 / 243))]
            layers.append({'src': d['patch'], 'size': ps, 'crop': list(crop),
                           'x': round(d['patchLeft'] * 320 / 432),
                           'y': round(d['patchTop'] * 180 / 243),
                           'opacity': a})
    return layers


def when_tokens(w, keys):
    out = []
    if not w:
        return out
    if w[0] == 'all':
        for x in w[1:]:
            out.extend(when_tokens(x, keys))
        out.append((7, 0, len(w) - 1))
    else:
        out.append((COND[w[1]], keys[w[0]], int(w[2])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--episode', choices=EPISODES, required=True)
    ap.add_argument('--pack', type=Path, required=True)
    ap.add_argument('--report', type=Path, default=None)
    args = ap.parse_args()

    src = args.source / f'nine-{args.episode}-rw5'
    raw_graph = js_graph(src / 'src/common/graph.js')
    pack = Pack((args.pack / 'nine_script.bin').read_bytes())
    images = Images((args.pack / 'nine_images.bin').read_bytes())
    report = json.loads((args.pack / 'nine_pack_report.json').read_text('utf-8'))
    report_sources = {'root': src / 'src'}
    errors = []
    notes = []

    def check(cond, msg):
        if not cond:
            errors.append(msg)

    check(pack.episode == args.episode, 'episode string')
    check(pack.source == '6a8907a5ae576f4377abe4b74f15bd9af13efe07', 'source commit binding')
    check(pack.title == json.loads((src / 'src/manifest.json').read_text('utf-8'))['name'],
          'title binding')
    check(pack.count('node') == len(raw_graph['nodes']), 'node count')
    check(pack.start == list(raw_graph['nodes']).index(raw_graph['start']), 'start node')
    check(images.count == report['images'], 'image count vs report')
    check(images.op_count == report['opening']['outputFrames'], 'op frame count vs report')

    var_names = [(pack.var(i)[0], pack.var(i)[1]) for i in range(pack.count('var'))]
    keys = {name: i for i, (name, _) in enumerate(var_names)}
    check(len(var_names) == 78, f'variable count {len(var_names)} != 78')
    persistent = {n for n, f in var_names if f & 1}
    for name in ('loop1', 'clear_true', 'titleBg', 'loop_my', 'loop_sr', 'cf_sr',
                 'loop_hk', 'cf_hk', 'loop_na', 'cf_na'):
        check(name in persistent, f'persistent key missing: {name}')

    node_ids = list(raw_graph['nodes'])
    index = {n: i for i, n in enumerate(node_ids)}
    chapters = raw_graph['chapters']
    chapter_names = {c.get('title', c.get('name', '')): i for i, c in enumerate(chapters)}
    check(pack.count('chapter') == len(chapters) == 23, 'chapter count')
    for i, c in enumerate(chapters):
        title, node = pack.chapter(i)
        check(title == c.get('title', c.get('name', '')), f'chapter {i} title')
        check(node == index[c['node']], f'chapter {i} node')
    check(pack.count('ending') == len(raw_graph['endings']) == 2, 'ending count')
    for i, e in enumerate(raw_graph['endings']):
        name, key, desc = pack.ending(i)
        check(name == e['name'], f'ending {i} name')
        check(key == keys[e['id']], f'ending {i} key')
        check(desc == e.get('description', ''), f'ending {i} description')

    gallery = json.loads((src / 'converted/gallery.json').read_text('utf-8'))
    expected_gallery = [(v['name'], gidx, v['src']) for gidx, g in enumerate(gallery) for v in g['variants']]
    check(pack.count('gallery') == len(expected_gallery) == 108, 'gallery count')
    for i, (name, gidx, path) in enumerate(expected_gallery):
        gname, image, group = pack.gallery(i)
        check(gname == name, f'gallery {i} name')
        check(group == gidx, f'gallery {i} group')
        srcmeta = report['imageSources'][image]
        check(srcmeta['source'] == path, f'gallery {i} image source')
        check(images.asset(image)[:3] == (0, 320, 180), f'gallery {i} not 320x180 JPEG')

    # Every source image reference exists and every pack image maps back to one.
    source_images = set()
    patch_sizes = {}
    frame_count = 0
    cond_index = 0
    for nid in node_ids:
        n = raw_graph['nodes'][nid]
        got = pack.node(index[nid])
        check(got['name'] == nid, f'{nid}: node id')
        check(got['count'] == n['count'], f'{nid}: frame count')
        expected_frames = []
        for chunk in n.get('chunks', []):
            rows = json.loads((src / 'src/common/story' / chunk['file'].lstrip('/')).read_text('utf-8'))
            expected_frames.extend(decode_chunk(rows))
        check(len(expected_frames) == n['count'], f'{nid}: chunk rows')
        for k, ef in enumerate(expected_frames):
            frame_count += 1
            pf = pack.frame(got['first_frame'] + k)
            if not ef or ef.get('kind') == 'op':
                check(pf['kind'] == 1, f'{nid}/{k}: OP kind')
                continue
            check(pf['kind'] == 0, f'{nid}/{k}: text kind')
            check(pack.text(pf) == ef['text'], f'{nid}/{k}: text')
            check(pf['speaker'] == ef['speaker'], f'{nid}/{k}: speaker')
            exp = expected_layers(ef['art'], report_sources, patch_sizes)
            got_layers = pack.visual(pf['visual'])
            check(len(got_layers) == len(exp), f'{nid}/{k}: layer count {len(got_layers)}/{len(exp)}')
            for j, (gl, el) in enumerate(zip(got_layers, exp)):
                gm = report['imageSources'][gl['image']]
                check(gm['source'] == el['src'], f'{nid}/{k}/{j}: source {gm["source"]} != {el["src"]}')
                check(gm['size'] == el['size'], f'{nid}/{k}/{j}: size {gm["size"]} != {el["size"]}')
                check(gm['crop'] == el['crop'], f'{nid}/{k}/{j}: crop {gm["crop"]} != {el["crop"]}')
                check((gl['x'], gl['y'], gl['w'], gl['h'], gl['opacity']) ==
                      (el['x'], el['y'], el['size'][0], el['size'][1], el['opacity']),
                      f'{nid}/{k}/{j}: geometry')
                source_images.add(el['src'])

        # Node effects: source effects then completion (packer appends).
        effects = []
        for k, op, val in n.get('effects', []):
            effects.append((0 if op == '=' else 1, keys[k], int(val)))
        for end in n.get('complete', []):
            effects.append((0, keys[end], 1))
            if end == 'loop1' and args.episode == 'kokoiro':
                effects.append((0, keys['loop_my'], 1))
            if end == 'true':
                effects.append((0, keys['clear_true'], 1))
        check(got['effect_count'] == len(effects), f'{nid}: effect count')
        for k, e in enumerate(effects):
            check(pack.effect(got['first_effect'] + k) == e, f'{nid}: effect {k}')

        conds = []
        edges = []
        for e in n.get('nexts', []):
            to = NONE if e['to'] == '@end' else index[e['to']]
            cf = len(conds)
            conds.extend(when_tokens(e.get('when'), keys))
            edges.append((to, cf, len(conds) - cf, 0, 0, e.get('text', '')))
        for e in n.get('choices', []):
            to = NONE if e['to'] == '@end' else index[e['to']]
            cf = len(conds)
            conds.extend(when_tokens(e.get('when'), keys))
            ef = 0
            # choice effects are compared below through the shared effect table
            edges.append((to, cf, len(conds) - cf, ef, 0, e.get('text', '')))
        check(got['next_count'] == len(n.get('nexts', [])), f'{nid}: next count')
        check(got['choice_count'] == len(n.get('choices', [])), f'{nid}: choice count')
        # Compare nexts and choices separately from one combined edge table.
        base = got['first_next']
        for k, e in enumerate(n.get('nexts', [])):
            pe = pack.edge(base + k)
            check(pe['to'] == (NONE if e['to'] == '@end' else index[e['to']]), f'{nid}: next {k} target')
            check(pe['label'] == e.get('text', ''), f'{nid}: next {k} label')
            exp_cond = when_tokens(e.get('when'), keys)
            check(pe['cond_count'] == len(exp_cond), f'{nid}: next {k} condition count')
            check(pe['first_cond'] == cond_index, f'{nid}: next {k} condition offset')
            for j, c in enumerate(exp_cond):
                check(pack.cond(pe['first_cond'] + j) == c, f'{nid}: next {k} condition {j}')
            cond_index += len(exp_cond)
        for k, e in enumerate(n.get('choices', [])):
            pe = pack.edge(got['first_choice'] + k)
            check(pe['to'] == (NONE if e['to'] == '@end' else index[e['to']]), f'{nid}: choice {k} target')
            check(pe['label'] == e.get('text', ''), f'{nid}: choice {k} label')
            exp_cond = when_tokens(e.get('when'), keys)
            check(pe['cond_count'] == len(exp_cond), f'{nid}: choice {k} condition count')
            check(pe['first_cond'] == cond_index, f'{nid}: choice {k} condition offset')
            for j, c in enumerate(exp_cond):
                check(pack.cond(pe['first_cond'] + j) == c, f'{nid}: choice {k} condition {j}')
            cond_index += len(exp_cond)
            effects = []
            for name, op, val in e.get('effects', []):
                effects.append((0 if op == '=' else 1, keys[name], int(val)))
            check(pe['effect_count'] == len(effects), f'{nid}: choice {k} effect count')
            for j, eff in enumerate(effects):
                check(pack.effect(pe['first_effect'] + j) == eff, f'{nid}: choice {k} effect {j}')

    check(frame_count == 12666, f'frame total {frame_count}')
    check(cond_index == pack.count('cond'), f'condition table coverage {cond_index}/{pack.count("cond")}')
    # Every pack image must be mapped by the report and decode.
    for i in range(images.count):
        kind, w, h, off, size = images.asset(i)
        check(0 <= i < len(report['imageSources']), f'image {i} report')
        im = images.decode(i)
        check(im.size == (w, h), f'image {i} decode size')
        check(w <= 320, f'image {i} width {w} exceeds band index width')
    # Audio blocks are structurally valid and CRC-covered.
    for a in range(images.audio_count):
        kind, name, off, size, dur, rate, ch, crc = struct.unpack_from('<8I', images.b, images.audio_table + a * 32)
        check(kind == 1 and rate == 8000 and ch == 1 and size % 132 == 0, f'audio {a} header')
        check(zlib.crc32(images.b[off:off + size]) & NONE == crc, f'audio {a} crc')
        check(dur > 0, f'audio {a} duration')
    # Title images and the logo are valid assets.
    for t in images.titles:
        kind, w, h, _, _ = images.asset(t)
        check(kind == 0 and (w, h) == (320, 180), 'title art asset')
    kind, w, h, _, _ = images.asset(images.logo)
    check(kind == 1 and w > 0 and h > 0, 'logo indexed asset')

    summary = {
        'source': str(src), 'episode': pack.episode, 'title': pack.title,
        'sourceCommit': pack.source, 'nodes': pack.count('node'),
        'frames': pack.count('frame'), 'textFrames': sum(1 for i in range(pack.count('frame')) if pack.frame(i)['kind'] == 0),
        'variables': len(var_names), 'chapters': pack.count('chapter'),
        'endings': pack.count('ending'), 'gallery': pack.count('gallery'),
        'images': images.count, 'opFrames': images.op_count,
        'audioTracks': images.audio_count, 'sourceImages': len(source_images),
        'sourceSha256': hashlib.sha256((args.pack / 'nine_script.bin').read_bytes()).hexdigest(),
        'imageSha256': hashlib.sha256((args.pack / 'nine_images.bin').read_bytes()).hexdigest(),
        'errors': errors[:50], 'errorCount': len(errors),
    }
    out = args.report or (args.pack / 'nine_audit.json')
    out.write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'errors'}, ensure_ascii=False))
    if errors:
        print('AUDIT FAILED:', len(errors), 'errors; first:', errors[0], file=sys.stderr)
        return 1
    print('AUDIT PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
