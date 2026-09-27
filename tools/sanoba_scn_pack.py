#!/usr/bin/env python3
"""Build the Sanoba Witch script pack (SANOSCN1) for the AI Passport port.

Source project: https://github.com/hrk666666/Sanoba-Witch-MiBand-10
  - 小米手环 9 / 10(Xiaomi Vela / aiot 快应用)上的《魔女的夜宴》移植版;剧本已由上游从
    KiriKiri 的 .ks 转成 JSON 节点数组,每章一个 <id>.<章节名>.ks.txt。
  - 剧本版权归 Yuzusoft 所有,汉化文本版权归暗鸽汉化组;本仓库只保存转换产物
    (main/sanoba_data/),不再分发源素材。先用 tools/sanoba_fetch_source.py 拉源:
      python tools/sanoba_fetch_source.py --dest build/sanoba-source --chunks

The pack is a single little-endian binary read straight out of flash.  The firmware
keeps only one chunk in RAM at a time, so every chunk is an independent raw-deflate
block addressable by (offset, length).  Architecture follows the SENRSCN1 pack of
the sibling port (千恋＊万花) in this same repository; the differences are listed
under "与 SENRSCN1 的差异" below.

  header   : magic "SANOSCN1"(8B), version u16=1, header_size u16,
             section_count u16, reserved u16, total_size u32       (20 B)
  section  : { type u32, offset u32, count u32, size u32 }        (16 B × N)
    SEC_CHAR     u32 count + count × u16 code point。按全局字频降序(同频按码点升序),
                 码值即字符表下标;同一张表同时充当固件字库的字形号 —— 字库子集按本表
                 顺序生成即可零缺字。
    SEC_NAME     字典块连排,顺序固定:speaker, sprite_key, sprite_position,
                 sprite_expression, sprite_outfit, background, event, flag, label。
                 每块 u16 count,随后每条 u16 长度 + u16[长度] 字符码(与 SEC_CHAR 同码表)。
    SEC_SCENARIO u16 count,每项 { first_chunk u16, chunk_count u16, title text }。
                 title 取源文件名去掉 .ks.txt,供固件章节列表显示。
    SEC_CHUNK    u16 count,每块 { data_off u32, data_len u32, raw_len u32, node_count u32 }
                 (u16 count 之后按 16 字节对齐存放)。data_off 相对 SEC_BLOB 数据区,
                 每块 4 字节对齐。
    SEC_BLOB     逐块 raw deflate(zlib wbits=-15,level 9)。
    SEC_LABEL    u32 count,每条 { name_id u32, scenario u16, chunk u16, node u32 }。
                 name_id 指向 label 字典块;chunk/node 是该标签解析后的落点(node 是
                 块内节点下标)。
    SEC_ROUTE    u16 触发场景下标,u16 兜底场景下标,u16 目标数,每条 { flag_id u8,
                 scenario u16 } —— 对应源工程 game.txt 的 flagRoute(019 按好感度旗标
                 选线,全 0 或并列走兜底)。
    SEC_META     key=value UTF-8 文本,每行一条(来源仓库 / ref / 计数 / 语义注记)。

  Per-chunk record stream (decompressed):
    u16 node_count, then records:
      u8 kind
        0 LABEL      u32 label_id
        1 CHAPTER    text
        2 BG         u8 bg index
        3 DIALOGUE   u8 speaker index, text, u8 sprite count, per sprite
                     { u8 key, u8 position, u8 expression, u8 outfit }
        4 SELECT     u8 option count, per option { text, u8 target_kind, u32 label_id,
                     u8 assignment count, per assignment { u8 flag_id, u8 op, u16 value } }
        5 EVENT      u16 =(kind<<14)|index; kind 0=EV 1=SD 2=源里未支持的名字;
                     0xFFFF 表示清空当前事件图
        6 NEXT       u8 target_kind, u32 label_id, u8 condition count,
                     per condition { u8 flag_id, u8 op, u16 value }
        7 SPRITE_OFF 无参(源剧本未用到,格式保留)
    text = u16 字符数 + 该数量的 u16 字符码
    target_kind : 0=标签(label_id 有效)、1=源里本场景未定义的标签(保持当前进度,与源
                  引擎"未找到标签"分支一致)、2=结局
    op          : 0=赋值(=)、1=累加(+= / ++,value 为增量)
  section.count: SEC_CHAR=字符数,SEC_NAME=块数,SEC_SCENARIO=场景数,SEC_CHUNK=块数,
                 SEC_BLOB=块数,SEC_LABEL=标签数,SEC_ROUTE=目标数,SEC_META=0

源数据语义(全部 101 章、61,627 个节点实测,只有 0-6 号节点):
  [0, "*label"]                  标签;名字可含空格/冒号,不做格式假设
  [1, "章节标题"]                章节标题
  [2, "<bg>"]                    切换背景
  [3, spk, text] / [3, spk, text, [[key,"中","03","制服"], …]]
                                 对白;第 4 字段是立绘列表(21,890 条)。源工程没有 ch/
                                 立绘图,所以默认丢弃该列表(--keep-sprites 可保留)。
  [4, [[text, target, expr], …]] 选项;expr 是逗号分隔的赋值列表(f.nen_flag ++ 等)
  [5, "evXXX"/"sdXXX"/null/其它] 事件图显示 / 清空 / 源里不支持的其它名字
  [6, target]                    跳转(本作用不到条件字段,格式保留)
文本必须双射:解出来的 UTF-8 与原文逐字节一致,不做归一化、不漏字。

场景推进(与源引擎一致,固件照此实现):
  - 场景内跳转只认本场景的标签;源里本场景未定义的标签(每章结尾的 *com_part_N、
    *gameend_title 等)按"忽略"处理,由场景顺序继续。
  - 一个场景的节点走完 → 命中 SEC_ROUTE 规则时按旗标选线,否则按 SEC_SCENARIO 的
    分组顺序推进;最后一个场景走完即通关。

与 SENRSCN1 的差异:
  - SEC_FLAG 换成 SEC_LABEL(本作旗标是具名变量且只出现在赋值里,不需要单独的表),
    新增 SEC_SCENARIO / SEC_ROUTE 承载 game.txt 的场景顺序与选线规则。
  - DIALOGUE 带立绘列表(4×u8 下标),不是单个立绘键 + 动作。
  - SELECT 带赋值列表;NEXT 用 (target_kind, label_id) 而不是 (类型,块,页号)。
  - 按解压后字节预算切块(默认 20 KB),不照搬源文件边界 —— 一章可能被切成多块。

Usage:
  python tools/sanoba_scn_pack.py --source build/sanoba-source \
      --out build/sanoba-pack/sanoba_scn.bin \
      --json build/sanoba-pack/sanoba_scn.json
  python tools/sanoba_scn_pack.py --check build/sanoba-pack/sanoba_scn.bin
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

MAGIC = b"SANOSCN1"
VERSION = 1
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
CHUNK_ENTRY = struct.Struct("<IIII")
LABEL_ENTRY = struct.Struct("<IHHI")       # name_id, scenario, chunk, node
assert HEADER.size == 20 and SECTION.size == 16
assert CHUNK_ENTRY.size == 16 and LABEL_ENTRY.size == 12

# 段类型
(SEC_CHAR, SEC_NAME, SEC_SCENARIO, SEC_CHUNK, SEC_BLOB, SEC_LABEL, SEC_ROUTE, SEC_META) = range(8)
SECTION_NAMES = {
    SEC_CHAR: "SEC_CHAR", SEC_NAME: "SEC_NAME", SEC_SCENARIO: "SEC_SCENARIO",
    SEC_CHUNK: "SEC_CHUNK", SEC_BLOB: "SEC_BLOB", SEC_LABEL: "SEC_LABEL",
    SEC_ROUTE: "SEC_ROUTE", SEC_META: "SEC_META",
}

# 记录类型
K_LABEL, K_CHAPTER, K_BG, K_DIALOGUE, K_SELECT, K_EV, K_NEXT, K_SPRITE_OFF = range(8)
KIND_NAMES = {
    K_LABEL: "LABEL", K_CHAPTER: "CHAPTER", K_BG: "BG", K_DIALOGUE: "DIALOGUE",
    K_SELECT: "SELECT", K_EV: "EV", K_NEXT: "NEXT", K_SPRITE_OFF: "SPRITE_OFF",
}

# 跳转目标类型
TARGET_LABEL, TARGET_IGNORED, TARGET_ENDING = range(3)

# 赋值 / 条件操作符
OP_SET, OP_ADD = 0, 1

# 事件图命名空间
EV_KIND_EV, EV_KIND_SD, EV_KIND_UNSUPPORTED = 0, 1, 2
EV_CLEAR = 0xFFFF

# 字典块顺序固定(与 SEC_NAME 一一对应)
NAME_BLOCKS = (
    "speaker", "sprite_key", "sprite_position", "sprite_expression",
    "sprite_outfit", "background", "event", "flag", "label",
)
# 下标是 u8 的块
U8_BLOCKS = (
    "speaker", "sprite_key", "sprite_position", "sprite_expression",
    "sprite_outfit", "background", "flag",
)
ENDING_LABELS = ("*gameend_title", "*endrecollection")

SCN_FILE = re.compile(r"^(?P<id>[^.]*)\.(?P<name>.+)\.ks\.txt$")
ASSIGN_RE = re.compile(r"^\s*f\.([A-Za-z0-9_]+)\s*(=|==|!=|<=|>=|\+=|-=|\+\+|--)\s*(-?\d+)?\s*$")
COND_RE = re.compile(r"^\s*f\.([A-Za-z0-9_]+)\s*(==|!=|<=|>=|<|>)\s*(-?\d+)\s*$")

DEFAULT_RAW_LIMIT = 20000
DEFAULT_BUDGET_MB = 1.60


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def human(count: float) -> str:
    if count >= 1048576:
        return f"{count / 1048576:.2f} MB"
    return f"{count / 1024:.1f} KB"


def percentile(values: list[int], fraction: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(len(ordered) * fraction))]


# --------------------------------------------------------------------------
# 读写原语
# --------------------------------------------------------------------------


class Writer:
    __slots__ = ("buf",)

    def __init__(self) -> None:
        self.buf = bytearray()

    def u8(self, value: int) -> None:
        self.buf += struct.pack("<B", value)

    def u16(self, value: int) -> None:
        self.buf += struct.pack("<H", value)

    def u32(self, value: int) -> None:
        self.buf += struct.pack("<I", value)

    def text(self, codes: list[int]) -> None:
        self.u16(len(codes))
        if codes:
            self.buf += struct.pack("<%dH" % len(codes), *codes)

    def __len__(self) -> int:
        return len(self.buf)


class Reader:
    __slots__ = ("data", "pos")

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def _take(self, count: int) -> None:
        if self.pos + count > len(self.data):
            raise ValueError("记录流提前结束")

    def u8(self) -> int:
        self._take(1)
        value = self.data[self.pos]
        self.pos += 1
        return value

    def u16(self) -> int:
        self._take(2)
        value = struct.unpack_from("<H", self.data, self.pos)[0]
        self.pos += 2
        return value

    def u32(self) -> int:
        self._take(4)
        value = struct.unpack_from("<I", self.data, self.pos)[0]
        self.pos += 4
        return value

    def text(self) -> list[int]:
        length = self.u16()
        self._take(length * 2)
        codes = list(struct.unpack_from("<%dH" % length, self.data, self.pos)) if length else []
        self.pos += length * 2
        return codes

    def done(self) -> bool:
        return self.pos >= len(self.data)


def encode_text(text: str, char_index: dict[int, int]) -> list[int]:
    return [char_index[ord(char)] for char in text]


def decode_text(codes: list[int], char_table: list[int]) -> str:
    return "".join(chr(char_table[code]) for code in codes)


def format_assignments(assignments: list[tuple[str, int, int]]) -> str:
    parts = []
    for name, op, value in assignments:
        if op == OP_SET:
            parts.append(f"f.{name} = {value}")
        elif op == OP_ADD:
            if value == 1:
                parts.append(f"f.{name} ++")
            elif value == -1:
                parts.append(f"f.{name} --")
            else:
                parts.append(f"f.{name} += {value}")
        else:
            raise ValueError(f"未知操作符 {op}")
    return " , ".join(parts)


def format_conditions(conditions: list[tuple[str, int, int]]) -> str:
    return " && ".join(f"f.{name} == {value}" for name, _op, value in conditions)


# --------------------------------------------------------------------------
# 源扫描
# --------------------------------------------------------------------------


@dataclass
class ScenarioPlan:
    index: int
    scn_id: str
    group: str
    title: str
    nodes: list
    first_chunk: int = 0
    chunk_count: int = 0


@dataclass
class Story:
    scenarios: list[ScenarioPlan] = field(default_factory=list)
    chunks: list[bytes] = field(default_factory=list)
    chunk_nodes: list[int] = field(default_factory=list)
    chunk_scenario: list[int] = field(default_factory=list)
    pools: dict[str, list[str]] = field(default_factory=dict)
    pool_index: dict[str, dict[str, int]] = field(default_factory=dict)
    char_table: list[int] = field(default_factory=list)
    char_index: dict[int, int] = field(default_factory=dict)
    flag_names: list[str] = field(default_factory=list)
    flag_index: dict[str, int] = field(default_factory=dict)
    label_defs: list[tuple[str, int]] = field(default_factory=list)     # id → (名字, 场景)
    label_index: dict[tuple[int, str], int] = field(default_factory=dict)
    label_pos: list[tuple[int, int]] = field(default_factory=list)      # id → (chunk, node)
    route: dict | None = None
    counts: collections.Counter = field(default_factory=collections.Counter)
    unknown_events: list[str] = field(default_factory=list)

    def text_codes(self, text: str) -> list[int]:
        return encode_text(text, self.char_index)

    def pool(self, block: str, name: str) -> int:
        return self.pool_index[block][name]

    def add_flag(self, name: str) -> None:
        if name not in self.flag_index:
            self.flag_index[name] = len(self.flag_names)
            self.flag_names.append(name)


def read_game_config(source: Path) -> dict | None:
    path = source / "game.txt"
    if not path.is_file():
        log(f"警告: 没有 {path},场景顺序改用文件名排序,选线规则也不会写入")
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as exc:
        log(f"警告: {path} 读不了({exc}),场景顺序改用文件名排序")
        return None


def scenario_order(source: Path) -> tuple[list[tuple[str, str, str]], dict | None]:
    """返回 [(scn_id, group, 文件名), …];顺序取自 game.txt 的 path→id 映射,
    缺失时按文件名排序并把首个点号前的片段当作 id。"""
    scn_dir = source / "scn"
    if not scn_dir.is_dir():
        sys.exit(f"ERROR: 找不到 {scn_dir}\n先跑: python tools/sanoba_fetch_source.py --dest {source} --chunks")
    files = sorted(path.name for path in scn_dir.glob("*.ks.txt"))
    if not files:
        sys.exit(f"ERROR: {scn_dir} 里没有 *.ks.txt")

    config = read_game_config(source)
    order: list[tuple[str, str, str]] = []
    placed: set[str] = set()
    if config and isinstance(config.get("scenarios"), dict):
        for group, items in config["scenarios"].items():
            for item in items:
                file_name = str(item.get("path", ""))
                if file_name in files and file_name not in placed:
                    order.append((str(item.get("id")), str(group), file_name))
                    placed.add(file_name)
    if len(placed) != len(files):
        missing = [name for name in files if name not in placed]
        log(f"警告: game.txt 没覆盖 {len(missing)} 个剧本文件,补在末尾(按文件名推 id): {missing[:5]}")
        for file_name in missing:
            order.append((scenario_id_of(file_name), "extra", file_name))
    return order, config


def strip_extension(file_name: str) -> str:
    return file_name[: -len(".ks.txt")] if file_name.endswith(".ks.txt") else file_name


def scenario_id_of(file_name: str) -> str:
    """文件名里首个点号前的片段(game.txt 缺失时兜底用)。"""
    match = SCN_FILE.match(file_name)
    return match.group("id") if match else file_name


def parse_assignments(expr: str) -> list[tuple[str, int, int]]:
    """把 "f.sel_flag = 0 , f.meg_flag ++" 解析成 [(旗标名, 操作, 值), …]。"""
    out: list[tuple[str, int, int]] = []
    if not expr or not expr.strip():
        return out
    for part in expr.split(","):
        match = ASSIGN_RE.match(part)
        if not match:
            raise ValueError(f"不认识的赋值片段: {part!r}")
        name, operator, value = match.group(1), match.group(2), match.group(3)
        if operator == "=":
            out.append((name, OP_SET, int(value or 0)))
        elif operator == "++":
            out.append((name, OP_ADD, 1))
        elif operator == "--":
            out.append((name, OP_ADD, -1))
        elif operator in ("+=", "-="):
            amount = int(value or 0)
            out.append((name, OP_ADD, amount if operator == "+=" else -amount))
        else:
            raise ValueError(f"不支持的赋值操作符: {part!r}")
    return out


def parse_condition(expr: str) -> list[tuple[str, int, int]]:
    """本作剧本没有条件表达式;格式保留,遇到就按 f.x == n 解析。"""
    if not expr or not expr.strip():
        return []
    out: list[tuple[str, int, int]] = []
    for term in expr.split("&&"):
        match = COND_RE.match(term)
        if not match:
            raise ValueError(f"不认识的条件片段: {term!r}")
        out.append((match.group(1), OP_SET, int(match.group(3))))
    return out


def event_kind(name: str) -> int:
    if name.startswith("sd"):
        return EV_KIND_SD
    if name.startswith("ev"):
        return EV_KIND_EV
    return EV_KIND_UNSUPPORTED


def scan_source(source: Path) -> tuple[Story, list[str]]:
    """读全部剧本:场景表、字典、字表、标签表、选线规则与全局字频。"""
    order, config = scenario_order(source)
    story = Story()
    pools: dict[str, set[str]] = {block: set() for block in NAME_BLOCKS}
    frequency: collections.Counter = collections.Counter()
    labels_defined: list[tuple[str, int]] = []

    def count_text(text: str) -> None:
        frequency.update(text)

    for index, (scn_id, group, file_name) in enumerate(order):
        nodes = json.loads((source / "scn" / file_name).read_text(encoding="utf-8"))
        if not isinstance(nodes, list):
            sys.exit(f"ERROR: {file_name} 不是节点数组")
        story.scenarios.append(
            ScenarioPlan(index=index, scn_id=scn_id, group=group, title=strip_extension(file_name), nodes=nodes)
        )
        count_text(strip_extension(file_name))
        for number, node in enumerate(nodes):
            if not isinstance(node, list) or not node:
                sys.exit(f"ERROR: {file_name} 第 {number} 个节点不是数组: {node!r}")
            kind = node[0]
            story.counts["nodes"] += 1
            story.counts[f"kind_{KIND_NAMES.get(kind, kind)}"] += 1
            if kind == K_LABEL:
                name = str(node[1])
                labels_defined.append((name, index))
                pools["label"].add(name)
                story.counts["labels"] += 1
            elif kind == K_CHAPTER:
                count_text(str(node[1]))
                story.counts["chapters"] += 1
            elif kind == K_BG:
                pools["background"].add(str(node[1]))
                story.counts["bg"] += 1
            elif kind == K_DIALOGUE:
                pools["speaker"].add(str(node[1]))
                count_text(str(node[2]))
                story.counts["dialogue"] += 1
                story.counts["dialogue_chars"] += len(str(node[2]))
                if len(node) > 3 and node[3]:
                    if not isinstance(node[3], list):
                        sys.exit(f"ERROR: {file_name} 第 {number} 个节点的立绘字段不是数组")
                    story.counts["dialogue_with_sprites"] += 1
                    for sprite in node[3]:
                        if not isinstance(sprite, list) or len(sprite) != 4:
                            sys.exit(f"ERROR: {file_name} 第 {number} 个立绘条目异常: {sprite!r}")
                        pools["sprite_key"].add(str(sprite[0]))
                        pools["sprite_position"].add(str(sprite[1]))
                        pools["sprite_expression"].add(str(sprite[2]))
                        pools["sprite_outfit"].add(str(sprite[3]))
                        story.counts["sprites"] += 1
            elif kind == K_SELECT:
                for option in node[1]:
                    if not isinstance(option, list) or len(option) < 2:
                        sys.exit(f"ERROR: {file_name} 第 {number} 个选项异常: {option!r}")
                    count_text(str(option[0]))
                    story.counts["options"] += 1
                    if len(option) > 2 and option[2]:
                        for name, _op, _value in parse_assignments(str(option[2])):
                            story.counts["assignments"] += 1
                            story.add_flag(name)
                    if len(option) > 3 and option[3]:
                        sys.exit(f"ERROR: {file_name} 第 {number} 个选项带条件字段,格式未支持")
                story.counts["selects"] += 1
            elif kind == K_EV:
                name = node[1] if len(node) > 1 else None
                if name is None:
                    story.counts["ev_clear"] += 1
                else:
                    name = str(name)
                    pools["event"].add(name)
                    story.counts["ev"] += 1
                    if event_kind(name) == EV_KIND_UNSUPPORTED and name not in story.unknown_events:
                        story.unknown_events.append(name)
            elif kind == K_NEXT:
                story.counts["next"] += 1
                if len(node) > 2 and node[2]:
                    for name, _op, _value in parse_condition(str(node[2])):
                        story.add_flag(name)
                        story.counts["conditions"] += 1
            elif kind == K_SPRITE_OFF:
                story.counts["sprite_off"] += 1
            else:
                sys.exit(f"ERROR: {file_name} 第 {number} 个节点类型 {kind} 未支持: {node!r}")

    # 标签按定义顺序编号;(场景, 名字) → id,同名的跨文件标签各自占一条
    for label_id, (name, scenario_index) in enumerate(labels_defined):
        story.label_defs.append((name, scenario_index))
        story.label_index[(scenario_index, name)] = label_id

    # 字表:全局字频降序,同频按码点升序,保证可复现。字典名也要进表 ——
    # SEC_NAME 里的名字用同一张码表存储
    pools["flag"].update(story.flag_names)
    for block in NAME_BLOCKS:
        for name in pools[block]:
            count_text(name)
    ordered_chars = sorted(frequency, key=lambda char: (-frequency[char], ord(char)))
    story.char_table = [ord(char) for char in ordered_chars]
    if len(story.char_table) > 0x10000:
        sys.exit(f"ERROR: 码表 {len(story.char_table)} 项超过 u16 下标上限")
    story.char_index = {point: index for index, point in enumerate(story.char_table)}
    story.pools = {block: sorted(pools[block]) for block in NAME_BLOCKS}
    story.pool_index = {block: {name: index for index, name in enumerate(story.pools[block])} for block in NAME_BLOCKS}
    # 旗标字典必须与记录里的 flag_id 同序:flag_id 是首次出现顺序,不是字典序
    story.pools["flag"] = list(story.flag_names)
    story.pool_index["flag"] = {name: index for index, name in enumerate(story.flag_names)}

    # 选线规则:只支持源工程实际用到的 flagRoute
    if config and isinstance(config.get("routes"), dict):
        by_id = {scenario.scn_id: scenario.index for scenario in story.scenarios}
        for trigger, rule in config["routes"].items():
            if str(rule.get("type")) != "flagRoute":
                sys.exit(f"ERROR: 不支持的 route 类型: {rule.get('type')!r}")
            if str(trigger) not in by_id:
                sys.exit(f"ERROR: route 触发场景 {trigger} 不在场景表里")
            fallback = str(rule.get("fallback", ""))
            if fallback not in by_id:
                sys.exit(f"ERROR: route 兜底场景 {fallback} 不在场景表里")
            targets: list[tuple[str, int]] = []
            for target in rule.get("targets", []):
                flag = str(target.get("flag"))
                if str(target.get("next")) not in by_id:
                    sys.exit(f"ERROR: route 目标 {target!r} 不在场景表里")
                story.add_flag(flag)
                targets.append((flag, by_id[str(target["next"])]))
            story.route = {"trigger": by_id[str(trigger)], "fallback": by_id[fallback], "targets": targets}

    # 容量护栏:宁可在打包时失败,也不静默截断
    for block in U8_BLOCKS:
        if len(story.pools[block]) > 0x100:
            sys.exit(f"ERROR: {block} 字典 {len(story.pools[block])} 项超过 256,u8 下标放不下")
    if len(story.pools["event"]) > 0x4000:
        sys.exit(f"ERROR: event 字典 {len(story.pools['event'])} 项超过 16384(记录里只留 14 位)")
    if len(story.pools["label"]) > 0x10000:
        sys.exit(f"ERROR: label 字典超过 u16")
    if len(story.flag_names) > 0x100:
        sys.exit(f"ERROR: 旗标 {len(story.flag_names)} 个超过 255")
    if story.counts["nodes"] > 0xFFFFFFFF:
        sys.exit("ERROR: 节点数超过 u32")
    notes = []
    if story.unknown_events:
        notes.append("unsupported_events=" + ",".join(story.unknown_events))
    return story, notes


# --------------------------------------------------------------------------
# 记录编码 / 解码
# --------------------------------------------------------------------------


def resolve_target(story: Story, scenario_index: int, target: str) -> tuple[int, int]:
    """目标解析:只认本场景标签(与源引擎 choose() 的分支一致)。"""
    if target in ENDING_LABELS:
        return TARGET_ENDING, ENDING_LABELS.index(target)
    label_id = story.label_index.get((scenario_index, target))
    if label_id is None:
        return TARGET_IGNORED, 0
    return TARGET_LABEL, label_id


def encode_node(writer: Writer, node: list, scenario: ScenarioPlan, story: Story, keep_sprites: bool) -> None:
    kind = node[0]
    writer.u8(kind)
    if kind == K_LABEL:
        writer.u32(story.label_index[(scenario.index, str(node[1]))])
    elif kind == K_CHAPTER:
        writer.text(story.text_codes(str(node[1])))
    elif kind == K_BG:
        writer.u8(story.pool("background", str(node[1])))
    elif kind == K_DIALOGUE:
        writer.u8(story.pool("speaker", str(node[1])))
        writer.text(story.text_codes(str(node[2])))
        sprites = node[3] if keep_sprites and len(node) > 3 and node[3] else []
        writer.u8(len(sprites))
        for sprite in sprites:
            writer.u8(story.pool("sprite_key", str(sprite[0])))
            writer.u8(story.pool("sprite_position", str(sprite[1])))
            writer.u8(story.pool("sprite_expression", str(sprite[2])))
            writer.u8(story.pool("sprite_outfit", str(sprite[3])))
    elif kind == K_SELECT:
        options = node[1]
        writer.u8(len(options))
        for option in options:
            writer.text(story.text_codes(str(option[0])))
            target_kind, label_id = resolve_target(story, scenario.index, str(option[1]))
            writer.u8(target_kind)
            writer.u32(label_id)
            assignments = parse_assignments(str(option[2])) if len(option) > 2 and option[2] else []
            writer.u8(len(assignments))
            for name, op, value in assignments:
                writer.u8(story.flag_index[name])
                writer.u8(op)
                writer.u16(value & 0xFFFF)
    elif kind == K_EV:
        name = node[1] if len(node) > 1 else None
        if name is None:
            writer.u16(EV_CLEAR)
        else:
            name = str(name)
            writer.u16((event_kind(name) << 14) | story.pool("event", name))
    elif kind == K_NEXT:
        target_kind, label_id = resolve_target(story, scenario.index, str(node[1]))
        writer.u8(target_kind)
        writer.u32(label_id)
        conditions = parse_condition(str(node[2])) if len(node) > 2 and node[2] else []
        writer.u8(len(conditions))
        for name, op, value in conditions:
            writer.u8(story.flag_index[name])
            writer.u8(op)
            writer.u16(value & 0xFFFF)
    elif kind == K_SPRITE_OFF:
        pass
    else:
        sys.exit(f"ERROR: 未支持的节点类型 {kind}: {node!r}")


class Context:
    """解码用只读上下文(字表 / 字典 / 标签)。"""

    def __init__(self, char_table: list[int], pools: dict[str, list[str]], labels: list[tuple[int, int, int, int]]) -> None:
        self.char_table = char_table
        self.pools = pools
        self.labels = labels      # (name_id, scenario, chunk, node)

    def text(self, reader: Reader) -> str:
        return decode_text(reader.text(), self.char_table)

    def pool(self, block: str, index: int) -> str:
        return self.pools[block][index]

    def label_name(self, label_id: int) -> str:
        return self.pools["label"][self.labels[label_id][0]]

    def flag_name(self, flag_id: int) -> str:
        return self.pools["flag"][flag_id]

    def target_name(self, target_kind: int, label_id: int) -> str:
        if target_kind == TARGET_LABEL:
            return self.label_name(label_id)
        if target_kind == TARGET_ENDING:
            return ENDING_LABELS[label_id] if 0 <= label_id < len(ENDING_LABELS) else ENDING_LABELS[0]
        return None    # TARGET_IGNORED:源引擎里就是"未找到标签"


def decode_node(reader: Reader, ctx: Context) -> list:
    kind = reader.u8()
    if kind == K_LABEL:
        return [K_LABEL, ctx.label_name(reader.u32())]
    if kind == K_CHAPTER:
        return [K_CHAPTER, ctx.text(reader)]
    if kind == K_BG:
        return [K_BG, ctx.pool("background", reader.u8())]
    if kind == K_DIALOGUE:
        speaker = ctx.pool("speaker", reader.u8())
        text = ctx.text(reader)
        count = reader.u8()
        node: list = [K_DIALOGUE, speaker, text]
        if count:
            sprites = []
            for _ in range(count):
                sprites.append([
                    ctx.pool("sprite_key", reader.u8()),
                    ctx.pool("sprite_position", reader.u8()),
                    ctx.pool("sprite_expression", reader.u8()),
                    ctx.pool("sprite_outfit", reader.u8()),
                ])
            node.append(sprites)
        return node
    if kind == K_SELECT:
        count = reader.u8()
        options = []
        for _ in range(count):
            text = ctx.text(reader)
            target_kind = reader.u8()
            label_id = reader.u32()
            assignments = []
            for _ in range(reader.u8()):
                flag_id = reader.u8()
                op = reader.u8()
                value = reader.u16()
                assignments.append((ctx.flag_name(flag_id), op, value))
            options.append([text, ctx.target_name(target_kind, label_id), format_assignments(assignments)])
        return [K_SELECT, options]
    if kind == K_EV:
        value = reader.u16()
        if value == EV_CLEAR:
            return [K_EV, None]
        return [K_EV, ctx.pool("event", value & 0x3FFF)]
    if kind == K_NEXT:
        target_kind = reader.u8()
        label_id = reader.u32()
        conditions = []
        for _ in range(reader.u8()):
            flag_id = reader.u8()
            op = reader.u8()
            value = reader.u16()
            conditions.append((ctx.flag_name(flag_id), op, value))
        node = [K_NEXT, ctx.target_name(target_kind, label_id)]
        if conditions:
            node.append(format_conditions(conditions))
        return node
    if kind == K_SPRITE_OFF:
        return [K_SPRITE_OFF]
    raise ValueError(f"未知记录类型 {kind}")


# --------------------------------------------------------------------------
# 分块与序列化
# --------------------------------------------------------------------------


def build_chunks(story: Story, raw_limit: int, keep_sprites: bool) -> None:
    """按解压后字节预算切块,并回填标签落点与场景的块区间。"""
    positions: dict[int, tuple[int, int]] = {}
    for scenario in story.scenarios:
        body = Writer()
        node_count = 0
        scenario.first_chunk = len(story.chunks)
        for node in scenario.nodes:
            record = Writer()
            encode_node(record, node, scenario, story, keep_sprites)
            if node_count and len(body) + len(record) > raw_limit:
                story.chunks.append(bytes(body.buf))
                story.chunk_nodes.append(node_count)
                story.chunk_scenario.append(scenario.index)
                body = Writer()
                node_count = 0
            if node[0] == K_LABEL:
                positions[story.label_index[(scenario.index, str(node[1]))]] = (len(story.chunks), node_count)
            body.buf += record.buf
            node_count += 1
        if node_count:
            story.chunks.append(bytes(body.buf))
            story.chunk_nodes.append(node_count)
            story.chunk_scenario.append(scenario.index)
        scenario.chunk_count = len(story.chunks) - scenario.first_chunk
    if len(story.chunks) > 0xFFFF:
        sys.exit(f"ERROR: 块数 {len(story.chunks)} 超过 u16")
    story.label_pos = [positions.get(label_id, (0, 0)) for label_id in range(len(story.label_defs))]


def pad4(payload: bytes) -> bytes:
    return payload + b"\0" * ((-len(payload)) % 4)


def serialize(story: Story, meta_lines: list[str], raw_limit: int, keep_sprites: bool) -> bytes:
    build_chunks(story, raw_limit, keep_sprites)

    # SEC_BLOB:逐块 raw deflate,4 字节对齐;同时算好 CHUNK 表
    compressed: list[bytes] = []
    offsets: list[int] = []
    cursor = 0
    for index, body in enumerate(story.chunks):
        raw = struct.pack("<H", story.chunk_nodes[index]) + body
        block = zlib.compress(raw, 9)[2:-4]
        offsets.append(cursor)
        compressed.append(block)
        cursor += len(block) + ((-len(block)) % 4)
    blob = b"".join(pad4(block) for block in compressed)

    chunk_writer = Writer()
    chunk_writer.u16(len(story.chunks))
    for index, block in enumerate(compressed):
        raw_len = 2 + len(story.chunks[index])
        chunk_writer.buf += CHUNK_ENTRY.pack(offsets[index], len(block), raw_len, story.chunk_nodes[index])

    char_writer = Writer()
    char_writer.u32(len(story.char_table))
    for code in story.char_table:
        char_writer.u16(code)

    name_writer = Writer()
    name_writer.u16(len(NAME_BLOCKS))
    for block in NAME_BLOCKS:
        names = story.pools[block]
        name_writer.u16(len(names))
        for name in names:
            name_writer.text(story.text_codes(name))

    scenario_writer = Writer()
    scenario_writer.u16(len(story.scenarios))
    for scenario in story.scenarios:
        scenario_writer.u16(scenario.first_chunk)
        scenario_writer.u16(scenario.chunk_count)
        scenario_writer.text(story.text_codes(scenario.title))

    label_writer = Writer()
    label_writer.u32(len(story.label_defs))
    for label_id, (name, scenario_index) in enumerate(story.label_defs):
        chunk, node = story.label_pos[label_id]
        label_writer.buf += LABEL_ENTRY.pack(story.pool("label", name), scenario_index, chunk, node)

    route_writer = Writer()
    if story.route:
        route_writer.u16(story.route["trigger"])
        route_writer.u16(story.route["fallback"])
        route_writer.u16(len(story.route["targets"]))
        for flag, scenario_index in story.route["targets"]:
            route_writer.u8(story.flag_index[flag])
            route_writer.u16(scenario_index)
    else:
        route_writer.u16(0)
        route_writer.u16(0)
        route_writer.u16(0)

    meta_writer = Writer()
    meta_writer.buf += ("\n".join(meta_lines) + "\n").encode("utf-8")

    sections = [
        (SEC_CHAR, bytes(char_writer.buf), len(story.char_table)),
        (SEC_NAME, bytes(name_writer.buf), len(NAME_BLOCKS)),
        (SEC_SCENARIO, bytes(scenario_writer.buf), len(story.scenarios)),
        (SEC_CHUNK, bytes(chunk_writer.buf), len(story.chunks)),
        (SEC_BLOB, blob, len(story.chunks)),
        (SEC_LABEL, bytes(label_writer.buf), len(story.label_defs)),
        (SEC_ROUTE, bytes(route_writer.buf), len(story.route["targets"]) if story.route else 0),
        (SEC_META, bytes(meta_writer.buf), 0),
    ]

    header_size = HEADER.size + SECTION.size * len(sections)
    offset = header_size
    table = []
    body = bytearray()
    for section_type, payload, count in sections:
        pad = (-offset) % 4
        if pad:
            body += b"\0" * pad
            offset += pad
        table.append((section_type, offset, count, len(payload)))
        body += payload
        offset += len(payload)
    total = header_size + len(body)

    head = bytearray()
    head += HEADER.pack(MAGIC, VERSION, header_size, len(sections), 0, total)
    for section_type, position, count, size in table:
        head += SECTION.pack(section_type, position, count, size)
    return bytes(head) + bytes(body)


def build_meta(story: Story, source: Path, ref: str, keep_sprites: bool, raw_limit: int) -> list[str]:
    lines = [
        "format=SANOSCN1",
        "version=1",
        "source_repo=https://github.com/hrk666666/Sanoba-Witch-MiBand-10",
        f"source_ref={ref}",
        f"source_dir={source.as_posix()}",
        "deflate=raw, one block per chunk, level 9",
        f"raw_limit={raw_limit}",
        f"sprites={'kept' if keep_sprites else 'dropped'}",
        f"scenarios={len(story.scenarios)}",
        f"chunks={len(story.chunks)}",
        f"nodes={story.counts['nodes']}",
        f"dialogue={story.counts['dialogue']}",
        f"dialogue_characters={story.counts['dialogue_chars']}",
        f"characters={len(story.char_table)}",
        f"labels={len(story.label_defs)}",
        f"flags={len(story.flag_names)}",
        f"options={story.counts['options']}",
        f"events={story.counts['ev']}",
        f"event_clear={story.counts['ev_clear']}",
        f"bg={story.counts['bg']}",
        f"chapters={story.counts['chapters']}",
        f"target_enum=0=label,1=ignored,2=ending",
        f"op_enum=0=set,1=add",
        f"event_enum=0=EV,1=SD,2=unsupported;0xFFFF=clear",
        "name_blocks=" + ",".join(NAME_BLOCKS),
        "text_encoding=u16-count + u16 char-table indices, bijective",
        "font=generate the glyph subset in SEC_CHAR order for missing-glyph-free rendering",
    ]
    for block in NAME_BLOCKS:
        lines.append(f"block_{block}={len(story.pools[block])}")
    if story.unknown_events:
        lines.append("unsupported_events=" + ",".join(story.unknown_events))
    if story.route:
        lines.append(f"route_trigger_scenario={story.scenarios[story.route['trigger']].scn_id}")
        lines.append(f"route_fallback_scenario={story.scenarios[story.route['fallback']].scn_id}")
    return lines


# --------------------------------------------------------------------------
# 读回 / 自检
# --------------------------------------------------------------------------


class Pack:
    def __init__(self, raw: bytes) -> None:
        if len(raw) < HEADER.size:
            raise ValueError("文件太短")
        magic, version, header_size, section_count, _reserved, total = HEADER.unpack_from(raw)
        if magic != MAGIC:
            raise ValueError(f"魔数不符: {magic!r}")
        if version != VERSION:
            raise ValueError(f"版本不支持: {version}")
        if total != len(raw):
            raise ValueError(f"头里写 {total} 字节,实际 {len(raw)}")
        if header_size != HEADER.size + SECTION.size * section_count:
            raise ValueError(f"header_size {header_size} 与段数 {section_count} 不一致")
        self.raw = raw
        self.sections: dict[int, tuple[int, int, int]] = {}
        for index in range(section_count):
            section_type, offset, count, size = SECTION.unpack_from(raw, HEADER.size + SECTION.size * index)
            self.sections[section_type] = (offset, count, size)

        char_off, char_count, _ = self.sections[SEC_CHAR]
        self.char_table = list(struct.unpack_from("<%dH" % char_count, raw, char_off + 4))
        if len(self.char_table) != char_count:
            raise ValueError("字符表长度不符")

        pool_off, _block_count, _ = self.sections[SEC_NAME]
        reader = Reader(raw[pool_off:pool_off + self.sections[SEC_NAME][2]])
        block_count = reader.u16()
        if block_count != len(NAME_BLOCKS):
            raise ValueError(f"字典块数 {block_count} != {len(NAME_BLOCKS)}")
        self.pools: dict[str, list[str]] = {}
        for block in NAME_BLOCKS:
            names = []
            for _ in range(reader.u16()):
                names.append(decode_text(reader.text(), self.char_table))
            self.pools[block] = names

        scenario_off, scenario_count, _ = self.sections[SEC_SCENARIO]
        reader = Reader(raw[scenario_off:scenario_off + self.sections[SEC_SCENARIO][2]])
        if reader.u16() != scenario_count:
            raise ValueError("场景表计数与段计数不一致")
        self.scenarios = []
        for _ in range(scenario_count):
            first_chunk = reader.u16()
            chunk_count = reader.u16()
            title = decode_text(reader.text(), self.char_table)
            self.scenarios.append({"first_chunk": first_chunk, "chunk_count": chunk_count, "title": title})

        chunk_off, chunk_count, _ = self.sections[SEC_CHUNK]
        reader = Reader(raw[chunk_off:chunk_off + self.sections[SEC_CHUNK][2]])
        total = reader.u16()
        if total != chunk_count:
            raise ValueError(f"块表计数 {total} != 段计数 {chunk_count}")
        self.chunks = []
        for _ in range(chunk_count):
            data_off = reader.u32()
            data_len = reader.u32()
            raw_len = reader.u32()
            node_count = reader.u32()
            self.chunks.append({"data_off": data_off, "data_len": data_len, "raw_len": raw_len, "node_count": node_count})

        blob_off, _blob_count, blob_size = self.sections[SEC_BLOB]
        self.blob = raw[blob_off:blob_off + blob_size]

        label_off, label_count, _ = self.sections[SEC_LABEL]
        self.labels = []
        for index in range(label_count):
            name_id, scenario, chunk, node = LABEL_ENTRY.unpack_from(raw, label_off + 4 + LABEL_ENTRY.size * index)
            self.labels.append((name_id, scenario, chunk, node))

        route_off, route_count, _ = self.sections[SEC_ROUTE]
        reader = Reader(raw[route_off:route_off + self.sections[SEC_ROUTE][2]])
        trigger = reader.u16()
        fallback = reader.u16()
        targets = []
        for _ in range(reader.u16()):
            targets.append((reader.u8(), reader.u16()))
        self.route = {"trigger": trigger, "fallback": fallback, "targets": targets, "count": route_count}

        meta_off, _meta_count, meta_size = self.sections[SEC_META]
        self.meta: dict[str, str] = {}
        for line in raw[meta_off:meta_off + meta_size].decode("utf-8").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                self.meta[key] = value

        self.context = Context(self.char_table, self.pools, self.labels)

    # ---- 访问 ----
    def decompress(self, index: int) -> bytes:
        entry = self.chunks[index]
        block = self.blob[entry["data_off"]:entry["data_off"] + entry["data_len"]]
        raw = zlib.decompress(block, -15)
        if len(raw) != entry["raw_len"]:
            raise ValueError(f"块 {index} 解压 {len(raw)} 字节,头里写 {entry['raw_len']}")
        return raw

    def records(self, index: int) -> list:
        raw = self.decompress(index)
        reader = Reader(raw)
        count = reader.u16()
        nodes = [decode_node(reader, self.context) for _ in range(count)]
        if not reader.done():
            raise ValueError(f"块 {index} 解压流还剩 {len(raw) - reader.pos} 字节未消费")
        if count != self.chunks[index]["node_count"]:
            raise ValueError(f"块 {index} 节点数 {count} != 头里 {self.chunks[index]['node_count']}")
        return nodes

    def all_nodes(self) -> list[list]:
        out: list[list] = []
        for index in range(len(self.chunks)):
            out.extend(self.records(index))
        return out

    def scenario_nodes(self, scenario: dict) -> list[list]:
        out: list[list] = []
        for index in range(scenario["first_chunk"], scenario["first_chunk"] + scenario["chunk_count"]):
            out.extend(self.records(index))
        return out

    # ---- 结构自检 ----
    def validate_structure(self) -> list[str]:
        problems: list[str] = []
        if len(set(self.char_table)) != len(self.char_table):
            problems.append("字符表里有重复码点")
        if len(self.char_table) > 0x10000:
            problems.append("字符表超过 u16 下标上限")
        for block, names in self.pools.items():
            if len(names) != len(set(names)):
                problems.append(f"{block} 字典有重复项")
        if sum(scenario["chunk_count"] for scenario in self.scenarios) != len(self.chunks):
            problems.append("场景块数与总块数不一致")
        expected = 0
        for scenario in self.scenarios:
            if scenario["first_chunk"] != expected:
                problems.append(f"场景块区间不连续: {scenario['title']}")
            expected += scenario["chunk_count"]
        for index, entry in enumerate(self.chunks):
            if entry["data_off"] % 4:
                problems.append(f"块 {index} 起点未 4 字节对齐")
            if entry["data_off"] + entry["data_len"] > len(self.blob):
                problems.append(f"块 {index} 越界")
            if entry["node_count"] == 0:
                problems.append(f"块 {index} 节点数为 0")
        for label_id, (name_id, scenario, chunk, node) in enumerate(self.labels):
            if name_id >= len(self.pools["label"]):
                problems.append(f"标签 {label_id} 的名字下标越界")
            if scenario >= len(self.scenarios):
                problems.append(f"标签 {label_id} 的场景下标越界")
            if chunk >= len(self.chunks):
                problems.append(f"标签 {label_id} 的块下标越界")
            elif node >= self.chunks[chunk]["node_count"]:
                problems.append(f"标签 {label_id} 的节点下标越界")
        if self.route["count"] != len(self.route["targets"]):
            problems.append("选线目标计数不一致")
        if self.route["targets"]:
            if self.route["trigger"] >= len(self.scenarios) or self.route["fallback"] >= len(self.scenarios):
                problems.append("选线触发/兜底场景越界")
            for flag_id, scenario in self.route["targets"]:
                if flag_id >= len(self.pools["flag"]):
                    problems.append("选线旗标下标越界")
                if scenario >= len(self.scenarios):
                    problems.append("选线目标场景越界")
        try:
            for index in range(len(self.chunks)):
                self.records(index)
        except ValueError as exc:
            problems.append(f"记录解码失败: {exc}")
        return problems


def read_pack(path: Path) -> Pack:
    return Pack(path.read_bytes())


def check_pack(path: Path) -> int:
    try:
        pack = read_pack(path)
    except (ValueError, OSError) as exc:
        log(f"ERROR: {path} 读不了: {exc}")
        return 1
    problems = pack.validate_structure()
    log(f"包 {path}: {human(len(pack.raw))},场景 {len(pack.scenarios)},块 {len(pack.chunks)},"
        f"字符 {len(pack.char_table)},标签 {len(pack.labels)},旗标 {len(pack.pools['flag'])}")
    if problems:
        for problem in problems:
            log(f"  ✗ {problem}")
        return 1
    log("  ✓ 结构自检通过")
    return 0


# --------------------------------------------------------------------------
# 报告 / 入口
# --------------------------------------------------------------------------


def report(pack: Pack, story: Story, args: argparse.Namespace) -> None:
    raw_lengths = [entry["raw_len"] for entry in pack.chunks]
    comp_lengths = [entry["data_len"] for entry in pack.chunks]
    nodes = [entry["node_count"] for entry in pack.chunks]
    source_bytes = sum(
        len(json.dumps(scenario.nodes, ensure_ascii=False).encode("utf-8")) for scenario in story.scenarios
    )
    log("")
    log(f"源 JSON(紧凑重排) {human(source_bytes)} → 记录流 {human(sum(raw_lengths))} → 包内 {human(sum(comp_lengths))}"
        f"(整体 {human(len(pack.raw))})")
    log(f"块: {len(pack.chunks)} 个,解压后最大 {human(max(raw_lengths))} / p90 {human(percentile(raw_lengths, 0.9))}"
        f" / 平均 {human(sum(raw_lengths) / len(raw_lengths))};压缩后最大 {human(max(comp_lengths))}")
    log(f"节点/块: 最大 {max(nodes)} / 平均 {sum(nodes) // len(nodes)};压缩率 {sum(raw_lengths) / sum(comp_lengths):.2f}x")
    log(f"字典: " + ", ".join(f"{block}={len(pack.pools[block])}" for block in NAME_BLOCKS))
    log(f"节点: 对白 {story.counts['dialogue']}({story.counts['dialogue_chars']} 字),选项 {story.counts['options']},"
        f"事件图 {story.counts['ev']},背景切换 {story.counts['bg']},跳转 {story.counts['next']}")
    if not args.keep_sprites:
        log(f"立绘: 已丢弃 {story.counts['sprites']} 条(源工程没有 ch/ 立绘图;要保留用 --keep-sprites)")
    budget = args.budget_mb * 1048576
    verdict = "放得下" if len(pack.raw) <= budget else "超出"
    log(f"预算: 剧本包 {human(len(pack.raw))} vs 预算 {args.budget_mb:.2f} MB → {verdict}"
        f"{' 余 ' + human(int(budget - len(pack.raw))) if len(pack.raw) <= budget else ' 超 ' + human(int(len(pack.raw) - budget))}")


def build(args: argparse.Namespace) -> int:
    source = Path(args.source)
    story, _notes = scan_source(source)
    ref = "unknown"
    manifest = source / "MANIFEST.json"
    if manifest.is_file():
        try:
            ref = str(json.loads(manifest.read_text(encoding="utf-8")).get("ref", "unknown"))
        except (json.JSONDecodeError, OSError):
            pass
    meta_lines = build_meta(story, source, ref, args.keep_sprites, args.raw_limit)
    blob = serialize(story, meta_lines, args.raw_limit, args.keep_sprites)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(blob)
    pack = Pack(blob)
    problems = pack.validate_structure()
    if problems:
        for problem in problems:
            log(f"  ✗ {problem}")
        return 1
    report(pack, story, args)
    log(f"写出 {out_path}")
    if args.json:
        payload = {
            "meta": pack.meta,
            "scenarios": [
                {"id": scenario.scn_id, "group": scenario.group, "title": scenario.title,
                 "first_chunk": scenario.first_chunk, "chunk_count": scenario.chunk_count}
                for scenario in story.scenarios
            ],
            "labels": [
                {"id": label_id, "name": name, "scn": story.scenarios[scenario_index].scn_id, "chunk": chunk, "node": node}
                for label_id, (name, scenario_index) in enumerate(story.label_defs)
                for chunk, node in [story.label_pos[label_id]]
            ],
            "chunks": [
                {"index": index, "scenario": story.scenarios[story.chunk_scenario[index]].scn_id,
                 "nodes": entry["node_count"], "raw": entry["raw_len"], "packed": entry["data_len"]}
                for index, entry in enumerate(pack.chunks)
            ],
        }
        json_path = Path(args.json)
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        log(f"写出清单 {json_path}")
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", default="build/sanoba-source", help="源素材目录(默认 build/sanoba-source)")
    parser.add_argument("--out", default="build/sanoba-pack/sanoba_scn.bin", help="输出剧本包")
    parser.add_argument("--json", help="可选:同时写一份可读清单")
    parser.add_argument("--check", metavar="PACK", help="只做只读自检")
    parser.add_argument("--raw-limit", type=int, default=DEFAULT_RAW_LIMIT, help=f"单块解压后字节预算(默认 {DEFAULT_RAW_LIMIT})")
    parser.add_argument("--keep-sprites", action="store_true", help="保留对白里的立绘列表(源工程没有立绘图)")
    parser.add_argument("--budget-mb", type=float, default=DEFAULT_BUDGET_MB, help=f"剧本包预算 MB(默认 {DEFAULT_BUDGET_MB})")
    args = parser.parse_args(argv)
    if args.check:
        return check_pack(Path(args.check))
    return build(args)


if __name__ == "__main__":
    raise SystemExit(main())
