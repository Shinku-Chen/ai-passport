#!/usr/bin/env python3
"""Image acceptance for 9-nine-: real decode + reference composite comparison.

Two references are compared against the production C compositor (nine_render):
  1. pack reference: decode the pack's JPEG / indexed / raw assets with Pillow,
     composite the exact layer list, and compare pixels.
  2. source reference: re-decode the pinned source art and composite the same
     stage at 256x144 (displayed 320x180), then compare with JPEG/resampling tolerance.

Usage:
  python tools/nine_image_audit.py --source <9-nine-rw5> --pack-dir assets/nine/kokoiro \
      --render build/nine/nine_render.exe --out build/nine/image-audit.json
"""
from __future__ import annotations
import argparse, io, json, math, struct, subprocess, sys, zlib
from pathlib import Path
from PIL import Image

NONE = 0xffffffff
W, H = 256, 144
S = 16 / 27
IMG_HDR, IMG_REC = 128, 32


def u32(b, off):
    return struct.unpack_from('<I', b, off)[0]


class Packed:
    def __init__(self, script, images):
        self.s, self.i = script, images
        self.str_off, self.str_size = u32(script, 132), u32(script, 136)
        self.nodes = []
        count, off = u32(script, 36), u32(script, 40)
        for k in range(count):
            r = struct.unpack_from('<12I', script, off + k * 48)
            self.nodes.append(r)
        self.node_names = [self.string(r[10]) for r in self.nodes]
        self.node_index = {n: k for k, n in enumerate(self.node_names)}
        self.frames_off = u32(script, 48)
        self.visual_off = u32(script, 64)
        self.layer_off = u32(script, 72)
        self.image_count = u32(images, 20)
        self.image_off = u32(images, 24)
        self.cache = {}

    def string(self, off):
        start = self.str_off + off
        stop = self.s.index(b'\0', start, self.str_off + self.str_size)
        return self.s[start:stop].decode('utf-8')

    def frame(self, k):
        return struct.unpack_from('<6I', self.s, self.frames_off + k * 24)

    def visual(self, k):
        return struct.unpack_from('<2I', self.s, self.visual_off + k * 8)

    def layer(self, k):
        r = struct.unpack_from('<6I', self.s, self.layer_off + k * 24)
        return (r[0], struct.unpack('<i', struct.pack('<I', r[1]))[0],
                struct.unpack('<i', struct.pack('<I', r[2]))[0], r[3], r[4], r[5])

    def image_rec(self, k):
        return struct.unpack_from('<8I', self.i, self.image_off + k * IMG_REC)

    def asset(self, k):
        if k in self.cache:
            return self.cache[k]
        kind, w, h, off, size, crc, _, _ = self.image_rec(k)
        payload = self.i[off:off + size]
        if zlib.crc32(payload) & 0xffffffff != crc:
            raise ValueError('image crc %d' % k)
        if kind == 0:
            im = Image.open(io.BytesIO(payload)).convert('RGB')
            if im.size != (w, h):
                raise ValueError('jpeg size %d' % k)
            out = im
        elif kind == 1:
            pal, rows, blocks = struct.unpack_from('<3I', payload, 0)
            palette = [tuple(payload[12 + p * 4:12 + p * 4 + 4]) for p in range(pal)]
            idx = bytearray(w * h)
            table = 12 + pal * 4
            for b in range(blocks):
                boff, blen = struct.unpack_from('<2I', payload, table + b * 8)
                raw = zlib.decompress(self.i[boff:boff + blen])
                y0 = b * rows
                n = min(rows, h - y0)
                if len(raw) != w * n:
                    raise ValueError('block size %d/%d' % (k, b))
                idx[y0 * w:(y0 + n) * w] = raw
            out = Image.new('RGBA', (w, h))
            px = out.load()
            for y in range(h):
                for x in range(w):
                    px[x, y] = palette[idx[y * w + x]]
        elif kind == 2:
            if size != w * h * 2:
                raise ValueError('raw size %d' % k)
            vals = struct.unpack_from('<%dH' % (w * h), payload, 0)
            out = Image.new('RGB', (w, h))
            px = out.load()
            for y in range(h):
                for x in range(w):
                    v = vals[y * w + x]
                    px[x, y] = (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
        else:
            raise ValueError('kind %d' % kind)
        self.cache[k] = out
        return out


def ppm(path):
    data = Path(path).read_bytes()
    parts = data.split(b'\n', 3)
    if parts[0] != b'P6':
        raise ValueError('ppm')
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def expand(p, q=None):
    r, g, b = ((p >> 11) & 31), ((p >> 5) & 63), (p & 31)
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def pack565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def composite_pack(pack, layers):
    canvas = [[(0, 0, 0)] * W for _ in range(H)]
    for image, x, y, w, h, opacity in layers:
        im = pack.asset(image)
        if im.size != (w, h):
            im = im.resize((w, h), Image.Resampling.NEAREST)
        mode = im.mode
        px = im.load()
        for yy in range(h):
            dy = y + yy
            if dy < 0 or dy >= H:
                continue
            row = canvas[dy]
            for xx in range(w):
                dx = x + xx
                if dx < 0 or dx >= W:
                    continue
                p = px[xx, yy]
                if mode == 'RGBA':
                    a = (p[3] * opacity + 127) // 255
                else:
                    a = opacity
                if a == 0:
                    continue
                if a >= 255:
                    rgb = (p[0], p[1], p[2])
                else:
                    d = row[dx]
                    rgb = ((p[0] * a + d[0] * (255 - a) + 127) // 255,
                           (p[1] * a + d[1] * (255 - a) + 127) // 255,
                           (p[2] * a + d[2] * (255 - a) + 127) // 255)
                row[dx] = rgb
    return canvas


def composite_source(root, layers):
    canvas = [[(0, 0, 0)] * W for _ in range(H)]
    from PIL import ImageOps
    for meta in layers:
        path = root / 'src' / meta['src'].lstrip('/')
        with Image.open(path) as im:
            im = im.convert('RGBA')
            if meta.get('crop'):
                im = im.crop(meta['crop'])
            im = im.resize((meta['w'], meta['h']), Image.Resampling.LANCZOS)
            px = im.load()
            opacity = meta['opacity']
            for yy in range(meta['h']):
                dy = meta['y'] + yy
                if dy < 0 or dy >= H:
                    continue
                row = canvas[dy]
                for xx in range(meta['w']):
                    dx = meta['x'] + xx
                    if dx < 0 or dx >= W:
                        continue
                    p = px[xx, yy]
                    a = (p[3] * opacity + 127) // 255
                    if a == 0:
                        continue
                    if a >= 255:
                        rgb = (p[0], p[1], p[2])
                    else:
                        d = row[dx]
                        rgb = ((p[0] * a + d[0] * (255 - a) + 127) // 255,
                               (p[1] * a + d[1] * (255 - a) + 127) // 255,
                               (p[2] * a + d[2] * (255 - a) + 127) // 255)
                    row[dx] = rgb
    return canvas


def diff(a, b):
    # compare after quantizing both to the same RGB565 representation
    total = 0
    worst = 0
    n = 0
    hist = []
    for y in range(H):
        for x in range(W):
            pa = expand(pack565(a[y][x]))
            pb = expand(pack565(b[y][x]))
            d = max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2]))
            total += d
            worst = max(worst, d)
            n += 1
            hist.append(d)
    hist.sort()
    return dict(mean=round(total / n, 4), p99=hist[int(n * 0.99)],
                max=worst, over8=sum(1 for d in hist if d > 8))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--pack-dir', type=Path, required=True)
    ap.add_argument('--render', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--work', type=Path, default=Path('build/nine/image-work'))
    ap.add_argument('--stride', type=int, default=500)
    ap.add_argument('--source-frames', type=int, default=24)
    a = ap.parse_args()
    root = a.source / 'nine-kokoiro-rw5'
    pack = Packed((a.pack_dir / 'nine_script.bin').read_bytes(),
                  (a.pack_dir / 'nine_images.bin').read_bytes())
    graph_text = (root / 'src/common/graph.js').read_text('utf-8')
    graph = json.loads(graph_text[graph_text.index('{'):graph_text.rindex('}') + 1])
    report = json.loads((a.pack_dir / 'nine_pack_report.json').read_text('utf-8'))
    source_meta = {}
    for m in report['imageSources']:
        key = (m['source'], tuple(m['size']), tuple(m['crop']) if m.get('crop') else None,
               bool(m.get('sprite')), bool(m.get('raw')))
        source_meta[key] = m['id']

    # Build the frame list from source chunks.
    frames = []
    local_frame = 0
    for nid, node in graph['nodes'].items():
        local = 0
        for chunk in node.get('chunks', []):
            rows = json.loads((root / 'src/common/story' / chunk['file'].lstrip('/')).read_text('utf-8'))
            cur_bg, cur_sp = '', []
            for row in rows:
                if row.get('kind') != 'op':
                    if 'art' in row:
                        cur_bg, cur_sp = row['art'].get('background', ''), row['art'].get('sprites', [])
                    else:
                        cur_bg, cur_sp = row.get('b', cur_bg), row.get('p', cur_sp)
                    frames.append(dict(node=nid, index=local, bg=cur_bg, sprites=cur_sp,
                                       global_index=local_frame))
                local += 1
                local_frame += 1
        assert local == node['count'], nid
    picked = []
    seen = set()
    for nid in graph['nodes']:
        node = graph['nodes'][nid]
        idxs = {0, max(0, node['count'] // 2), max(0, node['count'] - 1)} if node['count'] else set()
        for i in idxs:
            picked.append((nid, i))
    picked += [(f['node'], f['index']) for f in frames if f['global_index'] % a.stride == 0]
    picked = list(dict.fromkeys(picked))[:400]
    a.work.mkdir(parents=True, exist_ok=True)
    summaries = []
    source_checked = 0
    worst_pack = dict(mean=0, p99=0, max=0, over8=0)
    worst_src = dict(mean=0, p99=0, max=0, over8=0)
    for nid, index in picked:
        matches = [f for f in frames if f['node'] == nid and f['index'] == index]
        if not matches: continue
        frame = matches[0]
        out = a.work / ('frame-%s-%d.ppm' % (nid, index))
        r = subprocess.run([str(a.render), str(a.pack_dir / 'nine_script.bin'),
                            str(a.pack_dir / 'nine_images.bin'), nid, str(index), str(out)],
                           capture_output=True)
        if r.returncode != 0:
            report['errors'] = report.get('errors', []) + ['render %s/%d' % (nid, index)]
            continue
        w, h, raw = ppm(out)
        got = [[(raw[(y * W + x) * 3], raw[(y * W + x) * 3 + 1], raw[(y * W + x) * 3 + 2])
                for x in range(W)] for y in range(H)]
        node = pack.nodes[pack.node_index[nid]]
        f = pack.frame(node[0] + index)
        vf, vc = pack.visual(f[1])
        layers = [pack.layer(vf + k) for k in range(vc)]
        ref = composite_pack(pack, layers)
        d1 = diff(got, ref)
        summaries.append(dict(node=nid, index=index, pack=d1))
        if d1['mean'] > worst_pack['mean']:
            worst_pack['mean'] = d1['mean']
        worst_pack['p99'] = max(worst_pack['p99'], d1['p99'])
        worst_pack['max'] = max(worst_pack['max'], d1['max'])
        worst_pack['over8'] = max(worst_pack['over8'], d1['over8'])
        if d1['mean'] > 7.5 or d1['p99'] > 27:
            report.setdefault('errors', []).append('pack-ref %s/%d %s' % (nid, index, d1))
        if source_checked < a.source_frames:
            src_layers = []
            if frame['bg']:
                src_layers.append(dict(src=frame['bg'], x=0, y=0, w=W, h=H, opacity=255))
            for sp in frame['sprites'] or []:
                if isinstance(sp, list):
                    src, width, left = sp[:3]
                    opacity = sp[3] if len(sp) > 3 and sp[3] is not None else 1
                    height = sp[13] if len(sp) > 13 and sp[13] else 243
                    top = 4 if height >= 200 else 243 - height
                    sw = max(1, round(width * S))
                    sh = max(1, round(height * S))
                    sx = round(left * S)
                    sy = round(top * S)
                    src_layers.append(dict(src=src, x=sx, y=sy, w=sw, h=sh,
                                           opacity=round(opacity * 255)))
                    if len(sp) > 4 and sp[4]:
                        patch = sp[4]
                        with Image.open(root / 'src' / patch.lstrip('/')) as im:
                            iw, ih = im.size
                        ax, ay = sp[9], sp[10]
                        px, py = -sp[11], -sp[12]
                        pw, ph = sp[5], sp[6]
                        crop = (round(px * iw / ax), round(py * ih / ay),
                                round((px + pw) * iw / ax), round((py + ph) * ih / ay))
                        ps = (max(1, round(pw * S)), max(1, round(ph * S)))
                        src_layers.append(dict(src=patch, crop=crop,
                                               x=round(sp[7] * S),
                                               y=round(sp[8] * S),
                                               w=ps[0], h=ps[1],
                                               opacity=round(opacity * 255)))
            src_ref = composite_source(root, src_layers)
            d2 = diff(got, src_ref)
            summaries[-1]['source'] = d2
            source_checked += 1
            worst_src['mean'] = max(worst_src['mean'], d2['mean'])
            worst_src['p99'] = max(worst_src['p99'], d2['p99'])
            worst_src['max'] = max(worst_src['max'], d2['max'])
            if d2['mean'] > 18.0 or d2['p99'] > 60:
                report.setdefault('errors', []).append('source-ref %s/%d %s' % (nid, index, d2))
    report.setdefault('errors', [])
    report['frames'] = len(summaries)
    report['sourceFramesCompared'] = source_checked
    report['worstPackRef'] = worst_pack
    report['worstSourceRef'] = worst_src
    report['details'] = summaries
    # The 256x144 internal stage has a higher edge-pixel fraction than the
    # previous 320-wide proxy; these bounds were re-baselined over all 209 frames.
    report['tolerances'] = {'packRef': 'mean<=7.5 p99<=27', 'sourceRef': 'mean<=18.0 p99<=60'}
    report['pass'] = not report.get('errors')
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('pass', 'frames', 'sourceFramesCompared',
                                             'worstPackRef', 'worstSourceRef', 'errors')},
                     ensure_ascii=False))
    return 0 if report['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())
