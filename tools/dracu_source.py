#!/usr/bin/env python3
"""Locate, load and index the DRACU-RIOT source material for the pack builders.

Source project: https://github.com/hezdaaa/dracu-riot-miband
  - 小米手环快应用上的《DRACU-RIOT!》(柚子社 / Yuzusoft)移植,自己带一条
    KiriKiri2 解包 → 页表转换 → 立绘分层合成的工具链。
  - 剧本、立绘、背景、事件 CG 版权归 Yuzusoft 所有;本仓库只保存转换产物
    (main/dracu_data/),源素材用 tools/dracu_fetch_source.py 按需拉取。

源工程把整部作品压成一条"全局线性页表"(src/common/script/scriptData*.txt,
500 页一块),每页一条记录,字段:

  t   正文(UTF-8,\\n 换行)
  s   说话人(中文名;缺省 = 旁白)
  b   背景名(对应 src/common/bg/<名>.jpg)
  c   立绘引用 "角色?dress=..&pose=..&face=..@位置"(分号分隔可多个)
  cg  事件 CG(对应 src/common/evig/<名>.jpg|png)
  sd  SD 小人(对应 src/common/evig/<名>.png)
  cs  立绘缩放百分比(源播放器实际恒按 100 处理)
  fs  字号百分比(75 = 小字,仅 69 页)
  blur 背景轻微放大(模糊感的替代)
  e   震动效果,f 闪光弹
  co  本页是选项页;c1..c5 选项文案,c1t..c5t 目标页号

本模块只负责"读源数据 + 建立稳定的 id 映射",两个打包器共用,避免各写一份
排序规则导致设备侧 id 对不上。所有映射都按下面的规则确定(可复现):

  背景 id    0 = 标题图(--title-art),其余按剧本引用到的背景名升序
  事件 CG id 按剧本引用到的事件图名升序
  SD id      按剧本引用到的 SD 名升序
  立绘 id    按剧本引用到的立绘描述串升序(见 sprite_key)
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path

# 源工程素材目录(相对源仓库根)
SCRIPT_DIR = "src/common/script"
BG_DIR = "src/common/bg"
EV_DIR = "src/common/evig"
CH_DIR = "src/common/cimg"
CHAR_DATA = "src/common/char_data.txt"
TITLE_BG = "src/common/title_bg.jpg"
LOGO = "src/common/logo.png"
BRANCH_CONFIG = "转换工具/branchConfig.js"
LCT_PAGE = "src/pages/lct/lct.ux"

# 源工程屏幕(小米手环快应用 336x480)
SRC_W, SRC_H = 336, 480
# 源工程立绘合成:脸区顶部对齐到 y=130(见 detail.ux 的 parseCharacterOnly)
SRC_FACE_TOP = 130

# 设备屏幕
SCREEN_W, SCREEN_H = 240, 320
# 立绘"脸区顶部"在设备屏幕上的 y:130/480 的比例,向上取整到整数像素
SPRITE_HEAD_Y = round(SRC_FACE_TOP * SCREEN_H / SRC_H)   # 87

SCRIPT_RE = re.compile(r"scriptData(\d+)\.txt$")


def script_files(source: Path) -> list[Path]:
    """按块号自然序返回全部剧本块。"""
    paths = [p for p in (source / SCRIPT_DIR).glob("scriptData*.txt") if SCRIPT_RE.search(p.name)]
    return sorted(paths, key=lambda p: int(SCRIPT_RE.search(p.name).group(1)))


def load_pages(source: Path) -> dict[int, dict]:
    """读入全部剧本页,页号(int) -> 记录。"""
    pages: dict[int, dict] = {}
    for path in script_files(source):
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
        for key, value in data.items():
            pages[int(key)] = value
    return pages


def page_range(pages: dict[int, dict]) -> tuple[int, int]:
    keys = pages.keys()
    return min(keys), max(keys)


# --------------------------------------------------------------------------
# 引用收集
# --------------------------------------------------------------------------


@dataclass
class Refs:
    """剧本引用到的素材名 → 引用次数(id 映射与差分基准都按它排序)。"""

    bg: dict[str, int] = field(default_factory=dict)
    cg: dict[str, int] = field(default_factory=dict)
    sd: dict[str, int] = field(default_factory=dict)
    sprite: dict[str, int] = field(default_factory=dict)

    def bump(self, bucket: dict[str, int], name: str) -> None:
        if name:
            bucket[name] = bucket.get(name, 0) + 1


def collect_refs(pages: dict[int, dict]) -> Refs:
    refs = Refs()
    for page in pages.values():
        refs.bump(refs.bg, str(page.get("b") or ""))
        for name in str(page.get("cg") or "").split(";"):
            refs.bump(refs.cg, name.split("@")[0].strip())
        for name in str(page.get("sd") or "").split(";"):
            refs.bump(refs.sd, name.split("@")[0].strip())
        for name in str(page.get("c") or "").split(";"):
            refs.bump(refs.sprite, name.split("@")[0].strip())
    return refs


def sorted_names(counter: dict[str, int]) -> list[str]:
    """稳定的 id 顺序:字典序(与引用次数无关,重建结果永远一致)。"""
    return sorted(counter)


# --------------------------------------------------------------------------
# 立绘:描述串 → 身体/表情分层
# --------------------------------------------------------------------------

SPRITE_PARAM_RE = re.compile(r"([A-Za-z_]+)=([^&]*)")


@dataclass(frozen=True)
class SpriteSpec:
    raw: str            # 剧本里的完整描述(去掉 @位置 后缀)
    character: str      # 角色名
    dress: str          # 服装名
    pose: str           # 姿势
    face: str           # 表情编号(原始值,可能带后缀)


def parse_sprite(raw: str) -> SpriteSpec | None:
    """拆 "角色?dress=..&pose=..&face=.."。没有参数(如纯角色名)时返回 None。"""
    text = raw.split("@")[0].strip()
    if not text:
        return None
    head, _, rest = text.partition("?")
    params = {key: value for key, value in SPRITE_PARAM_RE.findall(rest)}
    return SpriteSpec(
        raw=text,
        character=head.strip(),
        dress=params.get("dress", "").strip(),
        pose=(params.get("pose", "1") or "1").strip(),
        face=(params.get("face", "") or "").strip(),
    )


@dataclass
class CharLayer:
    """立绘分层表里的一层:文件名 + 在合成画布上的位置 + 缩放。"""

    img: str
    left: float
    top: float
    w: float
    h: float
    scale: float


@dataclass
class CharPart:
    body: CharLayer
    face: CharLayer | None
    scale: float


def load_char_data(source: Path) -> dict:
    with open(source / CHAR_DATA, encoding="utf-8") as handle:
        return json.load(handle)


def _layer(record: dict) -> CharLayer:
    return CharLayer(
        img=str(record.get("img", "")),
        left=float(record.get("left", 0)),
        top=float(record.get("top", 0)),
        w=float(record.get("w", 0)),
        h=float(record.get("h", 0)),
        scale=float(record.get("scale", 0.35)),
    )


def resolve_part(char_data: dict, spec: SpriteSpec) -> CharPart | None:
    """把一条立绘描述解析成 (身体层, 表情层)。

    与源播放器 detail.ux 的 parseCharacterOnly 同一条规则:按 pose 取姿势块,
    dress 取身体变体(缺省用主身体),face 走 face_map 映射,查不到就退回 "01"。
    """
    poses = char_data.get(spec.character)
    if not poses:
        return None
    pose_data = poses.get(spec.pose) or poses.get(next(iter(poses)))
    if not isinstance(pose_data, dict):
        return None
    body_record = (pose_data.get("bodies") or {}).get(spec.dress) if spec.dress else None
    if not body_record:
        body_record = pose_data.get("body")
    if not body_record:
        return None
    body = _layer(body_record)
    face_map = pose_data.get("face_map") or {}
    layer_id = face_map.get(spec.face) or face_map.get("01") or face_map.get("1")
    face: CharLayer | None = None
    if layer_id:
        record = (pose_data.get("faces") or {}).get(layer_id)
        if record:
            face = _layer(record)
    if face is None:
        fallback = face_map.get("01")
        record = (pose_data.get("faces") or {}).get(fallback) if fallback else None
        if record:
            face = _layer(record)
    scale = body.scale or float(pose_data.get("scale", 0.35)) or 0.35
    return CharPart(body=body, face=face, scale=scale)


def sprite_parts(source: Path, refs: Refs) -> dict[str, CharPart]:
    """剧本引用到的每条立绘描述 → 分层(解析不出来的会被跳过)。"""
    char_data = load_char_data(source)
    parts: dict[str, CharPart] = {}
    for raw in sorted(refs.sprite):
        spec = parse_sprite(raw)
        if spec is None:
            continue
        part = resolve_part(char_data, spec)
        if part is None:
            continue
        parts[raw] = part
    return parts


# --------------------------------------------------------------------------
# 章节表(lct.ux 里手写的路线 / 章节页码)
# --------------------------------------------------------------------------

ROUTE_RE = re.compile(r'"(\w+Chapters)":\s*\[(.*?)\n    \]', re.S)
CHAPTER_RE = re.compile(r'\{"line":\s*(\d+),\s*"label":\s*"([^"]+)"\}')


def load_chapters(source: Path) -> list[tuple[str, int, str]]:
    """返回 [(路线键, 起始页, 标签)];路线键如 commonChapters / azuChapters。"""
    text = (source / LCT_PAGE).read_text(encoding="utf-8")
    chapters: list[tuple[str, int, str]] = []
    for match in ROUTE_RE.finditer(text):
        route = match.group(1)
        for line, label in CHAPTER_RE.findall(match.group(2)):
            chapters.append((route, int(line), label))
    return chapters


def route_labels(source: Path) -> dict[str, str]:
    """路线键 → 显示名(与 lct.ux 的界面文案一致)。"""
    text = (source / LCT_PAGE).read_text(encoding="utf-8")
    labels: dict[str, str] = {}
    for key, name in re.findall(r'<text class="mid1">([^<]+)</text>', text):
        labels[key] = name
    mapping = {
        "commonChapters": "共通线",
        "azuChapters": "梓",
        "miuChapters": "美羽",
        "rioChapters": "莉音",
        "eriChapters": "艾莉娜",
        "nicChapters": "尼古拉",
    }
    return mapping


# --------------------------------------------------------------------------
# 分支配置(branchConfig.js)
# --------------------------------------------------------------------------


def load_branch_config(source: Path) -> dict:
    """把 branchConfig.js 解析成 Python 数据(见 tools/dracu_branch.py)。

    这里只做文件读取,真正的 JS 子集解析在 dracu_branch 里,方便单测。
    """
    from dracu_branch import parse_branch_config

    text = (source / BRANCH_CONFIG).read_text(encoding="utf-8")
    return parse_branch_config(text)


def find_source(root: Path) -> Path:
    """校验一个目录确实是 dracu-riot-miband 的 checkout。"""
    root = Path(root)
    missing = [name for name in (SCRIPT_DIR, BG_DIR, EV_DIR, CH_DIR, CHAR_DATA) if not (root / name).exists()]
    if missing:
        raise SystemExit(
            f"{root} 不是 dracu-riot-miband 源目录(缺 {', '.join(missing)})。\n"
            f"先跑: python tools/dracu_fetch_source.py --dest {root}"
        )
    return root


def human_env() -> dict[str, str]:
    """给产物里的 META 留一份环境指纹(便于复现)。"""
    return {"python": os.sys.version.split()[0]}
