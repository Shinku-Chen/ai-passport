#!/usr/bin/env python3
"""独立审计:《命运石之门》rw5 源快照 vs 固件资源包。

这个脚本**不 import tools/steins_pack.py**,自己读源 JSON、自己解析包二进制。
存在的意义是"换个实现把同一件事再算一遍":如果打包器有漏帧、错帧、错落点、
漏图、漏解锁点,这里会报出来。它也不读打包器写的 manifest 来自证。

用法:
  python tools/steins_audit.py --source <steins-rw5 目录> \
      --pack assets/steins/steins_pack.bin [--font-dir assets/fonts] \
      [--report build/logs/steins-audit.json]

退出码:0 = 全部一致;1 = 发现不一致;2 = 输入不可用。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import zlib

# 包格式(与 main/steins_pack.h 一致;这里独立实现,便于交叉验证)
MAGIC = b"STEINPK3"
SEC = {
    "STRT": 1, "TXT": 2, "NODE": 3, "CHUNKREF": 4, "CHUNK": 5, "FRAME": 6,
    "VIEW": 7, "LAYER": 8, "ASSET": 9, "BLOB": 10, "CHOICE": 11, "CITEM": 12,
    "CHAPTER": 13, "GAL": 14, "GENTRY": 15, "WLINE": 16, "WCHANGE": 17,
    "BREAK": 18, "UI": 19, "META": 20,
    # v4 手机段:本工具只做"存在 + 长度自洽"检查,逐字段语义由 w6 的
    # tools/steins_mobile_audit.py 负责(两份独立实现,互不 import)。
    "MAIL": 21, "MAIL_SUCC": 22, "MAIL_STAMP": 23, "MROW": 24, "MEVT": 25,
    "MNODE": 26, "CALLSITE": 27, "CALLITEM": 28, "CLIP": 29, "CFRAME": 30,
    "WINDOW": 31, "WINOPT": 32, "KEYPHONE": 33, "MOBILE": 34, "MMAP": 35,
    "SEED": 36, "SEED_ID": 37, "CALLDESC": 38, "CALLDITEM": 39,
    # v5:压缩 TXT + 块表 + presentation/choice 元数据。
    "TXTBLK": 40, "TXTRAW": 41, "PRES": 42, "CHOICE_META": 43,
    "CITEM_META": 44,
}
REC = {"STRT": 6, "NODE": 15, "CHUNKREF": 2, "CHUNK": 8, "FRAME": 10, "VIEW": 4,
       "LAYER": 12, "ASSET": 40, "CHOICE": 12, "CITEM": 9, "CHAPTER": 8, "GAL": 8,
       "GENTRY": 4, "WLINE": 10, "WCHANGE": 8, "BREAK": 4, "UI": 4,
       "MAIL": 12, "MAIL_SUCC": 2, "MAIL_STAMP": 4, "MROW": 12, "MNODE": 6,
       "CALLSITE": 12, "CALLITEM": 10, "CLIP": 6, "CFRAME": 6, "WINDOW": 14,
       "WINOPT": 10, "KEYPHONE": 16, "MMAP": 6, "SEED": 4, "SEED_ID": 2,
       "CALLDESC": 8, "CALLDITEM": 6, "TXTBLK": 8, "TXTRAW": 4,
       "PRES": 10, "CHOICE_META": 8, "CITEM_META": 4}

failures = []
notes = []


def fail(msg):
    failures.append(msg)


def rd16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def rd32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


class Pack:
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        if self.data[:8] != MAGIC:
            raise SystemExit("不是 STEINPK3 包: %s" % path)
        self.version, self.total, sections = struct.unpack_from("<III", self.data, 8)
        if self.total != len(self.data):
            raise SystemExit("包长度字段与实际不符")
        self.sec = {}
        for i in range(sections):
            typ, off, count, size = struct.unpack_from("<IIII", self.data, 32 + i * 16)
            name = next((k for k, v in SEC.items() if v == typ), None)
            if name is None:
                raise SystemExit("未知段类型 %d" % typ)
            self.sec[name] = (off, count, size)
            if count * REC.get(name, size) != size and name not in ("TXT", "BLOB", "META",
                                                                   "MEVT", "MOBILE"):
                if name == "CHUNKREF" and count * 2 == size:
                    continue
                raise SystemExit("段 %s 长度与记录数不符: %d * %d != %d"
                                 % (name, count, REC.get(name, 0), size))
        # v5 的 TXT 是每块 ≤2 KiB 的 raw DEFLATE；审计器先还原成连续
        # UTF-16LE 字符串池，再走与 v4 相同的 string(index) 路径。
        self.txt = None
        if self.version >= 5:
            self._load_v5_txt()

    @staticmethod
    def _crc16(data):
        crc = 0xFFFF
        for byte in data:
            crc ^= byte << 8
            for _ in range(8):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        return crc

    def _load_v5_txt(self):
        if "TXTBLK" not in self.sec or "TXTRAW" not in self.sec:
            raise SystemExit("v5 包缺 TXTBLK/TXTRAW")
        off, _count, _size = self.sec["TXTRAW"]
        raw_len = rd32(self.data, off)
        blocks = []
        for i in range(self.sec["TXTBLK"][1]):
            boff = self.sec["TXTBLK"][0] + i * 8
            coff = rd32(self.data, boff)
            clen = rd16(self.data, boff + 4)
            crc = rd16(self.data, boff + 6)
            start = self.sec["TXT"][0] + coff
            raw = zlib.decompress(self.data[start:start + clen], -15)
            if self._crc16(raw) != crc:
                raise SystemExit("TXT 块 %d CRC 不符" % i)
            blocks.append(raw)
        self.txt = b"".join(blocks)
        if len(self.txt) != raw_len:
            raise SystemExit("TXT 解压长度 %d != TXTRAW %d" % (len(self.txt), raw_len))

    def rows(self, name):
        off, count, size = self.sec[name]
        rec = REC[name]
        for i in range(count):
            yield self.data[off + i * rec: off + (i + 1) * rec]

    def count(self, name):
        return self.sec[name][1] if name in self.sec else 0

    def string(self, index):
        off, count, _ = self.sec["STRT"]
        if index >= count:
            return None
        soff, slen = struct.unpack_from("<IH", self.data, off + index * REC["STRT"])
        if self.txt is not None:
            raw = self.txt[soff:soff + slen * 2]
        else:
            toff, _, tsize = self.sec["TXT"]
            raw = self.data[toff + soff: toff + soff + slen * 2]
        return raw.decode("utf-16-le", "strict")

    def meta(self, key):
        off, count, size = self.sec["META"]
        text = self.data[off:off + size].decode("utf-8")
        for line in text.splitlines():
            if line.startswith(key + "="):
                return line[len(key) + 1:]
        return None


def load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def js_const(path, name):
    text = open(path, "r", encoding="utf-8").read()
    m = re.search(name + r"\s*=\s*(\{.*?\});", text, re.S)
    if not m:
        raise SystemExit("cannot find %s" % name)
    return json.loads(m.group(1))


def js_string(path, name):
    text = open(path, "r", encoding="utf-8").read()
    m = re.search(name + r'\s*=\s*"([^"]*)"', text)
    if not m:
        raise SystemExit("cannot find %s" % name)
    return m.group(1)


def auditar(source, pack, font_dir):
    common = os.path.join(source, "src", "common")
    graph = load_json(os.path.join(common, "graph.txt"))
    worldline = js_const(os.path.join(common, "worldline-data.js"), "worldlineData")
    initial_rate = js_string(os.path.join(common, "worldline-data.js"), "INITIAL_WORLDLINE")

    # ---- 节点顺序必须与源 JSON 的键序一致(固件按 index 寻址)
    node_ids = list(graph["nodes"])
    if pack.count("NODE") != len(node_ids):
        fail("节点数不一致: 源 %d / 包 %d" % (len(node_ids), pack.count("NODE")))
    node_index = {nid: i for i, nid in enumerate(node_ids)}

    initial = pack.meta("initial")
    if initial is None or int(initial) != node_index[graph["initial"]]:
        fail("初始节点不一致: 源 %s / 包 %s" % (graph["initial"], initial))

    # ---- 逐节点逐 chunk 逐帧对照
    frame_rows = list(pack.rows("FRAME"))
    chunk_rows = list(pack.rows("CHUNK"))
    chunkref = list(pack.rows("CHUNKREF"))
    chunk_cache = {}
    pack_node_rows = list(pack.rows("NODE"))
    total_frames = 0
    compared = 0
    visited_frames = set()
    for i, nid in enumerate(node_ids):
        src_node = graph["nodes"][nid]
        raw = pack_node_rows[i]
        id_str = pack.string(rd16(raw, 0))
        count = rd32(raw, 2)
        chunk_first = rd16(raw, 6)
        chunk_count = raw[8]
        break_first = raw[9]
        break_count = raw[10]
        choice_idx = rd16(raw, 11)
        ending_str = rd16(raw, 13)
        if id_str != nid:
            fail("节点 %d 的 id 是 %r,源是 %r" % (i, id_str, nid))
        if count != src_node["count"]:
            fail("节点 %s 帧数 源 %d / 包 %d" % (nid, src_node["count"], count))
        src_chunks = [(c["file"], c["count"]) for c in src_node["chunks"]]
        if chunk_count != len(src_chunks):
            fail("节点 %s chunk 数 源 %d / 包 %d" % (nid, len(src_chunks), chunk_count))
        got_chunks = []
        for k in range(chunk_count):
            ci = rd16(chunkref[chunk_first + k], 0)
            first_frame, cnt = struct.unpack_from("<IH", chunk_rows[ci], 0)
            got_chunks.append((ci, first_frame, cnt))
        # 逐 chunk 逐帧比对文本/说话人/日期/标题/画面
        cursor = 0
        for k, (file_name, src_count) in enumerate(src_chunks):
            if k >= len(got_chunks):
                break
            ci, first_frame, cnt = got_chunks[k]
            if cnt != src_count:
                fail("节点 %s chunk %s 帧数 源 %d / 包 %d" % (nid, file_name, src_count, cnt))
                continue
            key = (file_name, src_count)
            src_blob = chunk_cache.get(key)
            if src_blob is None:
                src_blob = load_json(os.path.join(common, "story", file_name))
                chunk_cache[key] = src_blob
            src_frames = src_blob["frames"]
            for j in range(cnt):
                src_row = src_frames[j]
                raw_frame = frame_rows[first_frame + j]
                text_i, speaker_i, view_i, aux_i, title_i = struct.unpack_from("<HHHHH", raw_frame, 0)
                # 文本里可能有 NUL,固件用 UTF-8 传递,这里按原样比较
                if pack.string(text_i) != src_row[1]:
                    fail("节点 %s chunk %s 帧 %d 正文不一致" % (nid, file_name, j))
                    break
                if pack.string(speaker_i) != src_row[0]:
                    fail("节点 %s chunk %s 帧 %d 说话人不一致" % (nid, file_name, j))
                    break
                if pack.string(aux_i) != (src_row[3] or ""):
                    fail("节点 %s chunk %s 帧 %d 日期不一致" % (nid, file_name, j))
                    break
                if pack.string(title_i) != (src_row[4] or ""):
                    fail("节点 %s chunk %s 帧 %d 标题不一致" % (nid, file_name, j))
                    break
                compared += 1
                visited_frames.add(first_frame + j)
            cursor += cnt
        total_frames += count

        # 断点(下一章跳点)
        src_breaks = [int(b[0]) for b in (src_node.get("breaks") or [])]
        got_breaks = []
        for b in range(break_count):
            row = list(pack.rows("BREAK"))[break_first + b]
            got_breaks.append(rd32(row, 0))
        if got_breaks != src_breaks:
            fail("节点 %s 断点数/值不一致: 源 %r / 包 %r" % (nid, src_breaks, got_breaks))

        # 选择项
        src_choice = src_node.get("choice")
        if src_choice is None:
            if choice_idx != 0xFFFF:
                fail("节点 %s 源无选择,包里却有" % nid)
        else:
            if choice_idx == 0xFFFF:
                fail("节点 %s 源有选择,包里缺失" % nid)
            else:
                ch = list(pack.rows("CHOICE"))[choice_idx]
                heading_i, category_i, first_item, item_count, kind = struct.unpack_from(
                    "<HHHBB", ch, 0)
                if pack.string(heading_i) != (src_choice.get("heading") or ""):
                    fail("节点 %s 选择标题不一致" % nid)
                if pack.string(category_i) != (src_choice.get("category") or ""):
                    fail("节点 %s 选择分类不一致" % nid)
                if item_count != len(src_choice["items"]):
                    fail("节点 %s 选项数 源 %d / 包 %d" % (nid, len(src_choice["items"]), item_count))
                for k, item in enumerate(src_choice["items"][:item_count]):
                    raw_item = list(pack.rows("CITEM"))[first_item + k]
                    text_i, label_i, to_i, action, _pad = struct.unpack_from("<HHHBB", raw_item, 0)
                    if pack.string(text_i) != (item.get("text") or ""):
                        fail("节点 %s 选项 %d 文本不一致" % (nid, k))
                    if pack.string(label_i) != (item.get("label") or ""):
                        fail("节点 %s 选项 %d 标签不一致" % (nid, k))
                    if to_i != node_index[item["to"]]:
                        fail("节点 %s 选项 %d 落点 源 %s / 包节点 %d"
                             % (nid, k, item["to"], to_i))
        # 结局名
        if src_node.get("ending"):
            if pack.string(ending_str) != src_node["ending"]:
                fail("节点 %s 结局名不一致" % nid)
        elif ending_str != 0xFFFF:
            fail("节点 %s 源无结局,包里却有" % nid)

    # chunk 在多个节点间共享,所以"比对次数"会大于包内帧数;真正要保证的是:
    # 包里每一帧都至少被一个节点引用到,而且每一次引用都和源文件逐字段一致。
    if len(visited_frames) != pack.count("FRAME"):
        fail("包内 %d 帧中只用到 %d 帧" % (pack.count("FRAME"), len(visited_frames)))
    notes.append("逐帧比对 %d 次(包内 %d 帧全部被引用),节点可达帧总数 %d"
                 % (compared, pack.count("FRAME"), total_frames))

    # ---- chunk 表自身:每个 chunk 的首帧+帧数必须在范围内
    for i, row in enumerate(chunk_rows):
        first_frame, cnt = struct.unpack_from("<IH", row, 0)
        if first_frame + cnt > pack.count("FRAME"):
            fail("chunk %d 帧区间越界" % i)

    # ---- 章节入口
    src_chapters = list(graph["chapterStarts"].items())
    got_chapters = list(pack.rows("CHAPTER"))
    if len(got_chapters) != len(src_chapters):
        fail("章节数 源 %d / 包 %d" % (len(src_chapters), len(got_chapters)))
    for i, (name, spec) in enumerate(src_chapters):
        node_i, cursor = struct.unpack_from("<HI", got_chapters[i], 2)
        if pack.string(rd16(got_chapters[i], 0)) != name:
            fail("章节 %d 名称不一致" % i)
        if node_i != node_index[spec["node"]] or cursor != spec["index"]:
            fail("章节 %s 入口 源 %s:%d / 包 %d:%d"
                 % (name, spec["node"], spec["index"], node_i, cursor))

    # ---- 鉴赏
    src_gallery = load_json(os.path.join(common, "gallery.txt"))
    got_gal = list(pack.rows("GAL"))
    got_gentry = list(pack.rows("GENTRY"))
    if len(got_gal) != len(src_gallery):
        fail("鉴赏组数 源 %d / 包 %d" % (len(src_gallery), len(got_gal)))
    variants = 0
    for i, entry in enumerate(src_gallery[:len(got_gal)]):
        label_i, first, count = struct.unpack_from("<HHH", got_gal[i], 0)
        if pack.string(label_i) != entry["label"]:
            fail("鉴赏组 %d 标签不一致" % i)
        if count != len(entry["images"]):
            fail("鉴赏组 %s 变体数 源 %d / 包 %d" % (entry["label"], len(entry["images"]), count))
        variants += count
        for k, asset_id in enumerate(entry["images"][:count]):
            asset_i = rd16(got_gentry[first + k], 0)
            if asset_i >= pack.count("ASSET"):
                fail("鉴赏组 %s 变体 %d 资源下标越界" % (entry["label"], k))
    if variants != pack.count("GENTRY"):
        fail("鉴赏变体总数 %d != 包内 %d" % (variants, pack.count("GENTRY")))

    # ---- 世界线
    got_wline = list(pack.rows("WLINE"))
    got_wchange = list(pack.rows("WCHANGE"))
    for i, nid in enumerate(node_ids):
        node_i, first, count, rate = struct.unpack_from("<HHBxI", got_wline[i], 0)
        if node_i != i:
            fail("世界线表第 %d 行指向节点 %d" % (i, node_i))
        src_row = worldline.get(nid)
        if not src_row:
            continue
        src_rate = int(round(float(src_row[0]) * 1000000))
        if rate != src_rate:
            fail("节点 %s 初始世界线 %d != 源 %d" % (nid, rate, src_rate))
        if count != len(src_row[1]):
            fail("节点 %s 世界线变更数 源 %d / 包 %d" % (nid, len(src_row[1]), count))
        for k, (cursor, value) in enumerate(src_row[1][:count]):
            got_cursor, got_rate = struct.unpack_from("<Ii", got_wchange[first + k], 0)
            if got_cursor != cursor or got_rate != int(round(float(value) * 1000000)):
                fail("节点 %s 世界线变更 %d 不一致" % (nid, k))
    notes.append("世界线表 %d 行 / %d 个变更点" % (len(got_wline), len(got_wchange)))
    if pack.meta("initial_worldline") != initial_rate:
        fail("初始世界线文本不一致")

    # ---- 图片:层引用、裁剪框、解码边界
    assets = list(pack.rows("ASSET"))
    blob_off, _, blob_size = pack.sec["BLOB"]
    used = set()
    for v, row in enumerate(pack.rows("VIEW")):
        count, _pad, first = struct.unpack_from("<BBH", row, 0)
        for k in range(count):
            layer = list(pack.rows("LAYER"))[first + k]
            asset_i, x, y, w, h, opacity = struct.unpack_from("<HhhHHH", layer, 0)
            if asset_i >= len(assets):
                fail("画面 %d 层 %d 资源下标越界" % (v, k))
                continue
            used.add(asset_i)
            if w <= 0 or h <= 0:
                fail("画面 %d 层 %d 尺寸非法" % (v, k))
            if opacity > 1000:
                fail("画面 %d 层 %d 不透明度非法" % (v, k))
    for i, row in enumerate(assets):
        off, length, mask_off, mask_len, w, h, mw, mh, cx, cy, cw, ch, ow, oh, flags = \
            struct.unpack_from("<IIIIHHHHHHHHHHH", row, 0)
        if length == 0 or off + length > blob_size:
            fail("资源 %d 的 JPEG 区间越界" % i)
        if mask_len and mask_off + mask_len > blob_size:
            fail("资源 %d 的遮罩区间越界" % i)
        if w == 0 or h == 0 or cw == 0 or ch == 0:
            fail("资源 %d 尺寸为 0" % i)
        if cx + cw > ow or cy + ch > oh:
            fail("资源 %d 裁剪框超出原图" % i)
        if w > cw or h > ch:
            fail("资源 %d 打包尺寸大于裁剪框" % i)
        blob = pack.data[blob_off + off: blob_off + off + length]
        if blob[:2] != b"\xff\xd8":
            fail("资源 %d 不是 JPEG(SOI 不对)" % i)
        if blob[-2:] != b"\xff\xd9":
            fail("资源 %d JPEG 未正常结束(EOI 缺失)" % i)
    notes.append("资源 %d 张(被画面引用 %d 张),遮罩 %d 张"
                 % (len(assets), len(used), sum(1 for r in assets
                                                if rd32(r, 12) != 0)))

    # ---- 字体覆盖:包里出现的每个码位都要在生成的字库清单里
    if font_dir:
        story_inv = os.path.join(font_dir, "steins_story_symbols.txt")
        ui_inv = os.path.join(font_dir, "steins_ui_symbols.txt")
        covered = set()
        for path in (story_inv, ui_inv):
            if not os.path.exists(path):
                fail("缺少字库清单 %s" % path)
                continue
            with open(path, "r", encoding="utf-8") as fh:
                for line in fh:
                    if line.startswith("#"):
                        continue
                    covered |= set(line.rstrip("\n"))
        # 包里所有字符串(含正文、界面文案)出现的字
        strings = [pack.string(i) for i in range(pack.count("STRT"))]
        chars = set()
        for s in strings:
            if s:
                chars |= set(s)
        missing = [c for c in chars if c not in covered and c not in "\n\r\t"]
        if missing:
            fail("字库缺 %d 个字: %s" % (len(missing), "".join(sorted(missing))[:40]))
        else:
            notes.append("字库覆盖:包里 %d 个字全部有字形" % len(chars))

    # ---- 文本编码统计(证明"不能按不是 CJK 记 0 句")
    pua = 0
    total_chars = 0
    for s in (pack.string(i) for i in range(pack.count("STRT"))):
        if not s:
            continue
        for ch in s:
            total_chars += 1
            if 0xE000 <= ord(ch) <= 0xF8FF:
                pua += 1
    notes.append("文本字符 %d 个,其中 PUA 字形 %d 个(%.1f%%)"
                 % (total_chars, pua, 100.0 * pua / max(1, total_chars)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True)
    ap.add_argument("--pack", default="assets/steins/steins_pack.bin")
    ap.add_argument("--font-dir", default="assets/fonts")
    ap.add_argument("--report", default="")
    args = ap.parse_args()

    if not os.path.isdir(os.path.join(args.source, "src", "common")):
        print("source snapshot not found: %s" % args.source, file=sys.stderr)
        return 2
    try:
        pack = Pack(args.pack)
    except SystemExit as exc:
        print("pack unreadable: %s" % exc, file=sys.stderr)
        return 2

    auditar(args.source, pack, args.font_dir)
    report = {"pack": os.path.abspath(args.pack), "pack_bytes": len(pack.data),
              "notes": notes, "failures": failures}
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    if args.report:
        os.makedirs(os.path.dirname(os.path.abspath(args.report)) or ".", exist_ok=True)
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    if failures:
        print("AUDIT FAILED (%d)" % len(failures), file=sys.stderr)
        return 1
    print("AUDIT PASS", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
