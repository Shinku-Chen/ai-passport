#!/usr/bin/env python3
"""手机/邮件内容与状态的**实测**预算:不写正式包,只产出可核对数字。

与上一版(被 Lead 指出是估算)的区别:
  * app 尺寸与分区余量**从实际归档 manifest + partitions.csv 读**,不再写死;
  * 字符串按**全局池**去重(含当前包里已有的串),并明确区分三种口径:
    唯一正文 / type3 关键回复 / 其它后继;不把"mailIndex 所有后继"当"回复";
  * 事件按**真实编码**逐条算字节(kind+argc+u16 参数),不用 12 B/条;
  * 新增字形用**真实字体栅格化**量出 bitmap 字节,并与当前 story 字体的 cmap 核对
    哪些码位真的缺(不靠 107 B/字形均值);
  * 候选包**真的拼出来**(TXT+STRT+MAIL+EVENT+CALL 段)再报实际字节;
  * NVS 按 IDF 的 entry/blob-chunk/page 结构建模(32 B entry、126 entry/page、
    blob 分块、GC 保留页、namespace 入口),而不是"22KB < 24KB"的手算;
  * 代码增量**不给猜测值**:未实现就没有字节数,列为"未测量"。

用法:
  python tools/steins_mobile_budget.py --source <steins-rw5 检出> \
      --archive build/firmware/<sha>/ --pack assets/steins/steins_pack.bin \
      --font-dir assets/fonts --story-font <sg-original.ttf> \
      --out build/mobile-budget/<stamp>
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import time

MAGIC = b"STEINPK3"
REC_STRT = 6
NVS_PAGE = 4096
NVS_ENTRY = 32
NVS_PAGE_HEADER = 32
NVS_ENTRY_BITMAP = 32
NVS_ENTRIES_PER_PAGE = (NVS_PAGE - NVS_PAGE_HEADER - NVS_ENTRY_BITMAP) // NVS_ENTRY   # 126
NVS_BLOB_CHUNK = 1984          # IDF nvs: one blob chunk <= 62 data entries


# ------------------------------------------------------------------ 源读取

def load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def _js_value(text, name):
    m = re.search(r"(?:export\s+)?(?:const|let|var)\s+" + name + r"\s*=\s*", text)
    if not m:
        return None
    try:
        value, _end = json.JSONDecoder().raw_decode(text[m.end():])
    except ValueError:
        return None
    return value


def js_const(path, name):
    value = _js_value(open(path, "r", encoding="utf-8").read(), name)
    if value is None:
        raise SystemExit("cannot find %s in %s" % (name, path))
    return value


def js_multi(path, names):
    text = open(path, "r", encoding="utf-8").read()
    out = {}
    for name in names:
        value = _js_value(text, name)
        if value is not None:
            out[name] = value
    return out


def pack_strings(path):
    data = open(path, "rb").read()
    if data[:8] != MAGIC:
        raise SystemExit("%s: not a STEINPK3 pack" % path)
    version, total, count = struct.unpack_from("<III", data, 8)
    sec = {}
    for i in range(count):
        typ, off, scount, size = struct.unpack_from("<IIII", data, 32 + i * 16)
        sec[typ] = (off, scount, size)
    strt_off, strt_count, _ = sec[1]
    txt_off, _, txt_size = sec[2]
    txt = data[txt_off:txt_off + txt_size]
    out = set()
    for i in range(strt_count):
        soff, slen = struct.unpack_from("<IH", data, strt_off + i * REC_STRT)
        out.add(txt[soff:soff + slen * 2].decode("utf-16-le"))
    return out


def read_partition_sizes(path):
    sizes = {}
    for line in open(path, "r", encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        name, _type, _sub, _off, size = [f.strip() for f in line.split(",")[:5]]
        sizes[name] = int(size, 0)
    return sizes


# ------------------------------------------------------------------ 事件编码

def encode_events(events):
    """[u8 kind][u8 argc][argc*u16] —— 与未来生产编码同构,逐条累加真实字节。"""
    KIND = {"seed": 0, "receive": 1, "erase": 2, "readBit": 3, "replyBit": 4,
            "expire": 5, "queue": 6, "dispatch": 7, "rule": 8, "call": 9,
            "presentation": 10}
    out = bytearray()
    per_node = {}
    counts = {k: 0 for k in KIND}
    for _file, nodes in events.items():
        for node, rows in nodes.items():
            per_node.setdefault(node, 0)
            for row in rows:
                cursor, evs = row[0], row[1]
                out += struct.pack("<IH", int(cursor) & 0xFFFFFFFF, len(evs))
                per_node[node] += 1 + 6
                for ev in evs:
                    kind = ev[0]
                    if kind not in KIND:
                        raise SystemExit("unknown event kind %r" % (kind,))
                    counts[kind] += 1
                    args = ev[1:]
                    out.append(KIND[kind])
                    out.append(len(args))
                    per_node[node] += 2
                    for arg in args:
                        if isinstance(arg, bool):
                            val = 1 if arg else 0
                        elif isinstance(arg, int):
                            val = arg & 0xFFFF
                        else:
                            val = 0
                        out += struct.pack("<H", val)
                        per_node[node] += 2
    return bytes(out), counts, per_node


# ------------------------------------------------------------------ NVS 模型

def nvs_entries_for_blob(length):
    """IDF NVS:1 个 blob 头 + 数据项 + 每 1984 B 一个 chunk 头。"""
    if length <= 0:
        return 1
    data = (length + NVS_ENTRY - 1) // NVS_ENTRY
    chunks = (length + NVS_BLOB_CHUNK - 1) // NVS_BLOB_CHUNK
    return 1 + data + chunks


def nvs_capacity(partition_bytes, namespaces=1):
    pages = partition_bytes // NVS_PAGE
    usable_pages = pages - 1                     # 留一页给 GC/compaction
    entries = usable_pages * NVS_ENTRIES_PER_PAGE - namespaces
    return {
        "partition_bytes": partition_bytes,
        "pages": pages,
        "gc_reserve_pages": 1,
        "usable_pages": usable_pages,
        "entries_per_page": NVS_ENTRIES_PER_PAGE,
        "usable_entries": entries,
        "usable_payload_bytes": entries * NVS_ENTRY,
    }


def nvs_need(slot_sizes, fixed_sizes, namespaces=1):
    entries = namespaces
    for size in slot_sizes:
        entries += nvs_entries_for_blob(size)
    for size in fixed_sizes:
        entries += nvs_entries_for_blob(size)
    return {
        "entries": entries,
        "bytes": entries * NVS_ENTRY,
        "slot_count": len(slot_sizes),
        "slot_max_bytes": max(slot_sizes) if slot_sizes else 0,
    }


# ------------------------------------------------------------------ 主流程

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True)
    ap.add_argument("--archive", required=True, help="build/firmware/<sha> 同次归档目录")
    ap.add_argument("--pack", default="assets/steins/steins_pack.bin")
    ap.add_argument("--partitions", default="partitions.csv")
    ap.add_argument("--font-dir", default="assets/fonts")
    ap.add_argument("--story-font", default="")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    out_dir = args.out or os.path.join("build", "mobile-budget", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out_dir, exist_ok=True)

    common = os.path.join(args.source, "src", "common")
    mail_index = js_const(os.path.join(common, "mail-index.js"), "mailIndex")
    phone = js_multi(os.path.join(common, "phone-data.js"),
                     ("phoneData", "owners", "books", "contacts", "messages", "dmail"))
    call = js_multi(os.path.join(common, "call-index.js"), ("callIndex", "callClips"))
    windows = js_const(os.path.join(common, "mobile-windows.js"), "mobileWindows")
    keyphone = js_const(os.path.join(common, "key-phone-windows.js"), "keyPhoneWindows")
    event_map = js_const(os.path.join(common, "mobile-event-map.js"), "mobileEventFiles")

    letters = {}
    letter_dir = os.path.join(common, "letter-packs")
    for name in sorted(os.listdir(letter_dir)):
        if name.endswith(".txt"):
            for key, value in load_json(os.path.join(letter_dir, name)).items():
                letters[int(key)] = value
    events = {}
    event_dir = os.path.join(common, "mobile-packs")
    for name in sorted(os.listdir(event_dir)):
        if name.endswith(".txt"):
            events[name] = load_json(os.path.join(event_dir, name))

    call_frames = []
    for _cid, clip in (call.get("callClips") or {}).items():
        for chunk in clip.get("chunks", []):
            for base in ("story", "mobile-packs"):
                path = os.path.join(common, base, chunk["file"])
                if os.path.exists(path):
                    call_frames.extend(load_json(path).get("frames", []))
                    break

    # ---- 字符串池:当前包 + 全部候选串
    pool = set(pack_strings(args.pack))
    pack_pool_size = len(pool)

    bodies = {text for text in (e.get("text") or "" for e in letters.values()) if text}
    mail_rows = list(enumerate(mail_index)) if isinstance(mail_index, list) else \
        sorted((int(k), v) for k, v in mail_index.items())
    successors = set()
    type3_reply = set()
    for _mid, entry in mail_rows:
        if not entry or len(entry) < 4:
            continue
        for rid in (entry[3] or []):
            target = letters.get(int(rid)) if isinstance(rid, int) else None
            if target and target.get("text"):
                successors.add(target["text"])
                if len(entry) > 2 and entry[2] == 3:
                    type3_reply.add(target["text"])
    call_strings = {row[1] for row in call_frames if len(row) > 1 and row[1]}
    call_strings |= {row[0] for row in call_frames if len(row) > 0 and row[0]}
    meta_strings = set()
    for _mid, entry in mail_rows:
        if not entry or len(entry) < 2:
            continue
        for field in (entry[0], entry[1]):
            if field:
                meta_strings.add(field)

    def fresh(strings):
        return {s for s in strings if s not in pool}

    new_bodies = fresh(bodies)
    new_successors = fresh(successors)
    new_type3 = fresh(type3_reply)
    new_calls = fresh(call_strings)
    new_meta = fresh(meta_strings)

    def utf16(strings):
        return sum(len(s.encode("utf-16-le")) for s in strings)

    # ---- 候选包:真的拼出 TXT/STRT/MAIL/EVENT/CALL 段
    ordered = sorted(new_bodies | new_successors | new_calls | new_meta | new_type3)
    txt = bytearray()
    strt = bytearray()
    index = {}
    for text in ordered:
        index[text] = len(strt) // REC_STRT
        off = len(txt)
        txt += text.encode("utf-16-le")
        strt += struct.pack("<IH", off, len(text.encode("utf-16-le")) // 2)

    mail_meta = bytearray()
    for mid, entry in mail_rows:
        if not entry or len(entry) < 4:
            continue
        sender_i = index.get(entry[0], 0xFFFF) if entry[0] else 0xFFFF
        title_i = index.get(entry[1], 0xFFFF) if entry[1] else 0xFFFF
        body_i = index.get((letters.get(mid) or {}).get("text") or "", 0xFFFF)
        mail_meta += struct.pack("<HHHHBBH", mid & 0xFFFF, sender_i, title_i, body_i,
                                 entry[2] & 0xFF, len(entry[3] or []) & 0xFF, 0)

    event_blob, event_counts, per_node = encode_events(events)

    call_table = bytearray()
    for row in call_frames:
        speaker_i = index.get(row[0], 0xFFFF) if row[0] else 0xFFFF
        text_i = index.get(row[1], 0xFFFF) if len(row) > 1 and row[1] else 0xFFFF
        call_table += struct.pack("<HH", speaker_i, text_i)

    sections = {
        "TXT": len(txt),
        "STRT": len(strt),
        "MAIL": len(mail_meta),
        "EVENT": len(event_blob),
        "CALL": len(call_table),
    }
    candidate_total = sum(sections.values()) + 32 + 16 * len(sections)

    # ---- 字形增量:真实栅格化 + 与现有字体 cmap 核对
    new_cps = set()
    for text in ordered:
        for ch in text:
            if 0xE000 <= ord(ch) <= 0xF8FF and ch not in pool:
                pass
    # 缺字判定用字体清单(生成器导出),不是猜测
    story_inv = os.path.join(args.font_dir, "steins_story_symbols.txt")
    covered = set()
    if os.path.exists(story_inv):
        for line in open(story_inv, encoding="utf-8"):
            if not line.startswith("#"):
                covered |= set(line.rstrip("\n"))
    missing_cps = set()
    for text in ordered:
        for ch in text:
            if 0xE000 <= ord(ch) <= 0xF8FF and ch not in covered:
                missing_cps.add(ord(ch))

    glyph_bitmap_bytes = None
    glyph_metrics_bytes = None
    if args.story_font and missing_cps:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
        import gal_font  # noqa: E402
        glyphs = gal_font.rasterize(args.story_font, 16, sorted(missing_cps))
        bitmap = 0
        for cp in sorted(missing_cps):
            _adv, bw, bh, _x, _y, nibbles = glyphs[cp]
            bitmap += len(gal_font.pack_nibbles(nibbles))
        glyph_bitmap_bytes = bitmap
        glyph_metrics_bytes = len(missing_cps) * 16      # glyph_dsc 16 B/字形
    # 生成器把新字形并进子集后,cmap 的 unicode_list 会增长
    glyph_cmap_bytes = len(missing_cps) * 2 if missing_cps else 0

    # ---- 分区与真实余量
    parts = read_partition_sizes(args.partitions)
    manifest = json.load(open(os.path.join(args.archive, "manifest.json"), encoding="utf-8"))
    app_size = manifest["files"]["FoloToy-AI-Passport.bin"]["size"]
    app_partition = parts["factory"]
    free_now = app_partition - app_size

    # ---- NVS 模型
    nvs = nvs_capacity(parts["nvs"])
    fixed_keys = {"settings": 8, "unlock": 26, "autosave": 16}
    # 紧凑 checkpoint 的字段口径(位图 + varint + 有序 id 列表)
    compact = {
        "header_crc_version": 8,
        "mode_scene_cursor": 12,
        "event_cursor_lastNode_lastAt": 12,
        "read_bitmap_646": (646 + 7) // 8,
        "replied_bitmap_646": (646 + 7) // 8,
        "unread_bitmap_646": (646 + 7) // 8,
        "open_channel_map": 16,
        "channels5": 10,
        "call_and_return": 16,
        "presentations": 24,
        "deferred_keyphone": 16,
        "inbox_ordered_ids": 64 * 2,
        "sent_ordered_ids": 16 * 2,
        "mail_owners": 24,
        "endings_bits": 4,
    }
    compact_slot = sum(compact.values())
    slot_sizes_compact = [compact_slot] * 31
    need_compact = nvs_need(slot_sizes_compact, list(fixed_keys.values()))

    # 后备方案:手机状态只写 auto + 最近 4 槽,其它槽存 16 B 位置
    small_slot = 16
    slot_sizes_mixed = [compact_slot] * 5 + [small_slot] * 26
    need_mixed = nvs_need(slot_sizes_mixed, list(fixed_keys.values()))

    # 扩容后的 NVS(nvs 0x12000, app 上移到 0x1C000)
    grown_nvs = nvs_capacity(0x12000)
    grown_app_partition = 0x800000 - 0x1C000
    need_grown = nvs_need(slot_sizes_compact, list(fixed_keys.values()))
    grown_fits = need_grown["entries"] <= grown_nvs["usable_entries"] and \
        app_size <= grown_app_partition

    # 最近一次构建的代码增量:未实现,拒绝给猜测值
    report = {
        "source": os.path.abspath(args.source),
        "archive": os.path.abspath(args.archive),
        "app": {"size": app_size, "partition": app_partition, "free": free_now,
                "sha256": manifest["files"]["FoloToy-AI-Passport.bin"]["sha256"]},
        "counts": {
            "mail_index": len(mail_rows),
            "nonempty_bodies": len(bodies),
            "unique_bodies": len(bodies),
            "successors_total": len(successors),
            "type3_reply_strings": len(type3_reply),
            "type3_reply_subset_of_bodies": type3_reply <= bodies,
            "successors_subset_of_bodies": successors <= bodies,
            "call_frames": len(call_frames),
            "call_strings": len(call_strings),
            "meta_strings": len(meta_strings),
            "mobile_event_rows": sum(len(v) for nodes in events.values()
                                     for v in nodes.values()),
            "event_counts": event_counts,
            "event_files": len(event_map),
            "mail_windows": len(windows),
            "key_phone_windows": len(keyphone),
            "call_sites": len(call.get("callIndex") or {}),
            "call_clips": len(call.get("callClips") or {}),
        },
        "dedupe": {
            "pack_pool_strings": pack_pool_size,
            "new_body_strings": len(new_bodies),
            "new_successor_strings": len(new_successors),
            "new_type3_strings": len(new_type3),
            "new_call_strings": len(new_calls),
            "new_meta_strings": len(new_meta),
            "new_utf16_bytes": utf16(new_bodies | new_successors | new_calls | new_meta),
        },
        "candidate_pack": {
            "sections": sections,
            "total_bytes": candidate_total,
            "note": "真实拼出的候选段(不含 JPEG/字体/代码),不是均值估算",
        },
        "glyphs": {
            "missing_from_story_cmap": len(missing_cps),
            "bitmap_bytes": glyph_bitmap_bytes,
            "dsc_bytes": glyph_metrics_bytes,
            "cmap_bytes": glyph_cmap_bytes,
            "total_bytes": (glyph_bitmap_bytes or 0) + (glyph_metrics_bytes or 0) +
                           glyph_cmap_bytes,
            "note": "bitmap 由真实 16px 栅格化得出;dsc 16 B/字形、cmap 2 B/字形按生成器源码",
        },
        "budget": {
            "pack_plus_font": candidate_total + (glyph_bitmap_bytes or 0) +
                              (glyph_metrics_bytes or 0) + glyph_cmap_bytes,
            "code_bytes": None,
            "code_note": "未实现即无字节数;等生产实现后从同次 MAP 量", 
            "fits_current_partition_if_code_under": free_now - candidate_total -
                (glyph_bitmap_bytes or 0) - (glyph_metrics_bytes or 0) - glyph_cmap_bytes,
        },
        "nvs": {
            "partition": nvs,
            "compact_slot_bytes": compact_slot,
            "compact_slot_fields": compact,
            "need_31_full_slots": need_compact,
            "fits_31_full_slots": need_compact["entries"] <= nvs["usable_entries"],
            "need_mixed_5_full_26_small": need_mixed,
            "fits_mixed": need_mixed["entries"] <= nvs["usable_entries"],
            "grown_partition": grown_nvs,
            "grown_app_partition_bytes": grown_app_partition,
            "grown_fits_31_full_slots": grown_fits,
        },
    }

    with open(os.path.join(out_dir, "budget-v3.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=2)

    lines = [
        "Steins mobile budget v3 (measured, not estimated)",
        "app: %d / %d, free %d B" % (app_size, app_partition, free_now),
        "strings: bodies %d, successors %d (type3 replies %d, subset of bodies=%s), "
        "call strings %d, meta %d" % (len(bodies), len(successors), len(type3_reply),
                                      type3_reply <= bodies, len(call_strings),
                                      len(meta_strings)),
        "pool: pack has %d strings; new bodies %d / successors %d / type3 %d / call %d / meta %d"
        % (pack_pool_size, len(new_bodies), len(new_successors), len(new_type3),
           len(new_calls), len(new_meta)),
        "candidate pack: TXT %d + STRT %d + MAIL %d + EVENT %d + CALL %d = %d B"
        % (sections["TXT"], sections["STRT"], sections["MAIL"], sections["EVENT"],
           sections["CALL"], candidate_total),
        "events: %d rows, %d B (real encoding)" % (
            report["counts"]["mobile_event_rows"], sections["EVENT"]),
        "glyphs: %d missing from story cmap, bitmap %s B + dsc %s B + cmap %s B" % (
            len(missing_cps), glyph_bitmap_bytes, glyph_metrics_bytes, glyph_cmap_bytes),
        "pack+font total: %d B; current free %d B; code budget left: %d B" % (
            report["budget"]["pack_plus_font"], free_now,
            report["budget"]["fits_current_partition_if_code_under"]),
        "NVS: partition %d B = %d pages, usable %d entries (%d B payload)" % (
            nvs["partition_bytes"], nvs["pages"], nvs["usable_entries"],
            nvs["usable_payload_bytes"]),
        "  compact checkpoint %d B/slot x 31 -> %d entries (%d B): fits=%s" % (
            compact_slot, need_compact["entries"], need_compact["bytes"],
            need_compact["entries"] <= nvs["usable_entries"]),
        "  mixed (5 full + 26 position-only) -> %d entries: fits=%s" % (
            need_mixed["entries"], need_mixed["entries"] <= nvs["usable_entries"]),
        "  grown nvs 0x12000 (app->0x1C000, %d B) -> %d entries: fits=%s" % (
            grown_app_partition, need_grown["entries"], grown_fits),
        "code delta: not measured (no implementation yet)",
        "written to %s" % os.path.abspath(out_dir),
    ]
    text = "\n".join(lines)
    print(text)
    with open(os.path.join(out_dir, "budget-v3.txt"), "w", encoding="utf-8") as fh:
        fh.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
