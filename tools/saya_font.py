#!/usr/bin/env python3
"""生成 AI Passport《沙耶之歌》阅读器的中文字体子集。

字符集来自两处,保证界面文字与剧本正文都不缺字:
  1. 资源包(main/saya_data/saya_pack.bin)里的全部剧本文字与名字;
  2. main/ 下所有 .c/.h 里出现的非 ASCII 字面量(界面文案)。

用法:
  # 生成(需要 lv_font_conv 与一个 OFL 中文字体)
  python tools/saya_font.py --font <NotoSansSC-Regular.otf> \\
      --lv-font-conv <path/to/lv_font_conv.js> --pack main/saya_data/saya_pack.bin \\
      --out-dir assets/fonts

  # 自检(只用仓库内文件,无外部依赖;静态门禁会跑这一项)
  python tools/saya_font.py --check --pack main/saya_data/saya_pack.bin \\
      --out-dir assets/fonts

字体来源:Noto Sans SC(OFL-1.1),下载地址与版本记录在 assets/README.md。
生成物(assets/fonts/saya_cjk_*.c 与 saya_cjk_symbols.txt)提交进仓库,
源字体文件不上传(体积大),重新生成时自行准备。
"""

from __future__ import annotations

import argparse
import io
import os
import re
import struct
import subprocess
import sys

SIZES = ((16, 4), (20, 4))   # (像素, bpp)
ASCII_RANGES = ((0x20, 0x7E),)


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


# ---------------------------------------------------------------- 字符收集
def pack_chars(pack_path: str) -> set:
    blob = open(pack_path, "rb").read()
    if blob[:8] != b"SAYAPK01":
        raise SystemExit(f"资源包魔数不对: {pack_path}")
    version, total, section_count = struct.unpack_from("<III", blob, 8)
    if version != 1 or total != len(blob):
        raise SystemExit("资源包版本/长度不匹配,先重新生成")
    sections = {}
    for i in range(section_count):
        sec_type, off, count, size = struct.unpack_from("<IIII", blob, 20 + 16 * i)
        sections[sec_type] = (off, count, size)

    text_off, _, text_size = sections[0]
    text = blob[text_off : text_off + text_size]
    chars = set(text.decode("utf-8"))

    # 名字表里的字符串也在 text 段里,已经被上面覆盖。
    return {c for c in chars if c not in "\r\n\t"}


def source_chars(root: str) -> set:
    chars = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        if os.path.basename(dirpath) in { "managed_components", "build" }:
            continue
        for name in filenames:
            if not name.endswith((".c", ".h", ".hpp", ".cpp")):
                continue
            path = os.path.join(dirpath, name)
            try:
                data = open(path, "rb").read().decode("utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            # 只取字符串/字符字面量里的非 ASCII 字符,避免把注释里的说明也算进去。
            for literal in re.findall(r'"(?:[^"\\]|\\.)*"', data):
                chars |= {c for c in literal if ord(c) > 0x7F}
    return chars


def required_chars(pack_path: str, source_root: str) -> set:
    chars = pack_chars(pack_path) | source_chars(source_root)
    chars.discard("\u3000")   # 全角空格用不到,减少一个字形
    for start, end in ASCII_RANGES:
        chars |= {chr(c) for c in range(start, end + 1)}
    return {c for c in chars if c not in "\r\n\t"}


# ---------------------------------------------------------------- 生成物解析
def parse_font_cmap(path: str) -> set:
    """解析 lv_font_conv 生成的 lvgl 字体 C 文件,取出它覆盖的码位集合。"""
    text = open(path, "r", encoding="utf-8").read()
    covered = set()

    # unicode_list_N:稀疏码位相对 range_start 的偏移
    lists = {}
    for name, body in re.findall(r"static const uint16_t (unicode_list_\d+)\[\][^{]*\{(.*?)\};",
                                 text, re.S):
        lists[name] = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body)]

    for body in re.findall(r"static const lv_font_fmt_txt_cmap_t cmaps\[\][^{]*\{(.*?)\n\};",
                           text, re.S):
        for entry in re.findall(r"\{(.*?)\}", body, re.S):
            start = re.search(r"\.range_start\s*=\s*(\d+)", entry)
            length = re.search(r"\.range_length\s*=\s*(\d+)", entry)
            unicode_list = re.search(r"\.unicode_list\s*=\s*(\w+)", entry)
            if not start or not length:
                continue
            start_v = int(start.group(1))
            length_v = int(length.group(1))
            name = unicode_list.group(1) if unicode_list else "NULL"
            if name == "NULL":
                covered |= {start_v + i for i in range(length_v)}
            else:
                covered |= {start_v + off for off in lists.get(name, [])}
    return covered


def font_path(out_dir: str, size: int) -> str:
    return os.path.join(out_dir, f"saya_cjk_{size}.c")


# ---------------------------------------------------------------- 主流程
def generate(args) -> int:
    chars = required_chars(args.pack, args.source_root)
    symbols = "".join(sorted(chars))
    os.makedirs(args.out_dir, exist_ok=True)

    symbols_path = os.path.join(args.out_dir, "saya_cjk_symbols.txt")
    with open(symbols_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# 由 tools/saya_font.py 生成:界面文案 + 剧本正文出现过的全部字符。\n")
        fh.write("# 重新生成字体时必须与本文件一致(--symbols 传入下面这一行)。\n")
        fh.write(symbols + "\n")
    log(f"  字符清单 {len(chars)} 个 -> {symbols_path}")

    for size, bpp in SIZES:
        out = font_path(args.out_dir, size)
        cmd = [
            args.node, args.lv_font_conv,
            "--font", args.font,
            "--size", str(size),
            "--bpp", str(bpp),
            "--format", "lvgl",
            "--no-compress",
            "--lv-font-name", f"saya_cjk_{size}",
            "--lv-include", "lvgl.h",
            "--symbols", symbols,
            "--output", out,
        ]
        log(f"  生成 {size}px/{bpp}bpp ...")
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            log(proc.stdout)
            log(proc.stderr)
            raise SystemExit(f"lv_font_conv 失败(exit {proc.returncode})")
        if proc.stdout.strip():
            log("    " + proc.stdout.strip().splitlines()[-1])
        log(f"    {out} ({os.path.getsize(out) / 1024:.0f} KB)")

    return check(args)


def check(args) -> int:
    chars = required_chars(args.pack, args.source_root)
    ok = True
    for size, _bpp in SIZES:
        path = font_path(args.out_dir, size)
        if not os.path.exists(path):
            log(f"缺少字体文件: {path}")
            ok = False
            continue
        covered = parse_font_cmap(path)
        required = {ord(c) for c in chars}
        missing = sorted(required - covered)
        log(f"  {size}px: 覆盖 {len(covered)} 码位,缺失 {len(missing)} 个")
        if missing:
            out = io.StringIO()
            print("缺失字符: " + " ".join(f"U+{cp:04X}" for cp in missing), file=out)
            for cp in missing:
                print(f"  U+{cp:04X} {chr(cp)}", file=out)
            log(out.getvalue())
            ok = False
    if not ok:
        log("字体覆盖不完整:重新运行生成命令(见本文件顶部说明)")
        return 1
    log("  字体覆盖检查通过")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--font", help="源字体 TTF/OTF(生成时必填)")
    ap.add_argument("--lv-font-conv", dest="lv_font_conv", help="lv_font_conv.js 路径")
    ap.add_argument("--node", default="node", help="node 可执行文件")
    ap.add_argument("--pack", default="main/saya_data/saya_pack.bin")
    ap.add_argument("--source-root", default="main")
    ap.add_argument("--out-dir", default="assets/fonts")
    ap.add_argument("--check", action="store_true", help="只做覆盖度自检,不生成")
    args = ap.parse_args()

    if args.check:
        return check(args)
    if not args.font or not args.lv_font_conv:
        ap.error("生成模式需要 --font 与 --lv-font-conv")
    return generate(args)


if __name__ == "__main__":
    raise SystemExit(main())
