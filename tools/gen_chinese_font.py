#!/usr/bin/env python3
"""生成覆盖完整 GB2312 的 LVGL 字体源码(设备屏中文字库)。

用法::

    python3 tools/gen_chinese_font.py                 # 生成 main/fonts/gb2312_14.[ch]
    python3 tools/gen_chinese_font.py --verify-only   # 只自检已有产物,不重新转换
    python3 tools/gen_chinese_font.py --font D:/fonts/other.ttf

字符清单(请求集合 = 以下三类之和):
    - GB2312 符号区:0xA1A1-0xF7FE 中所有可解码的非汉字码位(682 个),含中文
      标点、全角 ASCII(FF01-FF5E)、单位/货币(℃ ℉ № ‰ §…)、箭头、几何、
      制表符、圈数字等;
    - GB2312 汉字区:同样码位范围内的 6763 个汉字(一级 3755 + 二级 3008);
    - ASCII 可打印字符 0x20-0x7E(95 个):状态行、提示、版本号里的拉丁字母与数字。
    换行/制表符不生成字形,它们是 LVGL 的排版字符,不是可绘制符号。

输出:
    main/fonts/gb2312_14.c   自包含源码,导出 const lv_font_t lv_font_gb2312_14
    main/fonts/gb2312_14.h   声明头,提供 LV_FONT_DECLARE(lv_font_gb2312_14)
    由 --size 决定的文件名/符号名模式为 gb2312_<size> / lv_font_gb2312_<size>。

LVGL 9.x 兼容性(重要,改参数前先读):
    - lv_font_conv 1.5.3 默认输出 RLE 压缩字体(bitmap_format=1),其压缩流与
      LVGL 9 的 lv_font_fmt_txt 解码路径不匹配,实机会呈现乱码;因此这里固定
      使用 ``--no-compress --no-prefilter`` 生成 PLAIN(bitmap_format=0)字体,
      与 Kconfig 里的 CONFIG_LV_USE_FONT_COMPRESSED 无关。
    - lv_font_conv 默认把非 LV_LVGL_H_INCLUDE_SIMPLE 分支写成
      ``#include "lvgl/lvgl.h"``。本仓库 main 组件的 LVGL 组件目录名是
      lvgl__lvgl,内建 include 目录只有组件根与其 src/,且 main 未定义
      LV_LVGL_H_INCLUDE_SIMPLE,因此 ``lvgl/lvgl.h`` 无法解析(已用真实
      build/compile_commands.json 里的编译命令实测:该分支报
      "lvgl/lvgl.h: No such file or directory")。故这里显式传
      ``--lv-include lvgl.h``,两条分支都使用 ``#include "lvgl.h"``,与仓库里
      main/*.c 的写法一致,不依赖任何宏、Kconfig 开关或 CMake 改动。
    - 生成结果内部用 LVGL_VERSION_MAJOR / LV_VERSION_CHECK 做条件编译,在本仓库
      锁定的 LVGL 9.6.x 上取 LVGL 9 分支(含 .bitmap_format/.fallback 字段)。

依赖:
    - lv_font_conv 1.5.3(优先使用全局 npm 安装的 lv_font_conv.js + node,
      避免经过 cmd.exe 传参;也可用 --lv-font-conv 或环境变量 LV_FONT_CONV
      指向 lv_font_conv.js / 可执行文件,或回退到 npx);
    - 源字体默认用 C:/Windows/Fonts/NotoSansSC-VF.ttf(Noto Sans SC,SIL OFL 1.1),
      可用 --font 或环境变量 GB2312_FONT 换成其它含 GB2312 字形的字体。

为什么不用 simhei.ttf:Windows 自带的 simhei/msyh 受系统授权约束,不能再分发;
由它派生的位图字库随固件公开发布会有授权风险。Noto Sans SC 是 OFL 1.1,允许再
分发与嵌入式派生。换字体只需重跑本脚本,产物与调用方无需改动。

汉字、ASCII 与符号必须全部命中,否则脚本以非零状态退出(Noto Sans SC 可做到
7540/7540;simhei 缺 U+30FB(・,片假名中点),缺失码位会打印出来)。
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
# 默认源字体:Noto Sans SC(SIL OFL 1.1,可再分发)。Windows 自带的 simhei/msyh
# 受系统授权约束,由它们派生的位图字库随固件发布有授权风险,因此不作默认。
DEFAULT_FONT = "C:/Windows/Fonts/NotoSansSC-VF.ttf"
DEFAULT_SIZE = 14
DEFAULT_BPP = 2
DEFAULT_OUTPUT_DIR = "main/fonts"

# GB2312 的符号区与汉字区都落在 A1A1-F7FE;F8-FE 是用户自定义区,不生成。
GB2312_FIRST_ROW = 0xA1
GB2312_LAST_ROW = 0xF7
GB2312_FIRST_CELL = 0xA1
GB2312_LAST_CELL = 0xFE
# ASCII 可打印区间,含空格(状态行里必须能画出空格宽度)。
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E

CONVERSION_TIMEOUT_S = 900
GLYPH_COMMENT_RE = re.compile(r"/\* U\+([0-9A-F]{4,6}) ")


class FontToolError(RuntimeError):
    """可预期的失败:缺工具、缺字体、转换器报错。"""


def gb2312_characters() -> tuple[list[str], list[str]]:
    """解码 GB2312 码位,返回 (汉字列表, 非汉字符号列表)。

    GB2312 行与列各 94 个位置,其中存在未分配的空位,直接解码会抛
    UnicodeDecodeError,这里按位跳过,得到与标准一致的真实字符数:
    6763 个汉字 + 682 个符号。
    """
    hanzi: list[str] = []
    symbols: list[str] = []
    for row in range(GB2312_FIRST_ROW, GB2312_LAST_ROW + 1):
        for cell in range(GB2312_FIRST_CELL, GB2312_LAST_CELL + 1):
            try:
                char = bytes((row, cell)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if "\u4e00" <= char <= "\u9fff":
                hanzi.append(char)
            else:
                symbols.append(char)
    return hanzi, symbols


def build_inventory() -> dict[str, object]:
    """构造请求字符集与分类计数。"""
    hanzi, symbols = gb2312_characters()
    ascii_chars = [chr(cp) for cp in range(ASCII_FIRST, ASCII_LAST + 1)]
    requested = sorted(set(hanzi) | set(symbols) | set(ascii_chars))
    return {
        "requested": requested,
        "hanzi": set(hanzi),
        "symbols": set(symbols),
        "ascii": set(ascii_chars),
    }


def _sibling_lv_font_conv_js(shim: str) -> str | None:
    """全局 npm shim 旁边的 lv_font_conv.js(git-bash 下 shim 无扩展名也适用)。"""
    npm_dir = Path(shim).parent
    for name in ("lv_font_conv.js", "cli.js"):
        candidate = npm_dir / "node_modules" / "lv_font_conv" / name
        if candidate.is_file():
            return str(candidate)
    return None


def _npx_cache_js() -> str | None:
    """npx 缓存里已下载的 lv_font_conv.js。"""
    import glob

    patterns = [
        os.path.expandvars(
            r"%LOCALAPPDATA%\npm-cache\_npx\*\node_modules\lv_font_conv\lv_font_conv.js"
        ),
        os.path.expandvars(
            "$HOME/.npm/_npx/*/node_modules/lv_font_conv/lv_font_conv.js"
        ),
    ]
    for pattern in patterns:
        for hit in sorted(glob.glob(pattern)):
            return hit
    return None


def _command_for(target: str) -> list[str]:
    """把候选目标翻译成可执行命令行前缀。"""
    if target.endswith(".js"):
        node = shutil.which("node")
        if not node:
            raise FontToolError(f"运行 {target} 需要 node,但未在 PATH 中找到")
        return [node, target]
    if Path(target).name.lower().startswith("npx"):
        return [target, "--yes", "lv_font_conv"]
    return [target]


def resolve_converter(explicit: str | None) -> tuple[list[str], str]:
    """按优先级定位 lv_font_conv,返回 (命令行前缀, 来源说明)。

    优先走 node + lv_font_conv.js:Windows 上直接执行 npm 的 .CMD shim 会经过
    cmd.exe,字符集参数里的 < > | & % ! 可能被 shell 解释,导致字形静默丢失。
    """
    requested = explicit or os.environ.get("LV_FONT_CONV")
    if requested:
        return _command_for(requested), f"--lv-font-conv/LV_FONT_CONV ({requested})"

    global_shim = shutil.which("lv_font_conv")
    if global_shim:
        js = _sibling_lv_font_conv_js(global_shim)
        if js:
            return _command_for(js), f"全局 lv_font_conv.js ({js})"
        return _command_for(global_shim), f"全局 lv_font_conv ({global_shim})"

    npx = shutil.which("npx")
    if npx:
        return _command_for(npx), f"npx ({npx})"

    cached_js = _npx_cache_js()
    if cached_js:
        return _command_for(cached_js), f"npx 缓存 ({cached_js})"

    raise FontToolError(
        "找不到 lv_font_conv:请安装 node 与 lv_font_conv@1.5.3,或用 "
        "--lv-font-conv / LV_FONT_CONV 指定 lv_font_conv.js"
    )


def converter_version(prefix: list[str]) -> str:
    """读取转换器版本,失败时返回 unknown(不阻断生成)。"""
    try:
        result = subprocess.run(
            [*prefix, "--version"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=120,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    if result.returncode != 0:
        return "unknown"
    text = (result.stdout or result.stderr or "").strip()
    return text.splitlines()[0] if text else "unknown"


def conversion_command(
    prefix: list[str],
    font: Path,
    size: int,
    bpp: int,
    symbols: str,
    symbol: str,
    output: Path,
) -> list[str]:
    """构造生成命令。固定 --no-compress --no-prefilter,见模块 docstring。"""
    return [
        *prefix,
        "--font",
        str(font),
        "--size",
        str(size),
        "--bpp",
        str(bpp),
        "--format",
        "lvgl",
        "--no-compress",
        "--no-prefilter",
        # 让两条 include 分支都落到 "lvgl.h"(见模块 docstring)
        "--lv-include",
        "lvgl.h",
        "--symbols",
        symbols,
        "--lv-font-name",
        symbol,
        "--output",
        str(output),
    ]


def printable_command(command: list[str], symbols: str) -> str:
    """打印用命令:把超长的 --symbols 参数缩写成字符数。"""
    shown = [
        f"<{len(symbols)} 个字符>" if item == symbols else item for item in command
    ]
    return " ".join(shown)


def run_conversion(command: list[str]) -> None:
    """执行 lv_font_conv。"""
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=CONVERSION_TIMEOUT_S,
        )
    except OSError as error:
        raise FontToolError(f"无法启动 lv_font_conv: {error}") from error
    except subprocess.TimeoutExpired as error:
        raise FontToolError(f"lv_font_conv 超时({CONVERSION_TIMEOUT_S}s)") from error
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()[-1500:]
        raise FontToolError(f"lv_font_conv 失败(rc={result.returncode}):\n{detail}")
    if (result.stderr or "").strip():
        # lv_font_conv 把"源字体缺字形"等提示写到 stderr,保留可见。
        for line in (result.stderr or "").strip().splitlines():
            print(f"[lv_font_conv] {line}")


def write_header(path: Path, guard: str, symbol: str, size: int, bpp: int, count: int) -> None:
    """写声明头。包含 lvgl.h 的条件与生成源码一致,方便单独 include。"""
    path.write_text(
        f"""/* 本文件由 tools/gen_chinese_font.py 生成,请勿手工修改。
 * GB2312 全量中文字库:size={size} px,bpp={bpp},PLAIN(非压缩)位图格式,
 * 共 {count} 个字形;重新生成:python3 tools/gen_chinese_font.py
 * 编译:把 main/fonts/{path.stem}.c 加入 main 组件的 SRCS。
 * 生成源码与本声明头都包含 "lvgl.h":main 组件的 include 目录已包含 LVGL 组件根,
 * 不依赖 LV_LVGL_H_INCLUDE_SIMPLE 等额外宏。
 */
#ifndef {guard}
#define {guard}

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

LV_FONT_DECLARE({symbol});

#ifdef __cplusplus
}}
#endif

#endif /* {guard} */
""",
        encoding="utf-8",
    )


def generated_glyphs(source: str) -> set[str]:
    """从 .c 的字形注释里取出实际生成的码位。"""
    return {chr(int(match, 16)) for match in GLYPH_COMMENT_RE.findall(source)}


def verify(
    c_path: Path, h_path: Path, symbol: str, guard: str, inventory: dict
) -> tuple[list[str], dict]:
    """自检产物,返回问题列表(空列表即通过)。"""
    problems: list[str] = []
    if not c_path.is_file():
        return [f"缺少产物 {c_path}"]
    if not h_path.is_file():
        problems.append(f"缺少产物 {h_path}")

    source = c_path.read_text(encoding="utf-8", errors="replace")
    generated = generated_glyphs(source)
    requested = set(inventory["requested"])
    missing = sorted(requested - generated)
    unexpected = sorted(generated - requested)

    if f"const lv_font_t {symbol} = {{" not in source:
        problems.append(f"{c_path.name}: 未找到导出符号 {symbol}")
    if ".bitmap_format = 0," not in source:
        problems.append(
            f"{c_path.name}: 位图不是 PLAIN(bitmap_format=0),与 LVGL 9 解码不兼容"
        )
    if 'get_glyph_bitmap = lv_font_get_bitmap_fmt_txt' not in source:
        problems.append(f"{c_path.name}: 缺少 lv_font_get_bitmap_fmt_txt 绑定")
    if ".fallback = NULL," not in source:
        problems.append(f"{c_path.name}: 缺少 LVGL 9 的 .fallback 字段")
    if '#include "lvgl.h"' not in source:
        problems.append(f"{c_path.name}: 缺少 LVGL 头文件包含(非自包含)")
    if "lvgl/lvgl.h" in source:
        problems.append(
            f"{c_path.name}: 仍包含 lvgl/lvgl.h 分支,本仓库 main 组件无法解析"
        )

    core_missing = sorted(set(missing) & (inventory["hanzi"] | inventory["ascii"]))
    if core_missing:
        preview = " ".join(f"U+{ord(c):04X}" for c in core_missing[:20])
        problems.append(
            f"源字体缺少 {len(core_missing)} 个必需字形(汉字/ASCII):{preview}"
        )
    if unexpected:
        problems.append(f"产物含 {len(unexpected)} 个未请求的码位")

    if h_path.is_file():
        header = h_path.read_text(encoding="utf-8", errors="replace")
        if f"LV_FONT_DECLARE({symbol});" not in header:
            problems.append(f"{h_path.name}: 未声明 {symbol}")
        if f"#ifndef {guard}" not in header:
            problems.append(f"{h_path.name}: 缺少 include guard {guard}")

    report = {
        "missing": missing,
        "generated": len(generated),
        "c_size": c_path.stat().st_size,
        "c_lines": source.count("\n") + 1,
        "h_size": h_path.stat().st_size if h_path.is_file() else 0,
    }
    return problems, report


def print_report(inventory: dict, report: dict, symbol: str, problems: list[str]) -> None:
    """打印可粘贴进 assets/README 的统计数字。"""
    print("—— 自检报告 ——")
    print(
        f"请求字符数: {len(inventory['requested'])} "
        f"(汉字 {len(inventory['hanzi'])} + GB2312 符号 {len(inventory['symbols'])} "
        f"+ ASCII {len(inventory['ascii'])})"
    )
    print(f"生成字形数: {report['generated']}")
    missing = report["missing"]
    print(f"源字体缺失: {len(missing)}" + (
        "(" + " ".join(f"U+{ord(c):04X}" for c in missing) + ")" if missing else ""
    ))
    print(f".c 大小: {report['c_size']} 字节 ({report['c_size'] / 1024 / 1024:.2f} MiB),"
          f"{report['c_lines']} 行")
    print(f".h 大小: {report['h_size']} 字节")
    print(f"导出符号: {symbol}")
    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
    else:
        print("自检: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="生成覆盖完整 GB2312 的 LVGL 字体源码。",
    )
    parser.add_argument(
        "--font",
        default=os.environ.get("GB2312_FONT", DEFAULT_FONT),
        help=f"源字体路径(默认 {DEFAULT_FONT},环境变量 GB2312_FONT 可覆盖)",
    )
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE, help="字号像素(默认 14)")
    parser.add_argument("--bpp", type=int, default=DEFAULT_BPP, help="每像素位深(默认 2)")
    parser.add_argument(
        "--output-dir",
        default=DEFAULT_OUTPUT_DIR,
        help=f"产物目录,相对仓库根(默认 {DEFAULT_OUTPUT_DIR})",
    )
    parser.add_argument(
        "--lv-font-conv",
        default=None,
        help="lv_font_conv.js 或可执行文件路径(默认自动查找,环境变量 LV_FONT_CONV 亦可)",
    )
    parser.add_argument(
        "--verify-only", action="store_true", help="只自检已有产物,不重新转换"
    )
    args = parser.parse_args()

    size = args.size
    stem = f"gb2312_{size}"
    symbol = f"lv_font_{stem}"
    guard = stem.upper() + "_H"
    output_dir = ROOT / args.output_dir
    c_path = output_dir / f"{stem}.c"
    h_path = output_dir / f"{stem}.h"

    inventory = build_inventory()
    symbols = "".join(inventory["requested"])

    if not args.verify_only:
        font_path = Path(args.font)
        if not font_path.is_file():
            raise SystemExit(f"源字体不存在: {font_path}(用 --font 指定中文字体)")
        prefix, origin = resolve_converter(args.lv_font_conv)
        version = converter_version(prefix)
        output_dir.mkdir(parents=True, exist_ok=True)
        command = conversion_command(
            prefix, font_path, size, args.bpp, symbols, symbol, c_path
        )
        print(f"转换器: {origin},版本 {version}")
        print("命令: " + printable_command(command, symbols))
        run_conversion(command)
        if not c_path.is_file():
            raise SystemExit(f"lv_font_conv 未生成 {c_path}")
        # 声明头里记录真实字形数(simhei.ttf 不含的字形会被跳过)
        generated = len(
            generated_glyphs(c_path.read_text(encoding="utf-8", errors="replace"))
        )
        write_header(h_path, guard, symbol, size, args.bpp, generated)

    problems, report = verify(c_path, h_path, symbol, guard, inventory)
    print_report(inventory, report, symbol, problems)
    return 1 if problems else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except FontToolError as error:
        raise SystemExit(f"ERROR: {error}") from error
