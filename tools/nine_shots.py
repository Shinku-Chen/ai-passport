#!/usr/bin/env python3
"""Convert host-real-LVGL PPM shots to PNG and a self-contained HTML contact sheet.

These are 320x240 host renders from the production compositor/UI at the target
layout, not device screenshots.
"""
from __future__ import annotations
import argparse, base64, html, io
from pathlib import Path
from PIL import Image

CAPTIONS = {
    'normal-16px': '16px：三行分页，320x180 等比舞台 + 60px 面板',
    'multi-character': '多人同屏：背景 + 两个角色，等比 320x180',
    'cg-16x9': 'CG：源 16:9 等比适配 320x180',
    'large-20px': '20px：两行分页，最后一行完整落在 60px 面板内',
    'menu': '游戏内菜单：320x240 安全区',
}
ORDER = ['normal-16px', 'multi-character', 'cg-16x9', 'large-20px', 'menu']

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dir', type=Path, default=Path('build/nine/shots'))
    ap.add_argument('--out', type=Path, default=Path('build/nine/shots'))
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    cards = []
    for name in ORDER:
        ppm = a.dir / f'{name}.ppm'
        if not ppm.is_file():
            raise SystemExit(f'missing {ppm}')
        im = Image.open(ppm).convert('RGB')
        png = a.out / f'{name}.png'
        im.save(png, optimize=True)
        buf = io.BytesIO(); im.save(buf, 'PNG', optimize=True)
        b64 = base64.b64encode(buf.getvalue()).decode('ascii')
        cards.append(f'''<figure><img src="data:image/png;base64,{b64}" width="320" height="240" alt="{html.escape(name)}"><figcaption><b>{html.escape(name)}</b><br>{html.escape(CAPTIONS.get(name,name))}</figcaption></figure>''')
        print(png)
    page = '''<!doctype html><meta charset="utf-8"><title>9-nine- host real-LVGL renders</title>
<style>body{background:#101722;color:#e8edf5;font:14px/1.5 system-ui,sans-serif;margin:24px}h1{font-size:20px}figure{display:inline-block;vertical-align:top;margin:0 18px 22px 0;background:#182233;border:1px solid #32445e;border-radius:8px;padding:10px}img{display:block;image-rendering:auto;border-radius:4px}figcaption{margin-top:8px;max-width:320px;color:#c9d5e5}.note{max-width:900px;color:#f0c674}</style>
<h1>9-nine- 九次九日九重色 · host real-LVGL renders</h1>
<p class="note">These are 320x240 host renders of the production compositor and gal_ui (real LVGL, real pack, real fonts), not device screenshots. Stage is 320x180 from a 256x144 internal canvas at 1.25x; panel is 60px. No device/COM/flash was used.</p>
''' + '\n'.join(cards) + '\n'
    out = a.out / 'index.html'
    out.write_text(page, encoding='utf-8')
    print(out)

if __name__ == '__main__':
    main()
