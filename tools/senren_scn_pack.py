#!/usr/bin/env python3
"""Build the Senren * Banka script pack (SENRSCN1) for the AI Passport port.

Source project: https://github.com/hrk666666/Senren-Banka-MiBand-10
  - 小米手环 10(小米 Vela / aiot 快应用)上的《千恋＊万花》移植版;素材内容来自
    https://github.com/hezdaaa/qlwh-mibandported。
  - 剧本、立绘、背景、事件图版权归 SAGA PLANETS 所有;本仓库只保存转换产物,
    不分发源素材。先用 tools/senren_fetch_source.py 把源素材拉到 build/senren-source/:
      python tools/senren_fetch_source.py --dest build/senren-source --chunks

The pack is a single little-endian binary read straight out of flash.  The
firmware keeps only one chunk in RAM at a time, so every chunk is an
independent raw-deflate block addressable by (offset, length):

  header   : magic "SENRSCN1"(8B), version u16=1, header_size u16,
             section_count u16, reserved u16, total_size u32     (20 B)
  section  : { type u32, offset u32, count u32, size u32 }      (16 B × N)
    SEC_CHAR  char table : u32 count, count × u16 code point (ordered by global
              frequency, descending; the 2-byte code IS the index and doubles
              as the glyph index for the firmware font)
    SEC_NAME  dictionaries: four frozen blocks in this order — speaker, sprite
              key, event image, background — each u16 count then per entry
              u16 length + u16[length] char codes.  A fifth "ending" block is
              appended for the `*gameend_*` jump targets the frozen semantics
              did not enumerate; decoders read blocks until the section ends.
    SEC_FLAG  flags : u32 count, count × u32 page (f.c<page> → index is flag_id)
    SEC_CHUNK chunk table : u16 count, per chunk
              { data_off u32, data_len u32, raw_len u32, node_count u16,
                first_block u16, block_count u16, reserved u16 }   (20 B)
              (data_off is relative to SEC_BLOB)
    SEC_BLOCK block table : u16 count, per small block
              { data_off u32, data_len u32, raw_len u32, first_node u32 }  (16 B)
              Every chunk's node stream is split into blocks of at most
              BLOCK_MAX_BYTES bytes / BLOCK_MAX_NODES nodes, each its own zlib
              stream starting with its own u16 node count, so the firmware only
              ever needs a ~4 KB buffer (no 32 KB inflate dictionary).
    SEC_BLOB  one zlib stream per small block (zlib level 9), concatenated,
              every block 4-byte aligned
    SEC_META  key=value UTF-8 text, one per line (source repo, ref, counts)

  Per-chunk record stream (decompressed):
    u16 node_count, then records:
      u8 kind
        0 LABEL      u32 page
        1 CHAPTER    text
        2 BG         u8 bg index
        3 DIALOGUE   u8 speaker index, text, u8 action,
                     u16 sprite key index (0xFFFF = none)
        4 SELECT     u8 option count, per option { text, u8 target kind
                     (0=same chunk,1=cross), u16 chunk number (0 when same),
                     u32 page, u8 flag_id, u8 value }
        5 EV         u16 event index (0xFFFF = clear CG)
        6 NEXT       u8 target kind, u16 chunk number, u32 page,
                     u8 condition count, per condition { u8 flag_id, u8 value }
        7 SPRITE_OFF (no arguments)
      text = u16 character count + that many u16 codes (char table indices)
    action enum: 0=none, 1=fadein, 2=fadeout, 3=change (unknown actions fold
    into 3 and are listed in SEC_META)
    target kind: 0=same chunk, 1=cross chunk, 2=ending (page = ending index,
    chunk number 0)
  section.count: SEC_CHAR=chars, SEC_NAME=blocks, SEC_FLAG=flags,
                 SEC_CHUNK=chunks, SEC_BLOB=chunks, SEC_META=0

源数据语义(在全部 112 块、74,504 个节点上实测):
  [0, "*pN"]                     标签,唯一跳转目标
  [1, "CHAPTERx-y"]              章节标题
  [2, "<bg>"]                    切换背景,名字在 bg 名字表
  [3, spk, text] / [3, spk, text, [[sprite, "center", action]]]
                                 对白;无立绘字段表示保持当前立绘
  [4, [[text, target, expr], …]] 选项;target 为 *pN 或 <chunk>@*pN,expr 置旗标
  [5, name] / [5, null]          事件 CG 显示 / 清除
  [6, target] / [6, target, cond]条件跳转,cond 由 " && " 连接
  [7, "*", "center", "fadeout"]  移除当前立绘
文本必须双射:解出来的 UTF-8 与原文逐字节一致,不做归一化、不漏字。

Usage:
  python tools/senren_scn_pack.py --source build/senren-source \
      --out build/senren-pack/senren_scn.bin \
      --json build/senren-pack/senren_scn.json
  python tools/senren_scn_pack.py --check build/senren-pack/senren_scn.bin
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

MAGIC = b"SENRSCN1"
VERSION = 1
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
CHUNK_ENTRY = struct.Struct("<IIIHHHH")
# 块表:每个小块 = { 数据偏移 u32, 压缩长 u32, 解压长 u32, 块内首节点下标 u32 }
BLOCK_ENTRY = struct.Struct("<IIII")
# 小块切分上限:固件只有几 KB 空闲堆,不能整块解压。两个上限同时生效。
BLOCK_MAX_BYTES = 3000
BLOCK_MAX_NODES = 128
assert HEADER.size == 20 and SECTION.size == 16 and CHUNK_ENTRY.size == 20 and BLOCK_ENTRY.size == 16

# 段类型
SEC_CHAR, SEC_NAME, SEC_FLAG, SEC_CHUNK, SEC_BLOCK, SEC_BLOB, SEC_META = range(7)
SECTION_NAMES = {
    SEC_CHAR: "SEC_CHAR", SEC_NAME: "SEC_NAME", SEC_FLAG: "SEC_FLAG",
    SEC_CHUNK: "SEC_CHUNK", SEC_BLOCK: "SEC_BLOCK", SEC_BLOB: "SEC_BLOB", SEC_META: "SEC_META",
}

# 记录类型
K_LABEL, K_CHAPTER, K_BG, K_DIALOGUE, K_SELECT, K_EV, K_NEXT, K_SPRITE_OFF = range(8)
KIND_NAMES = {
    K_LABEL: "LABEL", K_CHAPTER: "CHAPTER", K_BG: "BG", K_DIALOGUE: "DIALOGUE",
    K_SELECT: "SELECT", K_EV: "EV", K_NEXT: "NEXT", K_SPRITE_OFF: "SPRITE_OFF",
}

# action 枚举:未知动作折进 change,同时在 SEC_META 里留痕
ACT_NONE, ACT_FADEIN, ACT_FADEOUT, ACT_CHANGE = range(4)
ACTION_IDS = {"": ACT_NONE, "fadein": ACT_FADEIN, "fadeout": ACT_FADEOUT, "change": ACT_CHANGE}
ACTION_NAMES = ("none", "fadein", "fadeout", "change")

# 跳转目标类型
T_SAME, T_CROSS, T_END = 0, 1, 2

NO_INDEX = 0xFFFF
SPRITE_OFF_ACTION = "fadeout"     # [7] 节点固定语义
SPRITE_POSITION = "center"        # 立绘位置在源数据里恒为 center

NAME_BLOCKS = ("speaker", "sprite", "event", "background", "ending")

CHUNK_FILE = re.compile(r"^chunk(\d+)\.txt$")
LABEL_RE = re.compile(r"^\*p(\d+)$")
TARGET_RE = re.compile(r"^(?:(\d+)@)?\*p(\d+)$")
OPTION_FLAG_RE = re.compile(r"^f\.c(\d+)=(\d+)$")
CONDITION_RE = re.compile(r"^f\.c(\d+) == (\d+)$")
ENDING_RE = re.compile(r"^\*(gameend_.+)$")


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def human(count: int) -> str:
    if abs(count) >= 1048576:
        return f"{count / 1048576:.2f} MB"
    if abs(count) >= 1024:
        return f"{count / 1024:.1f} KB"
    return f"{count} B"


def percentile(values: list[int], fraction: float) -> int:
    """最近秩分位数:确定性,不插值。"""
    if not values:
        return 0
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(fraction * (len(ordered) - 1))))
    return ordered[index]


# --------------------------------------------------------------------------
# 二进制读写
# --------------------------------------------------------------------------


class Writer:
    """小端写入器;text 用 u16 字数 + u16 码表下标。"""

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
    """小端读取器;越界立即报错,避免静默截断。"""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def _take(self, count: int) -> None:
        if self.pos + count > len(self.data):
            raise ValueError(f"记录流越界: 需要 {count} 字节, 只剩 {len(self.data) - self.pos}")

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
        return [self.u16() for _ in range(self.u16())]

    @property
    def done(self) -> bool:
        return self.pos == len(self.data)


def encode_text(text: str, char_index: dict[int, int]) -> list[int]:
    """UTF-8 字符串 → 码表下标序列;缺字直接报错,绝不静默丢弃。"""
    codes: list[int] = []
    for char in text:
        index = char_index.get(ord(char))
        if index is None:
            raise KeyError(f"码表缺字 {char!r} (U+{ord(char):04X})")
        codes.append(index)
    return codes


def decode_text(codes: list[int], char_table: list[int]) -> str:
    return "".join(chr(char_table[code]) for code in codes)


# --------------------------------------------------------------------------
# 源剧本
# --------------------------------------------------------------------------


def source_ref(source: Path) -> str:
    """从取源清单里带出上游 ref,让 pack 元数据可追溯(取源工具写的 MANIFEST.json)。"""
    manifest = source / "MANIFEST.json"
    if manifest.is_file():
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
            ref = str(data.get("ref", "unknown"))
            requested = str(data.get("requested_ref", ""))
            return ref if requested in ("", ref) else f"{ref} (请求 {requested})"
        except (json.JSONDecodeError, OSError):
            pass
    return "unknown"


def split_conditions(expr: str) -> list[str]:
    return [term for term in expr.split(" && ") if term]


@dataclass
class Story:
    """解析好的源剧本 + 建表结果(名字表 / 旗标表 / 码表)。"""

    chunks: list[list] = field(default_factory=list)
    pools: dict[str, list[str]] = field(default_factory=dict)
    pool_index: dict[str, dict[str, int]] = field(default_factory=dict)
    flags: list[int] = field(default_factory=list)
    flag_index: dict[int, int] = field(default_factory=dict)
    char_table: list[int] = field(default_factory=list)
    char_index: dict[int, int] = field(default_factory=dict)
    counts: collections.Counter = field(default_factory=collections.Counter)
    unknown_actions: list[str] = field(default_factory=list)
    dialogue_chars: int = 0

    def text_codes(self, text: str) -> list[int]:
        return encode_text(text, self.char_index)


def parse_target(target: str) -> tuple[int, int, int, str]:
    """跳转目标 → (类型, 块号, 页号, 结局名)。结局只在 NEXT 里出现。"""
    match = TARGET_RE.match(target)
    if match:
        if match.group(1):
            return T_CROSS, int(match.group(1)), int(match.group(2)), ""
        return T_SAME, 0, int(match.group(2)), ""
    ending = ENDING_RE.match(target)
    if ending:
        return T_END, 0, 0, ending.group(1)
    raise ValueError(f"无法解析跳转目标: {target!r}")


def scan_source(source: Path) -> Story:
    """读全部 chunk*.txt:分块、建名字表和旗标表、统计全局字频。"""
    scn_dir = source / "scn"
    files: dict[int, Path] = {}
    for path in sorted(scn_dir.glob("chunk*.txt")):
        match = CHUNK_FILE.match(path.name)
        if match:
            files[int(match.group(1))] = path
    if not files:
        sys.exit(f"ERROR: {scn_dir} 里没有 chunk*.txt\n先跑: python tools/senren_fetch_source.py --dest {source} --chunks")
    numbers = sorted(files)
    if numbers != list(range(1, len(numbers) + 1)):
        sys.exit(f"ERROR: chunk 编号不连续: {numbers[:5]} … {numbers[-5:]}")

    story = Story()
    pools: dict[str, set[str]] = {block: set() for block in NAME_BLOCKS}
    flag_pages: set[int] = set()
    frequency: collections.Counter = collections.Counter()

    def count(text: str) -> None:
        frequency.update(text)

    for number in numbers:
        nodes = json.loads(files[number].read_text(encoding="utf-8"))
        story.chunks.append(nodes)
        for node in nodes:
            kind = node[0]
            story.counts["nodes"] += 1
            story.counts[f"kind_{KIND_NAMES.get(kind, kind)}"] += 1
            if kind == K_LABEL:
                match = LABEL_RE.match(node[1])
                if not match:
                    sys.exit(f"ERROR: chunk{number:03d} 标签格式异常: {node[1]!r}")
                story.counts["labels"] += 1
            elif kind == K_CHAPTER:
                count(node[1])
            elif kind == K_BG:
                pools["background"].add(node[1])
                count(node[1])
            elif kind == K_DIALOGUE:
                pools["speaker"].add(node[1])
                count(node[1])
                count(node[2])
                story.dialogue_chars += len(node[2])
                if len(node) > 3 and node[3]:
                    for sprite in node[3]:
                        pools["sprite"].add(sprite[0])
                        count(sprite[0])
                        if sprite[2] not in ACTION_IDS and sprite[2] not in story.unknown_actions:
                            story.unknown_actions.append(sprite[2])
            elif kind == K_SELECT:
                for option in node[1]:
                    if len(option) != 3:
                        sys.exit(f"ERROR: chunk{number:03d} 选项字段数异常: {option!r}")
                    count(option[0])
                    story.counts["options"] += 1
                    _kind, _chunk, _page, ending = parse_target(option[1])
                    if ending:
                        sys.exit(f"ERROR: chunk{number:03d} 选项出现结局目标: {option[1]!r}")
                    match = OPTION_FLAG_RE.match(option[2])
                    if not match:
                        sys.exit(f"ERROR: chunk{number:03d} 选项旗标表达式异常: {option[2]!r}")
                    flag_pages.add(int(match.group(1)))
                    story.counts["max_flag_value"] = max(story.counts["max_flag_value"], int(match.group(2)))
            elif kind == K_EV:
                if node[1] is not None:
                    pools["event"].add(node[1])
                    count(node[1])
                else:
                    story.counts["ev_clear"] += 1
            elif kind == K_NEXT:
                tkind, _tchunk, _tpage, ending = parse_target(node[1])
                if tkind == T_END:
                    pools["ending"].add(ending)
                    count(ending)
                if len(node) > 2:
                    for term in split_conditions(node[2]):
                        match = CONDITION_RE.match(term)
                        if not match:
                            sys.exit(f"ERROR: chunk{number:03d} 跳转条件异常: {term!r}")
                        flag_pages.add(int(match.group(1)))
                        story.counts["max_flag_value"] = max(story.counts["max_flag_value"], int(match.group(2)))
                        story.counts["conditions"] += 1
            elif kind == K_SPRITE_OFF:
                pass
            else:
                sys.exit(f"ERROR: chunk{number:03d} 未知节点类型 {kind}: {node!r}")

    # 码表按全局字频降序,同频按码点升序,保证可复现
    order = sorted(frequency, key=lambda char: (-frequency[char], ord(char)))
    story.char_table = [ord(char) for char in order]
    if len(story.char_table) > 0x10000:
        sys.exit(f"ERROR: 码表 {len(story.char_table)} 项超过 u16 下标上限")
    story.char_index = {point: index for index, point in enumerate(story.char_table)}
    # 名字表字典序(确定性);说话人/背景下标是 u8,这里顺手卡上限
    story.pools = {block: sorted(pools[block]) for block in NAME_BLOCKS}
    story.pool_index = {block: {name: index for index, name in enumerate(story.pools[block])} for block in NAME_BLOCKS}
    story.flags = sorted(flag_pages)
    story.flag_index = {page: index for index, page in enumerate(story.flags)}
    # u8/u16 字段的容量护栏:宁可报错,也不静默截断
    if len(story.pools["speaker"]) > 0x100 or len(story.pools["background"]) > 0x100:
        sys.exit("ERROR: 说话人或背景名字超过 256 个,u8 下标放不下")
    if len(story.flags) > 0x100:
        sys.exit("ERROR: 旗标超过 256 个,u8 flag_id 放不下")
    if story.counts["max_flag_value"] > 0xFF:
        sys.exit("ERROR: 旗标取值超过 255,u8 放不下")
    if len(story.chunks) > 0xFFFF or max((len(nodes) for nodes in story.chunks), default=0) > 0xFFFF:
        sys.exit("ERROR: 块数或单块节点数超过 u16")
    return story


# --------------------------------------------------------------------------
# 记录编码 / 解码
# --------------------------------------------------------------------------


def encode_node(writer: Writer, node: list, story: Story) -> None:
    kind = node[0]
    writer.u8(kind)
    if kind == K_LABEL:
        writer.u32(int(LABEL_RE.match(node[1]).group(1)))
    elif kind == K_CHAPTER:
        writer.text(story.text_codes(node[1]))
    elif kind == K_BG:
        writer.u8(story.pool_index["background"][node[1]])
    elif kind == K_DIALOGUE:
        writer.u8(story.pool_index["speaker"][node[1]])
        writer.text(story.text_codes(node[2]))
        sprite = node[3][0] if len(node) > 3 and node[3] else None
        if sprite:
            name, _position, action = sprite
            writer.u8(ACTION_IDS.get(action, ACT_CHANGE))
            writer.u16(story.pool_index["sprite"][name])
        else:
            writer.u8(ACT_NONE)
            writer.u16(NO_INDEX)
    elif kind == K_SELECT:
        writer.u8(len(node[1]))
        for text, target, expr in node[1]:
            writer.text(story.text_codes(text))
            tkind, tchunk, tpage, _ending = parse_target(target)
            writer.u8(tkind)
            writer.u16(tchunk)
            writer.u32(tpage)
            match = OPTION_FLAG_RE.match(expr)
            writer.u8(story.flag_index[int(match.group(1))])
            writer.u8(int(match.group(2)))
    elif kind == K_EV:
        writer.u16(NO_INDEX if node[1] is None else story.pool_index["event"][node[1]])
    elif kind == K_NEXT:
        tkind, tchunk, tpage, ending = parse_target(node[1])
        if tkind == T_END:
            tpage = story.pool_index["ending"][ending]
        writer.u8(tkind)
        writer.u16(tchunk)
        writer.u32(tpage)
        terms = split_conditions(node[2]) if len(node) > 2 else []
        writer.u8(len(terms))
        for term in terms:
            match = CONDITION_RE.match(term)
            writer.u8(story.flag_index[int(match.group(1))])
            writer.u8(int(match.group(2)))
    elif kind == K_SPRITE_OFF:
        pass
    else:
        raise ValueError(f"未知节点类型 {kind}")


def decode_node(reader: Reader, ctx: "Context") -> list:
    """还原成源 JSON 节点形状,便于逐节点比对。"""
    kind = reader.u8()
    if kind == K_LABEL:
        return [K_LABEL, "*p%d" % reader.u32()]
    if kind == K_CHAPTER:
        return [K_CHAPTER, ctx.text(reader)]
    if kind == K_BG:
        return [K_BG, ctx.block("background")[reader.u8()]]
    if kind == K_DIALOGUE:
        speaker = ctx.block("speaker")[reader.u8()]
        text = ctx.text(reader)
        action = reader.u8()
        sprite = reader.u16()
        if sprite == NO_INDEX:
            return [K_DIALOGUE, speaker, text]
        return [K_DIALOGUE, speaker, text, [[ctx.block("sprite")[sprite], SPRITE_POSITION, ACTION_NAMES[action]]]]
    if kind == K_SELECT:
        options = []
        for _ in range(reader.u8()):
            text = ctx.text(reader)
            tkind = reader.u8()
            tchunk = reader.u16()
            tpage = reader.u32()
            flag = ctx.flags[reader.u8()]
            value = reader.u8()
            target = "*p%d" % tpage if tkind == T_SAME else "%03d@*p%d" % (tchunk, tpage)
            options.append([text, target, "f.c%d=%d" % (flag, value)])
        return [K_SELECT, options]
    if kind == K_EV:
        index = reader.u16()
        return [K_EV, None if index == NO_INDEX else ctx.block("event")[index]]
    if kind == K_NEXT:
        tkind = reader.u8()
        tchunk = reader.u16()
        tpage = reader.u32()
        if tkind == T_SAME:
            target = "*p%d" % tpage
        elif tkind == T_CROSS:
            target = "%03d@*p%d" % (tchunk, tpage)
        elif tkind == T_END:
            target = "*" + ctx.block("ending")[tpage]
        else:
            raise ValueError(f"未知跳转目标类型 {tkind}")
        terms = []
        for _ in range(reader.u8()):
            terms.append("f.c%d == %d" % (ctx.flags[reader.u8()], reader.u8()))
        return [K_NEXT, target] if not terms else [K_NEXT, target, " && ".join(terms)]
    if kind == K_SPRITE_OFF:
        return [K_SPRITE_OFF, "*", SPRITE_POSITION, SPRITE_OFF_ACTION]
    raise ValueError(f"未知记录类型 {kind}")


@dataclass
class Context:
    """解码所需的表(从 pack 自身段里解出)。"""

    char_table: list[int]
    pools: dict[str, list[str]]
    flags: list[int]

    def block(self, name: str) -> list[str]:
        try:
            return self.pools[name]
        except KeyError:
            raise ValueError(f"pack 缺少名字表 {name}")

    def text(self, reader: Reader) -> str:
        codes = reader.text()
        for code in codes:
            if code >= len(self.char_table):
                raise ValueError(f"码表下标越界: {code}")
        return decode_text(codes, self.char_table)


# --------------------------------------------------------------------------
# 打包
# --------------------------------------------------------------------------


def build_sections(story: Story, meta_blob: bytes) -> tuple[list[tuple[int, bytes, int, int]], list[tuple[int, int, int, int]]]:
    """按段序拼出各段载荷,并顺带算出每块的压缩统计。"""
    # SEC_CHAR
    char_writer = Writer()
    char_writer.u32(len(story.char_table))
    for point in story.char_table:
        char_writer.u16(point)

    # SEC_NAME:四个冻结块 + 追加的 ending 块
    name_writer = Writer()
    for block in NAME_BLOCKS:
        entries = story.pools[block]
        name_writer.u16(len(entries))
        for name in entries:
            name_writer.text(story.text_codes(name))

    # SEC_FLAG
    flag_writer = Writer()
    flag_writer.u32(len(story.flags))
    for page in story.flags:
        flag_writer.u32(page)

    # 逐块压缩记录流:每个小块一个独立 zlib 流,固件每次只解一块(<= BLOCK_MAX_BYTES)。
    # 之前是整块(平均 21 KB)一个流,但设备端空闲堆只剩十几 KB,拿不到 32 KB 解压缓冲。
    blob = bytearray()
    chunk_table: list[tuple] = []
    block_table: list[tuple] = []
    for nodes in story.chunks:
        encoded = []
        for node in nodes:
            node_writer = Writer()
            encode_node(node_writer, node, story)
            encoded.append(bytes(node_writer.buf))
        chunk_start = len(blob)
        first_block = len(block_table)
        block_count = 0
        index = 0
        while index < len(encoded):
            bytes_in_block = 0
            count_in_block = 0
            while index + count_in_block < len(encoded) and count_in_block < BLOCK_MAX_NODES:
                following = len(encoded[index + count_in_block])
                if count_in_block > 0 and bytes_in_block + following > BLOCK_MAX_BYTES:
                    break
                bytes_in_block += following
                count_in_block += 1
            record = Writer()
            record.u16(count_in_block)
            for node_bytes in encoded[index:index + count_in_block]:
                record.buf += node_bytes
            payload = zlib.compress(bytes(record.buf), 9)
            blob += b"\x00" * (-len(blob) % 4)   # 每块 4 字节对齐(段内相对地址)
            block_table.append((len(blob), len(payload), len(record.buf), index))
            blob += payload
            block_count += 1
            index += count_in_block
        chunk_table.append((chunk_start, len(blob) - chunk_start, 0, len(nodes), first_block,
                            block_count, 0))

    chunk_writer = Writer()
    chunk_writer.u16(len(chunk_table))
    for entry in chunk_table:
        chunk_writer.buf += CHUNK_ENTRY.pack(*entry)

    block_writer = Writer()
    block_writer.u16(len(block_table))
    for entry in block_table:
        block_writer.buf += BLOCK_ENTRY.pack(*entry)

    sections = [
        (SEC_CHAR, bytes(char_writer.buf), len(story.char_table), len(char_writer.buf)),
        (SEC_NAME, bytes(name_writer.buf), len(NAME_BLOCKS), len(name_writer.buf)),
        (SEC_FLAG, bytes(flag_writer.buf), len(story.flags), len(flag_writer.buf)),
        (SEC_CHUNK, bytes(chunk_writer.buf), len(chunk_table), len(chunk_writer.buf)),
        (SEC_BLOCK, bytes(block_writer.buf), len(block_table), len(block_writer.buf)),
        (SEC_BLOB, bytes(blob), len(block_table), len(blob)),
        (SEC_META, meta_blob, 0, len(meta_blob)),
    ]
    return sections, chunk_table


def serialize(story: Story, meta: dict[str, str]) -> bytes:
    meta_blob = ("\n".join(f"{key}={value}" for key, value in sorted(meta.items())) + "\n").encode("utf-8")
    sections, _ = build_sections(story, meta_blob)
    header_size = HEADER.size + SECTION.size * len(sections)
    offset = header_size
    table = bytearray()
    for section_type, body, count, size in sections:
        table += SECTION.pack(section_type, offset, count, size)
        offset += len(body) + (-len(body) % 4)   # 段按 4 字节对齐
    total = offset
    head = HEADER.pack(MAGIC, VERSION, header_size, len(sections), 0, total)
    out = [head, bytes(table)]
    for _, body, _, _ in sections:
        out.append(body)
        out.append(b"\x00" * (-len(body) % 4))
    return b"".join(out)


# --------------------------------------------------------------------------
# 解包 / 自检
# --------------------------------------------------------------------------


@dataclass
class ChunkInfo:
    data_off: int
    data_len: int
    raw_len: int
    node_count: int
    first_block: int
    block_count: int
    reserved: int


@dataclass
class BlockInfo:
    data_off: int
    data_len: int
    raw_len: int
    first_node: int


class Pack:
    """只读 pack;`records(i)` 返回第 i 块还原后的源节点列表。"""

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
        expected = HEADER.size + SECTION.size * section_count
        if header_size < expected:
            raise ValueError(f"header_size={header_size} 放不下 {section_count} 个段表项")
        self.raw = raw
        self.version = version
        self.header_size = header_size
        self.sections: dict[int, tuple[int, int, int]] = {}
        self.section_order: list[int] = []
        for index in range(section_count):
            section_type, offset, count, size = SECTION.unpack_from(raw, HEADER.size + SECTION.size * index)
            if offset + size > len(raw):
                raise ValueError(f"段 {section_type} 越界: {offset}+{size} > {len(raw)}")
            self.sections[section_type] = (offset, count, size)
            self.section_order.append(section_type)

        for required in (SEC_CHAR, SEC_NAME, SEC_FLAG, SEC_CHUNK, SEC_BLOCK, SEC_BLOB, SEC_META):
            if required not in self.sections:
                raise ValueError(f"缺段 {SECTION_NAMES[required]}")

        # SEC_CHAR
        char_off, char_count, char_size = self.sections[SEC_CHAR]
        if char_size != 4 + 2 * char_count:
            raise ValueError(f"SEC_CHAR 大小不符: {char_size} vs 4+2*{char_count}")
        stored_count = struct.unpack_from("<I", raw, char_off)[0]
        if stored_count != char_count:
            raise ValueError(f"SEC_CHAR count 不符: {stored_count} vs {char_count}")
        self.char_table = list(struct.unpack_from("<%dH" % char_count, raw, char_off + 4))
        if len(set(self.char_table)) != len(self.char_table):
            raise ValueError("码表有重复码点")

        # SEC_NAME:读到段尾为止,天然兼容四个冻结块 + 追加的 ending 块
        name_off, _name_count, name_size = self.sections[SEC_NAME]
        self.pools: dict[str, list[str]] = {}
        name_reader = Reader(raw[name_off:name_off + name_size])
        for block in NAME_BLOCKS:
            if name_reader.pos >= name_size:
                if block == "ending":
                    self.pools[block] = []      # 冻结的 4 块格式也认
                    continue
                raise ValueError(f"SEC_NAME 缺少名字表 {block}")
            entries = []
            for _ in range(name_reader.u16()):
                codes = name_reader.text()
                for code in codes:
                    if code >= char_count:
                        raise ValueError(f"名字表 {block} 码表下标越界: {code}")
                entries.append(decode_text(codes, self.char_table))
            self.pools[block] = entries
        if not name_reader.done:
            raise ValueError(f"SEC_NAME 有多余字节: 停在 {name_reader.pos}/{name_size}")

        # SEC_FLAG
        flag_off, flag_count, flag_size = self.sections[SEC_FLAG]
        if flag_size != 4 + 4 * flag_count:
            raise ValueError(f"SEC_FLAG 大小不符: {flag_size} vs 4+4*{flag_count}")
        stored_count = struct.unpack_from("<I", raw, flag_off)[0]
        if stored_count != flag_count:
            raise ValueError(f"SEC_FLAG count 不符: {stored_count} vs {flag_count}")
        self.flags = list(struct.unpack_from("<%dI" % flag_count, raw, flag_off + 4))

        # SEC_CHUNK
        chunk_off, chunk_count, chunk_size = self.sections[SEC_CHUNK]
        if chunk_size != 2 + CHUNK_ENTRY.size * chunk_count:
            raise ValueError(f"SEC_CHUNK 大小不符: {chunk_size} vs 2+{CHUNK_ENTRY.size}*{chunk_count}")
        stored_count = struct.unpack_from("<H", raw, chunk_off)[0]
        if stored_count != chunk_count:
            raise ValueError(f"SEC_CHUNK count 不符: {stored_count} vs {chunk_count}")
        self.chunks = [
            ChunkInfo(*CHUNK_ENTRY.unpack_from(raw, chunk_off + 2 + CHUNK_ENTRY.size * index))
            for index in range(chunk_count)
        ]

        # SEC_BLOCK:小块表(每个 chunk 的节点被切成若干 <=3 KB 的小块)
        block_off, block_count, block_size = self.sections[SEC_BLOCK]
        if block_size != 2 + BLOCK_ENTRY.size * block_count:
            raise ValueError(f"SEC_BLOCK 大小不符: {block_size} vs 2+{BLOCK_ENTRY.size}*{block_count}")
        stored_blocks = struct.unpack_from("<H", raw, block_off)[0]
        if stored_blocks != block_count:
            raise ValueError(f"SEC_BLOCK count 不符: {stored_blocks} vs {block_count}")
        self.blocks = [
            BlockInfo(*BLOCK_ENTRY.unpack_from(raw, block_off + 2 + BLOCK_ENTRY.size * index))
            for index in range(block_count)
        ]

        self.blob_off, _blob_count, self.blob_size = self.sections[SEC_BLOB]
        self.meta_off, _meta_count, self.meta_size = self.sections[SEC_META]
        self._ctx = Context(self.char_table, self.pools, self.flags)

    @property
    def meta(self) -> dict[str, str]:
        text = self.raw[self.meta_off:self.meta_off + self.meta_size].decode("utf-8")
        result: dict[str, str] = {}
        for line in text.splitlines():
            if line and "=" in line:
                key, value = line.split("=", 1)
                result[key] = value
        return result

    def decompress_block(self, index: int) -> bytes:
        info = self.blocks[index]
        payload = self.raw[self.blob_off + info.data_off:self.blob_off + info.data_off + info.data_len]
        raw = zlib.decompress(payload)   # 每个小块一个 zlib 流(带 adler32)
        if len(raw) != info.raw_len:
            raise ValueError(f"第 {index + 1} 个小块解压长度不符: {len(raw)} != {info.raw_len}")
        return raw

    def records(self, index: int) -> list:
        """把一个 chunk 的各个小块拼回节点列表(同时校验块表与每块的节点数)。"""
        chunk = self.chunks[index]
        nodes: list = []
        for step in range(chunk.block_count):
            block = self.blocks[chunk.first_block + step]
            if block.first_node != len(nodes):
                raise ValueError(
                    f"第 {index + 1} 块第 {step + 1} 个小块的 first_node={block.first_node} != {len(nodes)}")
            reader = Reader(self.decompress_block(chunk.first_block + step))
            count = reader.u16()
            nodes.extend(decode_node(reader, self._ctx) for _ in range(count))
            if not reader.done:
                raise ValueError(f"第 {index + 1} 块第 {step + 1} 个小块记录流多余 {len(reader.raw) - reader.pos} 字节")
        if len(nodes) != chunk.node_count:
            raise ValueError(f"第 {index + 1} 块节点数不符: {len(nodes)} != {chunk.node_count}")
        return nodes

    def all_records(self) -> list[list]:
        return [self.records(index) for index in range(len(self.chunks))]

    def validate_structure(self) -> list[str]:
        """结构自检:段/表/记录流的一致性,不依赖源数据。"""
        errors: list[str] = []
        expected = HEADER.size + SECTION.size * len(self.section_order)
        if self.header_size != expected:
            errors.append(f"header_size {self.header_size} != {expected}")
        for section_type, (offset, _count, size) in self.sections.items():
            if offset % 4:
                errors.append(f"{SECTION_NAMES.get(section_type, section_type)} 未 4 字节对齐")
        for index, info in enumerate(self.blocks):
            if info.data_off % 4:
                errors.append(f"第 {index + 1} 个小块未 4 字节对齐: {info.data_off}")
            if info.data_off + info.data_len > self.blob_size:
                errors.append(f"第 {index + 1} 个小块越界: {info.data_off}+{info.data_len}")
            try:
                raw = zlib.decompress(
                    self.raw[self.blob_off + info.data_off:self.blob_off + info.data_off + info.data_len])
            except zlib.error as exc:
                errors.append(f"第 {index + 1} 个小块解压失败: {exc}")
                continue
            if len(raw) != info.raw_len:
                errors.append(f"第 {index + 1} 个小块 raw_len 不符: {len(raw)} vs {info.raw_len}")
            try:
                reader = Reader(raw)
                count = reader.u16()
                for _ in range(count):
                    decode_node(reader, self._ctx)
                if not reader.done:
                    errors.append(f"第 {index + 1} 个小块记录流有多余字节")
            except (ValueError, IndexError) as exc:
                errors.append(f"第 {index + 1} 个小块记录流异常: {exc}")
        for index, info in enumerate(self.chunks):
            if info.first_block + info.block_count > len(self.blocks):
                errors.append(f"第 {index + 1} 块的块表越界")
                continue
            counted = 0
            for step in range(info.block_count):
                block = self.blocks[info.first_block + step]
                if block.first_node != counted:
                    errors.append(
                        f"第 {index + 1} 块第 {step + 1} 个小块 first_node 不符: "
                        f"{block.first_node} != {counted}")
                counted = block.first_node + self._block_node_count(info.first_block + step)
            if counted != info.node_count:
                errors.append(f"第 {index + 1} 块 node_count 不符: {counted} vs {info.node_count}")
        return errors

    def _block_node_count(self, index: int) -> int:
        info = self.blocks[index]
        raw = zlib.decompress(
            self.raw[self.blob_off + info.data_off:self.blob_off + info.data_off + info.data_len])
        return struct.unpack_from("<H", raw)[0]


def read_pack(path: Path) -> Pack:
    return Pack(path.read_bytes())


def check_pack(path: Path) -> int:
    try:
        pack = read_pack(path)
        errors = pack.validate_structure()
    except (ValueError, OSError, zlib.error) as exc:
        log(f"自检: FAIL  {path.name}: {exc}")
        return 1
    nodes = sum(info.node_count for info in pack.chunks)
    log(f"自检: {'PASS' if not errors else 'FAIL'}  {path.name} {len(pack.raw)} 字节,"
        f"{len(pack.chunks)} 块 {nodes} 节点,码表 {len(pack.char_table)},"
        f"旗标 {len(pack.flags)},载荷 {human(pack.blob_size)}")
    names = " / ".join(f"{block}×{len(pack.pools[block])}" for block in NAME_BLOCKS)
    log(f"  名字表: {names}")
    for line in errors[:10]:
        log("  " + line)
    return 1 if errors else 0


# --------------------------------------------------------------------------
# 报告
# --------------------------------------------------------------------------


def report(pack: Pack, out_path: Path, story: Story, meta: dict[str, str], args: argparse.Namespace) -> None:
    sections = [(t, c, s) for t in pack.section_order for c, s in [pack.sections[t][1:]]]
    chunk_table = [(c.data_off, c.data_len, c.raw_len, c.node_count) for c in pack.chunks]
    log("")
    log(f"{'段':<12}{'条目':>8}{'大小':>12}")
    for section_type, count, size in sections:
        log(f"{SECTION_NAMES[section_type]:<12}{count:>8}{human(size):>12}")
    log(f"{'包总大小':<12}{'':>8}{human(len(pack.raw)):>12}")
    log("")
    nodes = story.counts["nodes"]
    log(f"剧本: {len(story.chunks)} 块 / {nodes} 节点 / {story.dialogue_chars} 对白字")
    kinds = " ".join(
        f"{KIND_NAMES[kind]}×{story.counts['kind_' + KIND_NAMES[kind]]}" for kind in range(8)
    )
    log(f"  节点类型: {kinds}")
    log(f"  名字表: 说话人 {len(story.pools['speaker'])} / 立绘键 {len(story.pools['sprite'])} / "
        f"事件图 {len(story.pools['event'])} / 背景 {len(story.pools['background'])} / "
        f"结局 {len(story.pools['ending'])}")
    log(f"  旗标: {len(story.flags)}  标签: {story.counts['labels']}  条件: {story.counts['conditions']}")
    compressed = [block.data_len for block in pack.blocks]
    raw_sizes = [block.raw_len for block in pack.blocks]
    nodes_per_block = sorted(
        (pack.chunks[index].node_count, ) for index in range(len(pack.chunks)))
    log(f"  小块数: {len(pack.blocks)}(平均每 chunk "
        f"{len(pack.blocks) / max(1, len(pack.chunks)):.1f} 块)")
    log(f"  压缩分布(每小块): 最小 {percentile(compressed, 0.0)} / 中位 {percentile(compressed, 0.5)} / "
        f"均值 {sum(compressed) / len(compressed):.0f} / p90 {percentile(compressed, 0.9)} / "
        f"最大 {percentile(compressed, 1.0)} 字节")
    log(f"  原始分布(每小块): 最小 {percentile(raw_sizes, 0.0)} / 中位 {percentile(raw_sizes, 0.5)} / "
        f"最大 {percentile(raw_sizes, 1.0)} 字节(固件解压缓冲需 >= 最大值),"
        f"整体压缩率 {100 * sum(compressed) / sum(raw_sizes):.1f}%")
    if story.unknown_actions:
        log(f"  未知立绘动作(折进 change): {story.unknown_actions}")
    log(f"写出 {out_path}")
    if args.json:
        payload = {
            "meta": meta,
            "sections": [
                {"name": SECTION_NAMES[t], "type": t, "count": c, "bytes": s}
                for t, c, s in sections
            ],
            "counts": {
                "chunks": len(story.chunks),
                "nodes": nodes,
                "nodes_by_kind": {KIND_NAMES[k]: story.counts["kind_" + KIND_NAMES[k]] for k in range(8)},
                "dialogue_characters": story.dialogue_chars,
                "characters": len(story.char_table),
                "flags": len(story.flags),
                "labels": story.counts["labels"],
                "conditions": story.counts["conditions"],
                "options": story.counts["options"],
                "name_blocks": {block: len(story.pools[block]) for block in NAME_BLOCKS},
            },
            "flags": story.flags,
            "chunks": [
                {
                    "index": index + 1,
                    "data_off": entry[0], "data_len": entry[1],
                    "raw_len": entry[2], "node_count": entry[3],
                    "ratio": round(entry[1] / entry[2], 4) if entry[2] else 0.0,
                }
                for index, entry in enumerate(chunk_table)
            ],
            "compressed": {
                "min": percentile(compressed, 0.0),
                "median": percentile(compressed, 0.5),
                "p90": percentile(compressed, 0.9),
                "max": percentile(compressed, 1.0),
                "mean": round(sum(compressed) / len(compressed), 1),
            },
            "unknown_actions": story.unknown_actions,
        }
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(payload, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        log(f"写出清单 {args.json}")


def build(args: argparse.Namespace) -> int:
    source = Path(args.source)
    if not (source / "scn").is_dir():
        sys.exit(f"ERROR: 源剧本目录不存在: {source / 'scn'}\n"
                 f"先跑: python tools/senren_fetch_source.py --dest {source} --chunks")
    story = scan_source(source)
    meta = {
        "source_repo": "https://github.com/hrk666666/Senren-Banka-MiBand-10",
        "content_repo": "https://github.com/hezdaaa/qlwh-mibandported",
        "ref": args.ref or source_ref(source),
        "format": "SENRSCN1",
        "version": str(VERSION),
        "chunks": str(len(story.chunks)),
        "nodes": str(story.counts["nodes"]),
        "dialogue_characters": str(story.dialogue_chars),
        "characters": str(len(story.char_table)),
        "flags": str(len(story.flags)),
        "labels": str(story.counts["labels"]),
        "name_blocks": ",".join(NAME_BLOCKS),
        "action_enum": "0=none,1=fadein,2=fadeout,3=change",
        "target_enum": "0=same-chunk,1=cross-chunk,2=ending",
        "unknown_actions": ",".join(story.unknown_actions) if story.unknown_actions else "none",
        "text_encoding": "u16-count + u16 char-table indices, bijective",
        "deflate": "zlib, one block per chunk, level 9",
    }
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(serialize(story, meta))
    # 重新读回来再报告,保证报告里的段表就是磁盘上的段表
    report(read_pack(out_path), out_path, story, meta, args)
    return check_pack(out_path)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", default="build/senren-source", help="源素材目录(默认 build/senren-source)")
    parser.add_argument("--out", default="build/senren-pack/senren_scn.bin", help="输出 pack 路径")
    parser.add_argument("--json", help="额外写出条目/统计清单 JSON")
    parser.add_argument("--check", metavar="PACK", help="只校验一个已生成的 pack(不需要源剧本)")
    parser.add_argument("--ref", default="", help="写进元数据的源仓库 ref(默认读源目录的 MANIFEST.json)")
    args = parser.parse_args(argv)
    if args.check:
        return check_pack(Path(args.check))
    return build(args)


if __name__ == "__main__":
    raise SystemExit(main())
