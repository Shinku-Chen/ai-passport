#!/usr/bin/env python3
"""《星空列车与白的旅行》阅读器的静态守卫(纯文本检查,不需要编译器)。

界面层已换成 ATRI 阅读器那套 LVGL 页面系统(main/atri_ui.c、atri_image.c、atri_app.c),
数据层仍是 starry 的模型/资源包/存档。本守卫检查门禁里最容易漏掉、但真机后果很重的几点:

  1. main/ 下不允许把字符数组强转成字符串指针数组 —— 这类错误编译能过、宿主测试也
     能过,真机一读就 Load access fault(沙耶版存档页踩过一次);
  2. main/ 下不允许出现 U+FFFD(源文件编码被破坏的信号);
  3. 界面文案 + 剧本正文里的非 ASCII 字符必须在生成好的字体清单里(缺字 = 方块);
  4. 自动阅读期间必须豁免空闲累计 —— 否则读到一半会自己变暗/熄屏/休眠。
"""

from __future__ import annotations

import os
import re
import sys

APP_ROOT = "main"
# 只有本应用真正编译的文件受这份守卫约束:baseline 的 demo_*.c / ui_pixel.c 以及旧
# 逐条带渲染器(starry_render.c / starry_gfx.c / starry_font.c / starry_ui.c /
# starry_app.c)仍留在仓库里,但不参与本应用的构建。
APP_SOURCES = (
    "main/main.c",
    "main/atri_app.c", "main/atri_app.h",
    "main/atri_image.c", "main/atri_image.h",
    "main/atri_ui.c", "main/atri_ui.h",
    "main/starry_model.c", "main/starry_model.h",
    "main/starry_pack.c", "main/starry_pack.h",
    "main/starry_save.c", "main/starry_save.h",
)
SYMBOLS = os.path.join("assets", "fonts", "starry_cjk_symbols.txt")
CAST_PATTERNS = (
    r"\(\s*const\s+char\s*\*\s*const\s*\*\s*\)",
    r"\(\s*const\s+char\s*\*\s*\*\s*\)",
    r"\(\s*char\s*\*\s*const\s*\*\s*\)",
    r"\(\s*char\s*\*\s*\*\s*\)",
)


def iter_sources(root: str = APP_ROOT):
    """本应用编译的源文件;顺带核对 CMakeLists 里的 SRCS 没漏登记。"""
    for path in APP_SOURCES:
        yield path
    cmake = read(os.path.join(root, "CMakeLists.txt"))
    match = re.search(r"SRCS(.*?)INCLUDE_DIRS", cmake, re.DOTALL)
    if not match:
        return
    listed = re.findall(r'"([^"]+\.c)"', match.group(1))
    expected = {os.path.basename(p) for p in APP_SOURCES if p.endswith(".c")}
    if set(listed) != expected:
        print(f"WARN: main/CMakeLists.txt SRCS={sorted(listed)} != {sorted(expected)}",
              file=sys.stderr)


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def check_no_pointer_array_cast(errors: list) -> None:
    for path in iter_sources(APP_ROOT):
        for lineno, line in enumerate(read(path).splitlines(), 1):
            if line.lstrip().startswith("//"):
                continue
            for pattern in CAST_PATTERNS:
                if re.search(pattern, line):
                    errors.append(f"{path}:{lineno}: 把字符数组强转成字符串指针数组会读到野指针,"
                                  "改成像 labels[]/values[] 那样用真正的指针数组")


def check_no_replacement_char(errors: list) -> None:
    for path in iter_sources(APP_ROOT):
        if "\ufffd" in read(path):
            errors.append(f"{path}: 含替换字符 U+FFFD,源文件编码可能已损坏")


def literals(path: str) -> str:
    """收集源文件里字符串字面量的非 ASCII 字符(注释里的说明不算)。"""
    chars = set()
    for literal in re.findall(r'"(?:[^"\\]|\\.)*"', read(path)):
        chars |= {c for c in literal if ord(c) > 0x7F}
    return "".join(sorted(chars))


def check_symbols_coverage(errors: list) -> None:
    """界面文案里的字必须在字体清单里;清单由 tools/starry_lvgl_font.py 生成。"""
    if not os.path.exists(SYMBOLS):
        errors.append(f"缺少字体字符清单 {SYMBOLS}:先运行 tools/starry_lvgl_font.py 生成")
        return
    covered = set(read(SYMBOLS))
    for path in iter_sources(APP_ROOT):
        for ch in literals(path):
            if ch not in covered:
                errors.append(
                    f"{path}: 界面文案里的 {ch!r} (U+{ord(ch):04X}) 不在 {SYMBOLS} 里,"
                    f"设备上会显示成方块;重新运行 tools/starry_lvgl_font.py 生成字体")


def check_chapter_transition_leaves_overlays(errors: list) -> None:
    """换章过场必须自己切回正文页:菜单里的"跳过章节"也走这条路径。

    漏掉这一步不会崩、宿主测试也看不出,但真机上菜单会一直压在正文上面。
    """
    path = os.path.join("main", "atri_app.c")
    text = read(path)
    match = re.search(r"static void start_transition\(atri_app_t \*app\)\s*\{(.*?)\n\}",
                      text, re.DOTALL)
    if not match:
        errors.append(f"{path}: 找不到 start_transition(),静态守卫失效")
        return
    if "set_page(app, ATRI_PAGE_GAME)" not in match.group(1):
        errors.append(f"{path}: start_transition() 没有切回正文页;从菜单跳过章节时菜单会留在屏幕上")


def check_auto_read_keeps_screen_awake(errors: list) -> None:
    """自动阅读时不能累计空闲时间,否则读到一半会自己变暗、熄屏、休眠。"""
    path = os.path.join("main", "atri_app.c")
    text = read(path)
    if not re.search(r"atri_app_auto_reading\(app\)[^\n]*\{\s*\n\s*app->idle_ms\s*=\s*0;",
                     text):
        errors.append(f"{path}: atri_app_tick 缺少自动阅读/快进的空闲豁免,推进中会自己熄屏/休眠;"
                      "应当把 idle_ms 清零")
    if "bool atri_app_auto_reading(const atri_app_t *app)" not in text:
        errors.append(f"{path}: 缺少 atri_app_auto_reading() 的实现")


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    os.chdir(root)
    errors: list = []
    check_no_pointer_array_cast(errors)
    check_no_replacement_char(errors)
    check_symbols_coverage(errors)
    check_chapter_transition_leaves_overlays(errors)
    check_auto_read_keeps_screen_awake(errors)
    if errors:
        for line in errors[:40]:
            print(f"ERROR: {line}", file=sys.stderr)
        print(f"静态守卫失败:{len(errors)} 项", file=sys.stderr)
        return 1
    print("Starry app static checks: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
