#!/usr/bin/env python3
"""Build the Limelight Lemonade Jam script pack for the AI Passport port.

Source: https://github.com/skdkzzx/limelight-lemonade-jam-xiaomi-band10 的
`src/common/script/scriptDataN.txt`(137 个 JSON 分片,共 68,229 条对白)。
素材与译文版权归原作品与移植者所有;本仓库只保存转换工具与转换产物。

为什么要转格式:源数据是缩进过的 JSON,10.4 MB 里 43% 是键名/引号/缩进,而设备
只有 8 MB Flash。打包后条目按 250 条一块、每块独立 raw-deflate,设备端用 ESP32-C3
ROM 里的 tinfl_decompress 按需解压(零固件开销,单块解压缓冲 <20 KB)。

Container layout (little-endian; 与 main/limelight_script.h 一一对应):

  header 16 B : magic "LLSPK001", version u32, total u32, section_count u32
  section     : { type u32, off u32, count u32, size u32 }(off 是文件绝对偏移)
    SEC_TEXT      分块数据:每块 raw deflate(或 stored,见块 flags)
    SEC_CHUNK     { off u32, size u32, first_id u32, count u16, flags u16 }
                  off/size 相对 SEC_TEXT 段;first_id 是该块第一条对白的 id
    SEC_NAME      { off u32, len u16, pad u16 }  名字/选项/章节标签,指向 SEC_NAMETEXT
    SEC_NAMETEXT  UTF-8 文本池
    SEC_CHAPTER   { first_id u32, name u16 }     章节起点 id + 标签(name 表下标)
    SEC_CHOICE    { id u32, count u8, pad u8, [name u16, target u32] × 4 }
    SEC_META      key=value 文本(来源 / 条目数 / 字符表规模)

块内每条对白的记录(解压后):

  u8 flags : bit0 有背景 bit1 有立绘 bit2 有 CG bit3 有说话人 bit4 有正文
             bit5 z=1  bit6 z=2  bit7 是选项
  [u16 b] [u16 c] [u16 cg] [u16 s] 仅当对应位为 1(全局名字下标)
  [u16 len][len 字节 UTF-8]        仅当有正文

id 连续(1..entries),所以块内第 i 条 = first_id + i,推进就是 id±1。

Usage:
  python tools/limelight_script_pack.py --source <手环移植版仓库根> \\
      --out main/limelight_data/limelight_script.bin [--chunk 250] [--stored]
"""

from __future__ import annotations

import argparse
import collections
import io
import json
import re
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"LLSPK001"
VERSION = 1
GEN_VERSION = "limelight_script_pack/1"

SEC_TEXT, SEC_CHUNK, SEC_NAME, SEC_NAMETEXT, SEC_CHAPTER, SEC_CHOICE, SEC_META = range(7)
SECTION_COUNT = 7

HEADER = struct.Struct("<8sIIII")          # magic, version, total, sections, max_chunk_raw
HEADER_SIZE = 32
CHUNK_ENTRY = struct.Struct("<II I H H")   # off, size, first_id, count, flags
NAME_ENTRY = struct.Struct("<I H H")       # off, len, pad
CHAPTER_ENTRY = struct.Struct("<I H")      # first_id, name
CHOICE_ENTRY = struct.Struct("<I B B")     # id, count, pad
CHOICE_OPTION = struct.Struct("<HI")       # name u16, target u32(与 C 侧 6 字节一致)

FLAG_HAS_BG, FLAG_HAS_SPRITE, FLAG_HAS_CG, FLAG_HAS_SPEAKER, FLAG_HAS_TEXT = 1, 2, 4, 8, 16
FLAG_Z1, FLAG_Z2, FLAG_IS_CHOICE = 32, 64, 128

CHUNK_FLAG_STORED = 1 << 0      # 该块未压缩(raw deflate 的 stored 等价物)


class NamePool:
    """UTF-8 文本池 + 字节级去重,返回 (off, len)。"""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.index: dict[bytes, int] = {}
        self.entries: list[tuple[int, int]] = []

    def add(self, text: str) -> int:
        """返回该文本的名字下标;同文本只写一份表项。"""
        raw = text.encode("utf-8")
        if not raw:
            return 0xFFFF
        idx = self.index.get(raw)
        if idx is not None:
            return idx
        idx = len(self.entries)
        self.entries.append((len(self.buf), len(raw)))
        self.buf += raw
        self.index[raw] = idx
        return idx

    def size(self) -> int:
        return len(self.entries)

    def text(self, idx: int) -> str:
        off, ln = self.entries[idx]
        return self.buf[off:off + ln].decode("utf-8")


# 源数据里混进来的东西(手环移植版遗留),只清这些,剧情正文一字不改:
#   1) 排版指令:%f … %r 字体切换与重置、%i、$名字$ 图片、#rrggbb[aa] 颜色
#      (例如「来，射出来吧%f$ハート$#00ffadd6♥%r快点」——去掉指令后留下 ♥ 本身)
#   2) 译者备注:7 条(35629..35635)把 "memo: ..." 的英文说明混进了正文,
#      备注之前的部分才是正文(备注最长的一条 740 字节 = 6 屏,真机上表现为
#      "对话框被文字填满")
# 指令的两种写法都要吃:
#   「…%f$ハート$#00ffadd6♥%r…」  成对写法
#   「…%f$ハート♥…」             没有收尾 $ 的写法(6542)
# 优先级:先成对、再单个 $名字、再颜色、最后 %x 指令;留下的 ♥ 是可见字符,保留。
DIRECTIVE_RE = re.compile(
    r"\$[^$]{0,16}\$"                 # $名字$
    r"|\$(?=[^\W\d_])[^\W\d_]{2,16}"   # $名字(无收尾;至少 2 个字/假名/字母,不碰 $5)
    r"|#[0-9a-fA-F]{6,8}"                     # #rrggbb[aa]
    r"|%[a-zA-Z]+"                            # %f %r %i
)
# 自检:清完之后不该再出现"$+名字 / #rrggbb / %字母"这类指令形状。
# 单独的 $ 或 %(比如 "价格 $5"、"打折 50%")是正常文字,不算残留。
LEFTOVER_RE = re.compile(r"\$(?=[^\W\d_])|#[0-9a-fA-F]{6,8}|%[a-zA-Z]")

MEMO_RE = re.compile(r"memo[「『“ ‘ ' \s]{0,2}[:：]")


def clean_text(text: str) -> tuple[str, str]:
    """返回 (清洗后的正文, 命中的问题类型: '' / 'memo' / 'directive' / 'memo+directive')。"""
    hit = []
    parts = MEMO_RE.split(text, maxsplit=1)
    if len(parts) > 1:
        text = parts[0]
        hit.append("memo")
    if DIRECTIVE_RE.search(text):
        text = DIRECTIVE_RE.sub("", text)
        hit.append("directive")
    if LEFTOVER_RE.search(text):
        hit.append("leftover")           # 出现就说明还有没覆盖的指令写法
    return text.rstrip(), "+".join(hit)


def deflate_raw(data: bytes) -> bytes:
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(data) + compressor.flush()


def load_scripts(source: Path) -> dict[int, dict]:
    """读取全部 scriptDataN.txt,按 id 合并。"""
    script_dir = source / "src" / "common" / "script"
    paths = sorted(script_dir.glob("scriptData*.txt"),
                   key=lambda p: int(re.sub(r"\D", "", p.stem) or 0))
    if not paths:
        raise SystemExit(f"找不到剧本: {script_dir}")
    records: dict[int, dict] = {}
    for path in paths:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
        for key, value in data.items():
            records[int(key)] = value
    return records


def build(source: Path, chunk_size: int, stored: bool, keep_raw: bool = False) -> tuple[bytes, dict]:
    records = load_scripts(source)
    ids = sorted(records)
    if ids != list(range(1, len(ids) + 1)):
        raise SystemExit("id 不连续,需要调整块索引设计")

    names = NamePool()
    stats = collections.Counter()
    fields = ("b", "c", "cg", "s")

    def intern(text: str) -> int:
        return names.add(str(text)) if text else 0xFFFF

    choices: dict[int, tuple[list[str], list[int]]] = {}
    chunks: list[bytes] = []
    chunk_index: list[tuple[int, int, int, int, int]] = []

    for start in range(0, len(ids), chunk_size):
        block = ids[start:start + chunk_size]
        body = bytearray()
        for record_id in block:
            record = records[record_id]
            flags = 0
            b_idx = c_idx = cg_idx = s_idx = 0xFFFF
            text = str(record.get("t") or "")
            if record.get("b"):
                b_idx = intern(record["b"])
                flags |= FLAG_HAS_BG
            if record.get("c"):
                c_idx = intern(record["c"])
                flags |= FLAG_HAS_SPRITE
            if record.get("cg"):
                cg_idx = intern(record["cg"])
                flags |= FLAG_HAS_CG
            if record.get("s"):
                s_idx = intern(record["s"])
                flags |= FLAG_HAS_SPEAKER
            if not keep_raw:
                text, cleaned = clean_text(text)
            else:
                cleaned = ""
            if cleaned:
                stats["cleaned_" + cleaned] += 1
            if text:
                flags |= FLAG_HAS_TEXT
            z = record.get("z")
            if z == 1:
                flags |= FLAG_Z1
            elif z == 2:
                flags |= FLAG_Z2
            if record.get("co"):
                flags |= FLAG_IS_CHOICE
                picks = [str(record.get("c%d" % i) or "") for i in range(1, 5)]
                targets = [int(record.get("c%dt" % i) or record_id) for i in range(1, 5)]
                picks = [p for p in picks[:4]]
                targets = targets[:4]
                while picks and not picks[-1]:
                    picks.pop()
                    targets.pop()
                choices[record_id] = (picks, targets)
                stats["choice_steps"] += 1
            if record.get("z"):
                stats[f"z{int(record['z'])}"] += 1

            body += bytes((flags,))
            if flags & FLAG_HAS_BG:
                body += struct.pack("<H", b_idx)
            if flags & FLAG_HAS_SPRITE:
                body += struct.pack("<H", c_idx)
            if flags & FLAG_HAS_CG:
                body += struct.pack("<H", cg_idx)
            if flags & FLAG_HAS_SPEAKER:
                body += struct.pack("<H", s_idx)
            if flags & FLAG_HAS_TEXT:
                raw = text.encode("utf-8")
                body += struct.pack("<H", len(raw))
                body += raw

        payload = bytes(body)
        if stored:
            packed = payload
            flag = CHUNK_FLAG_STORED
        else:
            packed = deflate_raw(payload)
            flag = 0
        stats["raw_bytes"] += len(payload)
        stats["packed_bytes"] += len(packed)
        stats["max_chunk_raw"] = max(stats["max_chunk_raw"], len(payload))
        chunks.append(packed)
        chunk_index.append((0, len(packed), block[0], len(block), flag))

    # 章节:说话人以 "[CHAPTER" 开头的条目就是章节起点(源数据自带标签)
    chapters = []
    for record_id in ids:
        speaker = str(records[record_id].get("s") or "")
        if speaker.startswith("[CHAPTER"):
            chapters.append((record_id, intern(speaker)))
    stats["chapters"] = len(chapters)
    stats["entries"] = len(ids)
    stats["names"] = names.size()

    # ---- 组装段 ----
    text_blob = bytearray()
    fixed_index = []
    for off, size, first_id, count, flag in chunk_index:
        fixed_index.append((len(text_blob), size, first_id, count, flag))
        text_blob += chunks[len(fixed_index) - 1]

    # 选项文案也要入名字池,所以必须在建名字表之前完成(否则表长与条数对不上)。
    chapter_table = b"".join(CHAPTER_ENTRY.pack(first, name) for first, name in chapters)
    choice_table = bytearray()
    for record_id, (picks, targets) in sorted(choices.items()):
        choice_table += CHOICE_ENTRY.pack(record_id, len(picks), 0)
        for name_idx, target in zip(picks, targets):
            choice_table += CHOICE_OPTION.pack(intern(name_idx), target)
    name_table = b"".join(NAME_ENTRY.pack(off, ln, 0) for off, ln in names.entries)

    meta_text = "\n".join([
        f"generator={GEN_VERSION}",
        "source=limelight-lemonade-jam-xiaomi-band10",
        f"entries={stats['entries']}",
        f"chunk_entries={chunk_size}",
        f"chunks={len(chunks)}",
        f"names={names.size()}",
        f"chapters={stats['chapters']}",
        f"choice_steps={stats['choice_steps']}",
        f"raw_bytes={stats['raw_bytes']}",
        f"packed_bytes={stats['packed_bytes']}",
        f"max_chunk_raw={stats['max_chunk_raw']}",
        f"compression={'stored' if stored else 'raw-deflate'}",
        f"name_fields={','.join(fields)}",
    ]) + "\n"

    # 解压后的上限(固件按它申请块缓存):不是压缩后的大小。
    max_chunk_raw = stats["max_chunk_raw"]

    sections = [
        (SEC_TEXT, bytes(text_blob), len(chunks)),
        (SEC_CHUNK, b"".join(CHUNK_ENTRY.pack(*item) for item in fixed_index), len(fixed_index)),
        (SEC_NAME, name_table, names.size()),
        (SEC_NAMETEXT, bytes(names.buf), len(names.buf)),
        (SEC_CHAPTER, chapter_table, len(chapters)),
        (SEC_CHOICE, bytes(choice_table), len(choices)),
        (SEC_META, meta_text.encode("utf-8"), 0),
    ]
    header_size = HEADER_SIZE + 16 * SECTION_COUNT
    blob = bytearray(b"\x00" * header_size)
    table = bytearray()
    for sec_type, payload, count in sections:
        off = len(blob)
        blob += payload
        while len(blob) % 4:
            blob += b"\x00"
        table += struct.pack("<IIII", sec_type, off, count, len(payload))
    header = bytearray(HEADER_SIZE)
    HEADER.pack_into(header, 0, MAGIC, VERSION, len(blob), SECTION_COUNT, max_chunk_raw)
    blob[0:HEADER_SIZE] = header
    blob[HEADER_SIZE:HEADER_SIZE + len(table)] = table

    report = {
        "bytes": len(blob),
        "entries": stats["entries"],
        "chunks": len(chunks),
        "names": names.size(),
        "chapters": stats["chapters"],
        "choices": len(choices),
        "raw_bytes": stats["raw_bytes"],
        "packed_bytes": stats["packed_bytes"],
        "name_bytes": len(names.buf),
        "meta": meta_text,
        "cleaned": {k[len("cleaned_"):]: v for k, v in stats.items() if k.startswith("cleaned_")},
    }
    return bytes(blob), report


def main() -> int:
    parser = argparse.ArgumentParser(description="打包 limelight 剧本(供 AI Passport 移植)")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--chunk", type=int, default=250, help="每块对白条数(默认 250)")
    parser.add_argument("--keep-raw", action="store_true",
                        help="不清洗正文(保留源数据里的排版指令与译者备注)")
    parser.add_argument("--stored", action="store_true",
                        help="不压缩(调试/宿主测试用:不需要 inflate)")
    args = parser.parse_args()
    if not (16 <= args.chunk <= 4096):
        parser.error("--chunk 必须在 16..4096")

    blob, report = build(args.source, args.chunk, args.stored, args.keep_raw)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f"pack {args.out}: {report['bytes']} B = {report['bytes'] / 1048576:.3f} MiB")
    print(f"  对白 {report['entries']} 条 / 块 {report['chunks']} 个"
          f"(每块 {args.chunk} 条) / 名字 {report['names']} 条 / 章节 {report['chapters']}"
          f" / 选项 {report['choices']} 处")
    if report.get("cleaned"):
        print(f"  正文清洗: " + ", ".join(f"{k} {v} 条" for k, v in sorted(report["cleaned"].items())))
    print(f"  块内原始 {report['raw_bytes']} B -> 存储 {report['packed_bytes']} B"
          f" ({100 * report['packed_bytes'] / report['raw_bytes']:.0f}%),"
          f" 名字池 {report['name_bytes']} B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
