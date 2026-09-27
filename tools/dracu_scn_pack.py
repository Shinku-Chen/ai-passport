#!/usr/bin/env python3
"""Build the DRACU-RIOT script pack for the AI Passport port.

Source: the linear page table of github.com/hezdaaa/dracu-riot-miband
(src/common/script/scriptData*.txt) plus its branch configuration
(转换工具/branchConfig.js).  See tools/dracu_source.py for the field meanings.

The pack is read straight out of flash, in small self-contained deflate blocks
so the firmware only needs a 4 KB scratch buffer (no 32 KB inflate window):

  header  : magic "DRACUSC1", version u16, header_size u16, section_count u16,
            reserved u16, total u32                                    (20 B)
  section : { type u32, offset u32, count u32, size u32 }              (16 B x N)

  SEC_CHAR   (0) u32 count + count x u16 码位(按字频降序;下标 = 正文里的字符编码,
                 同时也是字库里的字形顺序)
  SEC_STR    (1) u32 count + count x (u16 字符数 + 字符码...)  # 说话人 / 结局名 / 章节标题
  SEC_BLOCK  (2) u16 count + u16 page_block_count + u32 page_count + u32 pages_per_block,
                 然后 count x { off u32, comp u32, raw u32 }(off 相对 SEC_BLOB)
                 前 page_block_count 块是页表流(每块固定 256 页),其余是正文流
  SEC_BLOB   (3) 逐块 raw deflate(zlib wbits=-15)
  SEC_BRANCH (4) 分支表(见下)
  SEC_CHOICE (5) 选项页表
  SEC_CHAPTERS(6) 章节表
  SEC_META   (7) key=value

  页记录 16 字节,位于页表流的偏移 N*16 处(N = 页号 - 1):
    text_off u32   正文在正文流里的原始字节偏移(0xFFFFFFFF = 无正文)
    text_len u16   正文的字符数(不是字节数)
    cg u16         事件 CG id,0xFFFF = 无
    sprite u16     立绘 id,0xFFFF = 无
    bg u8          背景 id,0xFF = 无(黑屏)
    sd u8          SD id,0xFF = 无
    name u8        说话人 id,0xFF = 旁白
    flags u8       bit0 模糊 bit1 闪光 bit2 震动 bit3 选项页
    cs u8          立绘缩放百分比(源数据,设备侧按 100 处理)
    fs u8          字号百分比

  SEC_BRANCH 布局(全部小端):
    u32 no_next_count,  no_next[count] : { page u32, target u32 }
    u32 no_back_count,  no_back[count] : { page u32, target u32 }
    u32 end_count,      end[count]     : { page u32, name u16 }
    u32 cond_count,     cond[count]    : u32 页号(条件里出现过的选项页;选择历史按此下标存)
    u32 hidden_count,   hidden[count]  : { page u32, rule_off u32, rule_count u8, pad u8,
                                           fallback u32 }
    u32 rule_count,     rules[count]   : { lit_off u32, lit_count u8, pad u8, target u32 }
    u32 lit_count,      lits[count]    : { cond_index u8, value u8 }

  SEC_CHOICE 布局:
    u32 count, count x { page u32, options u8, pad u8, pad u16,
                         5 x { text_off u32, text_len u16, pad u16, target u32 } }

  SEC_CHAPTERS 布局:
    u32 count, count x { page u32, name u16, route u8, pad u8 }

Usage:
  python tools/dracu_scn_pack.py --source build/dracu-source \
      --out build/dracu-pack/dracu_scn.bin
  python tools/dracu_scn_pack.py --check build/dracu-pack/dracu_scn.bin
"""

from __future__ import annotations

import argparse
import collections
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import dracu_branch as B  # noqa: E402
import dracu_source as S  # noqa: E402

MAGIC = b"DRACUSC1"
VERSION = 1
HEADER = struct.Struct("<8sHHHHI")
SECTION = struct.Struct("<IIII")
PAGE = struct.Struct("<IHHHBBBBBB")
assert PAGE.size == 16
BLOCK = struct.Struct("<III")
BLOCK_HEADER = struct.Struct("<HHII")
assert BLOCK_HEADER.size == 12
CHOICE_SLOT = struct.Struct("<IHHI")
CHOICE_HEAD = struct.Struct("<IBBH")
assert CHOICE_HEAD.size == 8 and CHOICE_SLOT.size == 12

SEC_CHAR, SEC_STR, SEC_BLOCK, SEC_BLOB, SEC_BRANCH, SEC_CHOICE, SEC_CHAPTERS, SEC_META = range(8)

# 标题图在图片包里的条目名(与 tools/dracu_pack.py 的 --title-name 一致)
TITLE_NAME = "标题画面"

FLAG_BLUR = 1 << 0
FLAG_FLASH = 1 << 1
FLAG_VIBRATE = 1 << 2
FLAG_CHOOSE = 1 << 3

PAGES_PER_BLOCK = 256                     # 256 x 16 = 4096 字节
TEXT_BLOCK_RAW = 3000                     # 正文块上限(解压缓冲 4 KB)
# 上游 branchConfig 的已知缺陷(本次移植时实测发现并修正,原样搬运会让玩家卡死):
# 尼古拉线第 4 话有两份重复的 H 场景块(20587-20611 与 20995-21019),两份的收场
# 变体(中出 20607 / 外射 21015 / 中出 21019)在 noNextPages 里都被导回 20612
# ——那是**第一份**的后日谈,走完又回到第二份选项目,于是 20608..21011 这 400 页
# 无限循环(选项页的两个选项都指向同一后续,玩家只能靠菜单里的"跳过章节"逃出去)。
# 剧本页表的实际顺序是:21012-21015/21016-21019 之后紧接着就是 21020 起的后日谈
# 与 21029 起的第 5 话,所以把这两条改指 21020。
NO_NEXT_FIXES = {
    21015: 21020,   # 第二份 H 场景·中出变体 收场
    21019: 21020,   # 第二份 H 场景·外射变体 收场
}

ROUTE_LABELS = {
    "commonChapters": "共通线", "miuChapters": "美羽", "rioChapters": "莉音",
    "azuChapters": "梓", "eriChapters": "艾莉娜", "nicChapters": "尼古拉",
}


def log(message: str) -> None:
    print(message, file=sys.stderr)


def human(count: int) -> str:
    return f"{count / 1048576:.2f} MB" if abs(count) >= 1048576 else f"{count / 1024:.1f} KB"


def deflate_block(data: bytes) -> bytes:
    """zlib 流(带 2 字节头 + adler32):设备侧的 dracu_inflate 按 zlib 封装解析。

    不要写裸 deflate:固件的 inflate 会先读 CMF/FLG,裸流会被判成"头不合法",
    表现是"一进阅读就卡住/全剧终"(本仓库的前一次移植踩过同一个坑)。
    """
    return zlib.compress(data, 9)


class CharTable:
    """按字频降序的字符表:正文以 2 字节下标存,顺序同时是字库的字形顺序。"""

    def __init__(self, texts) -> None:
        counter = collections.Counter("".join(texts))
        self.chars = [char for char, _count in counter.most_common()]
        if len(self.chars) > 0xFFFF:
            raise SystemExit(f"字符表 {len(self.chars)} 项超出 u16")
        self.index = {char: position for position, char in enumerate(self.chars)}

    def encode(self, text: str) -> bytes:
        return b"".join(struct.pack("<H", self.index[char]) for char in text)

    def blob(self) -> bytes:
        return struct.pack("<I", len(self.chars)) + b"".join(
            struct.pack("<H", ord(char)) for char in self.chars)


class StringTable:
    """去重字符串表(说话人 / 结局名 / 章节标题),按首次出现顺序编号。"""

    def __init__(self) -> None:
        self.strings: list[str] = []
        self.index: dict[str, int] = {}

    def add(self, text: str) -> int:
        if text not in self.index:
            if len(self.strings) >= 0xFFFF:
                raise SystemExit("字符串表超出 u16")
            self.index[text] = len(self.strings)
            self.strings.append(text)
        return self.index[text]

    def blob(self, table: CharTable) -> bytes:
        out = bytearray(struct.pack("<I", len(self.strings)))
        for text in self.strings:
            out += struct.pack("<H", len(text)) + table.encode(text)
        return bytes(out)


@dataclass
class BlockStream:
    """按上限切块的字节流:记录每条的原始偏移,块边界保证条目不跨块。"""

    limit: int
    data: bytearray = None
    blocks: list[bytes] = None

    def __post_init__(self) -> None:
        self.data = bytearray()
        self.blocks = []

    def add(self, payload: bytes) -> int:
        """追加一条,返回它在流里的原始偏移;必要时先封块(不切分条目)。"""
        if len(self.data) - self._closed() + len(payload) > self.limit:
            self._close()
        offset = len(self.data)
        self.data += payload
        return offset

    def _closed(self) -> int:
        return sum(len(block) for block in self.blocks)

    def _close(self) -> None:
        used = self._closed()
        if used < len(self.data):
            self.blocks.append(bytes(self.data[used:]))

    def finish(self) -> list[bytes]:
        self._close()
        return self.blocks


def split_fixed(payload: bytes, block_bytes: int) -> list[bytes]:
    return [payload[start:start + block_bytes] for start in range(0, len(payload), block_bytes)]


# --------------------------------------------------------------------------
# 打包
# --------------------------------------------------------------------------


@dataclass
class ScriptPacker:
    source: Path

    def __post_init__(self) -> None:
        self.pages = S.load_pages(self.source)
        self.refs = S.collect_refs(self.pages)
        self.config = S.load_branch_config(self.source)
        self.chapters = S.load_chapters(self.source)
        self.strings = StringTable()
        self.build_id_maps()
        self.texts = [str(page.get("t") or "") for page in self.pages.values()]
        # 字符表要覆盖后面可能进字符串表的每一个字:正文、选项文案、说话人、
        # 结局名、章节标签。漏一个就会在写字符串表时 KeyError。
        self.char_table = CharTable(
            self.texts + self.all_choice_texts() + self.all_strings())

    def all_choice_texts(self) -> list[str]:
        return [str(page.get(f"c{index}") or "")
                for page in self.pages.values() for index in range(1, 6)]

    def all_strings(self) -> list[str]:
        names = [str(page.get("s") or "") for page in self.pages.values()]
        names += [str(name) for name in self.config["end"].values()]
        names += [label for _route, _page, label in self.chapters]
        return names

    def build_id_maps(self) -> None:
        # 背景 id 空间 = 图片包里背景池的顺序:标题图也是池里的一个条目
        # (按 (pool, name) 升序排),所以这里把标题图一起排序 —— 漏了它就会让
        # 整个背景 id 移位(画面显示成别的背景),tests/test_dracu_pack_layout.py 会抓。
        names = set(self.refs.bg) | {TITLE_NAME}
        self.bg_ids = {name: index for index, name in enumerate(sorted(names))}
        self.cg_ids = {name: i for i, name in enumerate(S.sorted_names(self.refs.cg))}
        self.sd_ids = {name: i for i, name in enumerate(S.sorted_names(self.refs.sd))}
        self.sprite_ids = {name: i for i, name in enumerate(S.sorted_names(self.refs.sprite))}

    # -- 正文 -------------------------------------------------------------
    def build_text_stream(self) -> tuple[BlockStream, dict[int, tuple[int, int]]]:
        stream = BlockStream(TEXT_BLOCK_RAW)
        offsets: dict[int, tuple[int, int]] = {}
        for number in sorted(self.pages):
            text = str(self.pages[number].get("t") or "")
            if not text:
                offsets[number] = (0xFFFFFFFF, 0)
                continue
            offsets[number] = (stream.add(self.char_table.encode(text)), len(text))
        return stream, offsets

    def build_choice_texts(self, stream: BlockStream) -> dict[tuple[int, int], tuple[int, int]]:
        """选项文案也要进正文流(排在正文之后)。"""
        offsets: dict[tuple[int, int], tuple[int, int]] = {}
        for number, page in self.choices:
            for slot in range(1, 6):
                text = str(page.get(f"c{slot}") or "")
                if text.strip():
                    offsets[(number, slot)] = (stream.add(self.char_table.encode(text)), len(text))
        return offsets

    # -- 页表 -------------------------------------------------------------
    def page_record(self, number: int, offsets) -> bytes:
        page = self.pages[number]
        text_off, text_len = offsets[number]
        flags = 0
        if page.get("blur"):
            flags |= FLAG_BLUR
        if page.get("f"):
            flags |= FLAG_FLASH
        if page.get("e"):
            flags |= FLAG_VIBRATE
        if page.get("co"):
            flags |= FLAG_CHOOSE
        name = str(page.get("s") or "")
        name_id = self.strings.add(name) if name else 0xFFFF
        bg = str(page.get("b") or "")
        cg = str(page.get("cg") or "").split("@")[0].strip()
        sd = str(page.get("sd") or "").split("@")[0].strip()
        sprite = str(page.get("c") or "").split(";")[0].split("@")[0].strip()
        cs = int(page.get("cs") or 100)
        fs = int(page.get("fs") or 100)
        return PAGE.pack(
            text_off, text_len,
            self.cg_ids.get(cg, 0xFFFF) if cg else 0xFFFF,
            self.sprite_ids.get(sprite, 0xFFFF) if sprite else 0xFFFF,
            self.bg_ids.get(bg, 0xFF) if bg else 0xFF,
            self.sd_ids.get(sd, 0xFF) if sd else 0xFF,
            0xFF if name_id == 0xFFFF else name_id,
            flags,
            cs if 0 <= cs <= 255 else 100,
            fs if 0 <= fs <= 255 else 100,
        )

    # -- 选项 -------------------------------------------------------------
    def build_choices(self, text_offsets) -> bytes:
        blob = bytearray(struct.pack("<I", len(self.choices)))
        for number, page in self.choices:
            texts = [str(page.get(f"c{index}") or "") for index in range(1, 6)]
            targets = [page.get(f"c{index}t") for index in range(1, 6)]
            count = sum(1 for text in texts if text.strip())
            blob += CHOICE_HEAD.pack(number, count, 0, 0)
            for slot in range(5):
                text = texts[slot]
                target = int(targets[slot]) if str(targets[slot] or "").strip() else 0xFFFFFFFF
                if text.strip():
                    offset, length = text_offsets[(number, slot + 1)]
                    blob += CHOICE_SLOT.pack(offset, length, 0, target)
                else:
                    blob += CHOICE_SLOT.pack(0xFFFFFFFF, 0, 0, 0xFFFFFFFF)
        return bytes(blob)

    # -- 分支 -------------------------------------------------------------
    def build_branch(self) -> tuple[bytes, dict]:
        counts = B.option_counts(self.pages)
        cond_pages = B.build_cond_pages(self.config["hidden"])
        cond_index = {page: index for index, page in enumerate(cond_pages)}
        self.cond_pages = cond_pages
        # 上游已知缺陷:两条收场跳转被导回重复场景块,会把玩家卡在 400 页循环里
        no_next_map = dict(self.config["no_next"])
        for page, target in NO_NEXT_FIXES.items():
            old_target = no_next_map.get(page)
            if old_target == target:
                continue
            if old_target is None:
                log(f"提示: noNextPages 里没有 {page},跳过这条上游修正")
                continue
            log(f"修正上游跳转: noNextPages[{page}] {old_target} -> {target}"
                f"(原目标是重复场景块,会让玩家在 20608..21011 之间无限循环)")
            no_next_map[page] = target
        no_next = sorted(no_next_map.items())
        no_back = sorted(self.config["no_back"].items())
        endings = sorted(self.config["end"].items())
        hidden = sorted(self.config["hidden"].items())
        out = bytearray()
        out += struct.pack("<I", len(no_next))
        for page, target in no_next:
            out += struct.pack("<II", page, target)
        out += struct.pack("<I", len(no_back))
        for page, target in no_back:
            out += struct.pack("<II", page, target)
        out += struct.pack("<I", len(endings))
        for page, name in endings:
            out += struct.pack("<IH", page, self.strings.add(name))
        out += struct.pack("<I", len(cond_pages))
        for page in cond_pages:
            out += struct.pack("<I", page)
        rule_blob = bytearray()
        lit_blob = bytearray()
        hidden_records = bytearray()
        for page, entry in hidden:
            compiled = B.compile_hidden(entry, counts)
            B.verify_hidden(entry, compiled, counts)
            rule_off = len(rule_blob) // 10
            for clause, target in compiled["rules"]:
                lit_off = len(lit_blob) // 2
                for literal in clause:
                    lit_blob += struct.pack("<BB", cond_index[literal.page], literal.value)
                rule_blob += struct.pack("<IBBI", lit_off, len(clause), 0, target)
            hidden_records += struct.pack("<IIBBI", page, rule_off, len(compiled["rules"]), 0,
                                          compiled["fallback"])
        out += struct.pack("<I", len(hidden))
        out += hidden_records
        out += struct.pack("<I", len(rule_blob) // 10)
        out += rule_blob
        out += struct.pack("<I", len(lit_blob) // 2)
        out += lit_blob
        stats = {
            "no_next": len(no_next), "no_back": len(no_back), "end": len(endings),
            "hidden": len(hidden), "rules": len(rule_blob) // 10,
            "literals": len(lit_blob) // 2, "cond_pages": len(cond_pages),
        }
        return bytes(out), stats

    # -- 章节 -------------------------------------------------------------
    def build_chapters(self) -> bytes:
        """按页号升序写章节表。

        源工程的 lct.ux 是按"路线"分组列章节的(梓线排在美羽线前面),页号因此不是
        升序;而固件要按页号找"当前在第几章"和"下一章在哪",所以这里按页号重排 ——
        路线编号保留在记录里。tests/test_dracu_scn_pack.py 会守住这个顺序。
        """
        route_ids: dict[str, int] = {}
        blob = bytearray()
        for route, page, label in sorted(self.chapters, key=lambda row: row[1]):
            route_ids.setdefault(route, len(route_ids))
            blob += struct.pack("<IHBB", page, self.strings.add(label), route_ids[route], 0)
        self.routes = list(route_ids)
        return struct.pack("<I", len(self.chapters)) + bytes(blob)

    # -- 组装 -------------------------------------------------------------
    def build(self) -> tuple[bytes, dict]:
        self.choices = [(number, page) for number, page in sorted(self.pages.items())
                        if page.get("co")]
        text_stream, text_offsets = self.build_text_stream()
        choice_offsets = self.build_choice_texts(text_stream)
        records = bytearray()
        for number in sorted(self.pages):
            records += self.page_record(number, text_offsets)
        choice_blob = self.build_choices(choice_offsets)
        branch_blob, branch_stats = self.build_branch()
        chapters_blob = self.build_chapters()
        text_blocks = text_stream.finish()
        page_blocks = split_fixed(bytes(records), PAGES_PER_BLOCK * PAGE.size)
        self.page_block_count = len(page_blocks)
        blocks = page_blocks + text_blocks
        compressed = [deflate_block(block) for block in blocks]
        table = bytearray(BLOCK_HEADER.pack(len(blocks), len(page_blocks), len(self.pages),
                                            PAGES_PER_BLOCK))
        offset = 0
        for block, body in zip(blocks, compressed):
            table += BLOCK.pack(offset, len(body), len(block))
            offset += len(body)
        meta = {
            "source": "github.com/hezdaaa/dracu-riot-miband",
            "pages": str(len(self.pages)),
            "page_block_count": str(len(page_blocks)),
            "text_block_count": str(len(text_blocks)),
            "pages_per_block": str(PAGES_PER_BLOCK),
            "page_raw": str(len(records)),
            "text_raw": str(len(text_stream.data)),
            "text_chars": str(sum(len(text) for text in self.texts)),
            "choice_pages": str(len(self.choices)),
            "generator": "tools/dracu_scn_pack.py",
        }
        sections = [
            (SEC_CHAR, len(self.char_table.chars), self.char_table.blob()),
            (SEC_STR, len(self.strings.strings), self.strings.blob(self.char_table)),
            (SEC_BLOCK, len(blocks), bytes(table)),
            (SEC_BLOB, len(blocks), b"".join(compressed)),
            (SEC_BRANCH, branch_stats["hidden"], branch_blob),
            (SEC_CHOICE, len(self.choices), choice_blob),
            (SEC_CHAPTERS, len(self.chapters), chapters_blob),
            (SEC_META, 0, "".join(f"{key}={value}\n" for key, value in meta.items()).encode()),
        ]
        stats = {
            "meta": meta,
            "page_bytes": len(records),
            "text_bytes": len(text_stream.data),
            "compressed": sum(len(body) for body in compressed),
            "page_compressed": sum(len(body) for body in compressed[:len(page_blocks)]),
            "text_compressed": sum(len(body) for body in compressed[len(page_blocks):]),
            "branch": branch_stats,
        }
        return assemble(sections), stats


def assemble(sections: list[tuple[int, int, bytes]]) -> bytes:
    header_size = HEADER.size + SECTION.size * len(sections)
    offset = header_size
    table = bytearray()
    payload = bytearray()
    for kind, count, data in sections:
        table += SECTION.pack(kind, offset, count, len(data))
        payload += data
        offset += len(data)
    total = header_size + len(payload)
    return (HEADER.pack(MAGIC, VERSION, header_size, len(sections), 0, total)
            + bytes(table) + bytes(payload))


# --------------------------------------------------------------------------
# 自检
# --------------------------------------------------------------------------


def load_pack(path: Path):
    raw = path.read_bytes()
    magic, version, header_size, section_count, _reserved, total = HEADER.unpack_from(raw, 0)
    if magic != MAGIC or version != VERSION or total != len(raw):
        raise SystemExit(f"{path}: 魔数/版本/长度不符")
    sections = {}
    for index in range(section_count):
        kind, offset, count, size = SECTION.unpack_from(raw, HEADER.size + index * SECTION.size)
        sections[kind] = (offset, count, size)
    return raw, sections


def decompress_block(raw: bytes, blob_off: int, off: int, comp: int, raw_len: int) -> bytes:
    """按设备侧同一个约定解块:zlib 流(wbits=15,带头与 adler32)。"""
    data = zlib.decompressobj().decompress(raw[blob_off + off:blob_off + off + comp], raw_len + 1)
    if len(data) != raw_len:
        raise ValueError(f"块解压长度 {len(data)} != {raw_len}")
    return data


def meta_of(raw: bytes, sections) -> dict[str, str]:
    offset, _count, size = sections[SEC_META]
    text = raw[offset:offset + size].decode("utf-8", errors="replace")
    meta: dict[str, str] = {}
    for line in text.splitlines():
        key, _, value = line.partition("=")
        meta[key] = value
    return meta


def page_block_count(raw: bytes, sections, block_count: int) -> int:
    """页表块数(旧包没有头部时退回全部块)。"""
    offset, _count, size = sections[SEC_BLOCK]
    if size >= BLOCK_HEADER.size:
        return BLOCK_HEADER.unpack_from(raw, offset)[1]
    return block_count


def check_pack(path: Path) -> int:
    raw, sections = load_pack(path)
    errors: list[str] = []
    char_off, char_count, char_size = sections[SEC_CHAR]
    block_off, block_count, block_size = sections[SEC_BLOCK]
    blob_off, _blob_count, blob_size = sections[SEC_BLOB]
    choice_off, choice_count, choice_size = sections[SEC_CHOICE]
    chapter_off, chapter_count, _chapter_size = sections[SEC_CHAPTERS]
    if char_size != 4 + char_count * 2:
        errors.append("SEC_CHAR 长度不符")
    if block_size < BLOCK_HEADER.size:
        errors.append("SEC_BLOCK 头部不完整")
    else:
        stored_blocks, page_block_count, page_count, pages_per_block = \
            BLOCK_HEADER.unpack_from(raw, block_off)
        if stored_blocks != block_count:
            errors.append(f"块数 {block_count} != 头部 {stored_blocks}")
        if block_size != BLOCK_HEADER.size + block_count * BLOCK.size:
            errors.append("SEC_BLOCK 长度不符")
    if choice_size != 4 + choice_count * (CHOICE_HEAD.size + 5 * CHOICE_SLOT.size):
        errors.append(f"SEC_CHOICE 长度 {choice_size} 与 {choice_count} 项不符")
    page_blocks = page_block_count
    text_blocks = block_count - page_blocks
    for index in range(block_count):
        off, comp, raw_len = BLOCK.unpack_from(raw, block_off + BLOCK_HEADER.size + index * BLOCK.size)
        if off + comp > blob_size:
            errors.append(f"块 {index}: 越界")
            continue
        if index < page_blocks - 1 and raw_len != PAGES_PER_BLOCK * PAGE.size:
            errors.append(f"块 {index}: 页表块 {raw_len} 字节不是整块")
        if index == page_blocks - 1 and raw_len > PAGES_PER_BLOCK * PAGE.size:
            errors.append(f"块 {index}: 末页表块 {raw_len} 字节过大")
        try:
            decompress_block(raw, blob_off, off, comp, raw_len)
        except Exception as exc:  # noqa: BLE001
            errors.append(f"块 {index}: 解压失败 {exc}")
    if chapter_count:
        for index in range(chapter_count):
            page, name, route, _pad = struct.unpack_from("<IHBB", raw, chapter_off + 4 + index * 8)
            if page < 1 or page > 1_000_000:
                errors.append(f"章节 {index}: 页号异常 {page}")
    log(f"OK: {path} 字符 {char_count} 页 {page_count} 块 {block_count}(页表 {page_blocks} / "
        f"正文 {text_blocks}) 选项 {choice_count} 章节 {chapter_count} 载荷 {human(blob_size)}")
    if errors:
        for message in errors[:20]:
            log(f"错误: {message}")
        return 1
    return 0


# --------------------------------------------------------------------------
# 命令行
# --------------------------------------------------------------------------


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="构建 DRACU-RIOT 剧本包")
    parser.add_argument("--source", type=Path, default=Path("build/dracu-source"))
    parser.add_argument("--out", type=Path, default=Path("build/dracu-pack/dracu_scn.bin"))
    parser.add_argument("--check", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.check:
        return check_pack(args.check)
    source = S.find_source(args.source)
    packer = ScriptPacker(source)
    pack, stats = packer.build()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(pack)
    meta = stats["meta"]
    log(f"剧本包: {args.out} ({human(len(pack))})")
    log(f"  页 {meta['pages']} 页表 {human(stats['page_bytes'])} -> "
        f"{human(stats['page_compressed'])} ({meta['page_block_count']} 块)")
    log(f"  正文 {human(stats['text_bytes'])} / {meta['text_chars']} 字 -> "
        f"{human(stats['text_compressed'])} ({meta['text_block_count']} 块);"
        f"字符表 {len(packer.char_table.chars)} 字")
    log(f"  分支: {stats['branch']}")
    return check_pack(args.out)


if __name__ == "__main__":
    raise SystemExit(main())
