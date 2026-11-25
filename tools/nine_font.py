#!/usr/bin/env python3
"""Generate per-episode LVGL 16/20px fonts and verify every emitted glyph.

Uses the repository's existing gal_font rasterizer/emitter, no Node conversion.
The read-only source subset font is preferred; --fallback supplies missing UI
glyphs (defaults to Windows Microsoft YaHei when installed). Source font files
are never copied into output.
"""
from __future__ import annotations
import argparse, json, sys
from pathlib import Path
from PIL import ImageFont
from fontTools.ttLib import TTFont
import gal_font

def cmap(path):
 with TTFont(path,fontNumber=0) as font:return set(font.getBestCmap())

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--font',type=Path,required=True);p.add_argument('--fallback',type=Path,default=Path('C:/Windows/Fonts/msyh.ttc'));p.add_argument('--out',type=Path,required=True);p.add_argument('--source-root',type=Path,default=Path('main'))
 a=p.parse_args();chars=set((a.out/'nine_symbols.txt').read_text('utf-8'));chars.update(gal_font.source_chars(str(a.source_root)));chars.update(map(chr,range(32,127)));chars-=set('\r\n\t');cps=sorted(map(ord,chars))
 primary=cmap(a.font);fallback=cmap(a.fallback) if a.fallback.is_file() else set();missing=set(cps)-primary-fallback
 if missing:raise SystemExit('Missing font codepoints: '+','.join(f'U+{cp:04X}' for cp in sorted(missing)))
 report={'glyphs':len(cps),'fallbackGlyphs':len(set(cps)-primary),'sizes':{}}
 a.out.mkdir(parents=True,exist_ok=True)
 (a.out/'nine_symbols.txt').write_text(''.join(map(chr,cps)),encoding='utf-8')
 for size in (16,20):
  glyphs=gal_font.rasterize(str(a.font),size,[cp for cp in cps if cp in primary])
  fallback_cps=[cp for cp in cps if cp not in primary]
  if fallback_cps:glyphs.update(gal_font.rasterize(str(a.fallback),size,fallback_cps))
  font=ImageFont.truetype(str(a.font),size,layout_engine=ImageFont.Layout.BASIC);asc,desc=font.getmetrics();height=size+gal_font.EXTRA_LEADING;baseline=asc-(asc+desc-height)//2
  name=f'nine_cjk_{size}';path=a.out/f'{name}.c';path.write_text(gal_font.emit_font(name,size,cps,glyphs,height,height-baseline),encoding='utf-8')
  if gal_font.verify(str(path),cps,glyphs):raise SystemExit('Emitted glyph verification failed')
  compiled=gal_font.parse_font(str(path));bitmap=len(compiled['bitmap']);report['sizes'][str(size)]={'bitmapBytes':bitmap,'estimatedCompiledBytes':bitmap+(len(cps)+1)*16+len(cps)*2,'sourceBytes':path.stat().st_size,'missingGlyphs':0,'verifiedGlyphs':len(cps)}
 (a.out/'nine_font_report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(report))

if __name__=='__main__':main()
