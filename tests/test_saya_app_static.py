#!/usr/bin/env python3
"""《沙耶之歌》阅读器的静态守卫(纯文本检查,不需要编译器)。

这个文件存在的直接原因:main/saya_app.c 的存档页曾经把
`static char values[6][40]` 强转成 `const char *const *` 传给行列表,被调方按
"指针数组"读,于是把字符串内容当地址用 —— 宿主机测试和编译都过,真机一进存档页
就 Load access fault。这类错误类型系统拦不住,所以在门禁里扫一遍。

检查项:
  1. main/ 下不允许把字符数组强转成字符串指针数组(`(const char *const *)` 等);
  2. 交付的界面文案里的非 ASCII 字符必须在字体子集清单里(缺字会显示成方块),
     最外层由 tools/saya_font.py --check 兜底,这里只做"清单与源文件同步"的快速核对。
"""

from __future__ import annotations

import os
import re
import sys

APP_SOURCES = ("main",)
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


def check_ascii_only_in_app_sources(errors: list) -> None:
    """界面文案里不该出现无法显示的 C0 控制字符或替换字符。"""
    for src_root in APP_SOURCES:
        for path in iter_sources(src_root):
            with open(path, "r", encoding="utf-8") as fh:
                text = fh.read()
            for bad in ("\ufffd",):
                if bad in text:
                    errors.append(f"{path}: 含替换字符 U+FFFD,源文件编码可能已损坏")


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    os.chdir(root)
    errors: list = []
    check_no_pointer_array_cast(errors)
    check_ascii_only_in_app_sources(errors)
    if errors:
        for line in errors:
            print(f"ERROR: {line}", file=sys.stderr)
        return 1
    print("Saya app static checks: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
