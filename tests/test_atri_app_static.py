#!/usr/bin/env python3
"""《ATRI -My Dear Moments-》阅读器的静态守卫(纯文本检查,不需要编译器)。

这个文件存在的直接原因(与《沙耶之歌》阅读器同源):界面层把局部字符数组当成
"字符串指针数组"传给列表接口,被调方按指针数组读 —— 宿主机测试和编译都过,
真机一进列表页就 Load access fault。这类错误类型系统拦不住,所以在门禁里扫一遍。

检查项:
  1. main/ 下不允许把字符数组强转成字符串指针数组(`(const char *const *)` 等);
  2. 交付的界面文案里的非 ASCII 字符必须在字体子集清单里(缺字会显示成方块),
     最外层由 tools/atri_font.py --check 兜底,这里只做"清单与源文件同步"的快速核对;
  3. 源文件里不允许出现替换字符 U+FFFD(编码损坏)。
"""

from __future__ import annotations

import os
import re
import sys

APP_SOURCES = ("main",)
SYMBOLS = os.path.join("assets", "fonts", "atri_cjk_symbols.txt")
CAST_PATTERNS = (
    r"\(\s*const\s+char\s*\*\s*const\s*\*\s*\)",
    r"\(\s*const\s+char\s*\*\s*\*\s*\)",
    r"\(\s*char\s*\*\s*const\s*\*\s*\)",
    r"\(\s*char\s*\*\s*\*\s*\)",
)


def iter_sources(root: str):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if name.endswith((".c", ".h")):
                yield os.path.join(dirpath, name)


def check_no_pointer_array_cast(errors: list) -> None:
    for src_root in APP_SOURCES:
        for path in iter_sources(src_root):
            with open(path, "r", encoding="utf-8") as fh:
                for lineno, line in enumerate(fh, 1):
                    if line.lstrip().startswith("//"):
                        continue
                    for pattern in CAST_PATTERNS:
                        if re.search(pattern, line):
                            errors.append(
                                f"{path}:{lineno}: 把字符数组强转成字符串指针数组会读到野指针,"
                                f"改成像 labels[]/values[] 那样用真正的指针数组"
                            )


def literals(path: str) -> str:
    """收集源文件里字符串字面量的非 ASCII 字符。"""
    data = open(path, "rb").read().decode("utf-8")
    chars = set()
    for literal in re.findall(r'"(?:[^"\\]|\\.)*"', data):
        chars |= {c for c in literal if ord(c) > 0x7F}
    return "".join(sorted(chars))


def check_symbols_coverage(errors: list) -> None:
    if not os.path.exists(SYMBOLS):
        errors.append(f"缺少字体字符清单 {SYMBOLS}:先运行 tools/atri_font.py 生成")
        return
    covered = set(open(SYMBOLS, encoding="utf-8").read())
    for src_root in APP_SOURCES:
        for path in iter_sources(src_root):
            text = open(path, "r", encoding="utf-8").read()
            if "\ufffd" in text:
                errors.append(f"{path}: 含替换字符 U+FFFD,源文件编码可能已损坏")
            for ch in literals(path):
                if ch not in covered:
                    errors.append(
                        f"{path}: 界面文案里的 {ch!r} (U+{ord(ch):04X}) 不在 {SYMBOLS} 里,"
                        f"设备上会显示成方块;重新运行 tools/atri_font.py 生成字体"
                    )
    return


def main() -> int:
    os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    errors: list = []
    check_no_pointer_array_cast(errors)
    check_symbols_coverage(errors)
    if errors:
        for e in errors[:40]:
            print(f"ERROR: {e}", file=sys.stderr)
        print(f"静态守卫失败:{len(errors)} 项", file=sys.stderr)
        return 1
    print("ATRI 静态守卫: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
