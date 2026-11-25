#!/usr/bin/env python3
"""Generate a source-art reference sample sheet for tests/nine/test_nine_image.c.

The reference is composed from the original 432x243 source art (not from the
compressed pack), then the C decoder/compositor is compared against it with a
tolerance that accounts for JPEG quality 38 and 192-colour sprite palettes.
"""
from __future__ import annotations
import argparse, json, random
from pathlib import Path
from PIL import Image

NONE = 0xffffffff


def js_graph(path):
    text = path.read_text('utf-8')
    return json.loads(text.split('=', 1)[1].strip().rstrip(';'))


def expand_sprite(s, i):
    if not isinstance(s, list):
        d = dict(s)
        d['opacity'] = 1 if d.get('opacity') is None else d['opacity']
        return d
    src, width, left = s[0], s[1], s[2]
    opacity = 1 if len(s) < 4 or s[3] is None else s[3]
    patch = s[4] if len(s) > 4 else ''
    height = s[13] if len(s) > 13 and s[13] else 243
    top = 4 if height >= 200 else 243 - height
    d = {'src': src, 'width': width, 'height': height, 'left': left, 'top': top,
         'opacity': opacity}
    if patch:
        d.update({'patch': patch, 'patchWidth': s[5], 'patchHeight': s[6],
                  'patchLeft': s[7], 'patchTop': s[8], 'patchAtlasWidth': s[9],
                  'patchAtlasHeight': s[10], 'patchImageLeft': s[11],
                  'patchImageTop': s[12]})
    return d


def decode_chunk(raw):
    out, bg, sp = [], '', []
    for f in raw:
        if not f or f.get('kind') == 'op':
            out.append(f)
            continue
        if 'art' in f:
            bg = f['art'].get('background', '') or ''
            sp = f['art'].get('sprites') or []
            out.append({'art': {'background': bg, 'sprites': [expand_sprite(x, i) for i, x in enumerate(sp)]}})
            continue
        if 'b' in f:
            bg = f['b']
        if 'p' in f:
            sp = [expand_sprite(x, i) for i, x in enumerate(f['p'])]
        out.append({'art': {'background': bg, 'sprites': sp}})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--node', default='n23')
    ap.add_argument('--index', type=int, default=10)
    a = ap.parse_args()
    src = a.source / 'nine-yukiiro-rw5'
    graph = js_graph(src / 'src/common/graph.js')
    n = graph['nodes'][a.node]
    frames = []
    for c in n['chunks']:
        rows = json.loads((src / 'src/common/story' / c['file'].lstrip('/')).read_text('utf-8'))
        frames.extend(decode_chunk(rows))
    art = frames[a.index]['art']

    canvas = Image.new('RGBA', (320, 240), (0, 0, 0, 255))
    bg = Image.open(src / 'src' / art['background'].lstrip('/')).convert('RGB')
    bg = bg.resize((320, 180), Image.Resampling.LANCZOS)
    canvas.paste(bg, (0, 0))

    for d in art['sprites']:
        w = max(1, round(d['width'] * 320 / 432))
        h = max(1, round(d['height'] * 180 / 243))
        x = round(d['left'] * 320 / 432)
        y = round(d['top'] * 180 / 243)
        alpha = round(d.get('opacity', 1) * 255)
        im = Image.open(src / 'src' / d['src'].lstrip('/')).convert('RGBA')
        im = im.resize((w, h), Image.Resampling.LANCZOS)
        if alpha < 255:
            r, g, b, al = im.split()
            al = al.point(lambda v: v * alpha // 255)
            im = Image.merge('RGBA', (r, g, b, al))
        canvas.alpha_composite(im, (x, y))
        if d.get('patch'):
            with Image.open(src / 'src' / d['patch'].lstrip('/')) as atlas:
                atlas.load()
                iw, ih = atlas.size
                ax, ay = d['patchAtlasWidth'], d['patchAtlasHeight']
                px, py = -d['patchImageLeft'], -d['patchImageTop']
                pw, ph = d['patchWidth'], d['patchHeight']
                crop = (round(px * iw / ax), round(py * ih / ay),
                        round((px + pw) * iw / ax), round((py + ph) * ih / ay))
                ps = (max(1, round(pw * 320 / 432)), max(1, round(ph * 180 / 243)))
                face = atlas.convert('RGBA').crop(crop).resize(ps, Image.Resampling.LANCZOS)
            if alpha < 255:
                r, g, b, al = face.split()
                al = al.point(lambda v: v * alpha // 255)
                face = Image.merge('RGBA', (r, g, b, al))
            canvas.alpha_composite(face, (round(d['patchLeft'] * 320 / 432),
                                          round(d['patchTop'] * 180 / 243)))

    # Fixed grid plus random samples; stay in the visible 320x180 art area.
    samples = []
    for y in range(6, 170, 17):
        for x in range(6, 320, 28):
            samples.append((x, y))
    rnd = random.Random(9)
    for _ in range(160):
        samples.append((rnd.randrange(0, 320), rnd.randrange(0, 170)))
    lines = [f'{x} {y} {canvas.getpixel((x, y))[0]} {canvas.getpixel((x, y))[1]} {canvas.getpixel((x, y))[2]}'
             for x, y in samples]
    a.out.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(json.dumps({'node': a.node, 'index': a.index, 'samples': len(samples),
                      'background': art['background'], 'sprites': len(art['sprites']),
                      'out': str(a.out)}))


if __name__ == '__main__':
    main()
