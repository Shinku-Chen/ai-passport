#!/usr/bin/env python3
"""把手机/邮件数据以 **typed** 形式追加进资源包(v4),输出候选包。

设计要点(见 build/mobile-integration/pack-api.md):
  * 不重写主剧情段:以现有 v3 包为底,原样保留 20 个段,只**追加**新段;
  * 字符串继续进全局池(STRT/TXT 追加),已存在的串复用下标,不重复存 333 条后继文案;
  * 事件参数按源真实类型编码,不做 "非整数就填 0":
      seed          arg0 = SEED 描述符下标(描述符指向 SEED_ID 有序 ID 流)
      call          arg0 = CALLDESC 下标(站点字符串/owner/kind 字符串/有序 items/waiting)
      presentation  arg1 = 显式 enum(draft=0 send=1 inbox=2 read=3 other=4)
      其余 kind    原数值/bool
  * 段表放在文件头;版本升 4。

用法:
  python tools/steins_mobile_pack.py --source <steins-rw5> \
      --base-pack assets/steins/steins_pack.bin \
      --out build/mobile-pack-<UTC>-<pid>/steins_pack.bin
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import time

MAGIC = b"STEINPK3"
VERSION_V4 = 4

SEC_TYPES = {
    "STRT": 1, "TXT": 2, "NODE": 3, "CHUNKREF": 4, "CHUNK": 5, "FRAME": 6,
    "VIEW": 7, "LAYER": 8, "ASSET": 9, "BLOB": 10, "CHOICE": 11, "CITEM": 12,
    "CHAPTER": 13, "GAL": 14, "GENTRY": 15, "WLINE": 16, "WCHANGE": 17,
    "BREAK": 18, "UI": 19, "META": 20,
    "MAIL": 21, "MAIL_SUCC": 22, "MAIL_STAMP": 23, "MROW": 24, "MEVT": 25,
    "MNODE": 26, "CALLSITE": 27, "CALLITEM": 28, "CLIP": 29, "CFRAME": 30,
    "WINDOW": 31, "WINOPT": 32, "KEYPHONE": 33, "MOBILE": 34, "MMAP": 35,
    "SEED": 36, "SEED_ID": 37, "CALLDESC": 38, "CALLDITEM": 39,
    "CHOICE_META": 43,
    "CITEM_META": 44,
    "PRES": 42,
}
NUM_TYPES = {v: k for k, v in SEC_TYPES.items()}

PRES_KIND = {"draft": 0, "send": 1, "view": 2, "inbox": 3, "read": 4}
EV_KIND = {"seed": 0, "receive": 1, "erase": 2, "readBit": 3, "replyBit": 4,
           "expire": 5, "queue": 6, "dispatch": 7, "rule": 8, "call": 9,
           "presentation": 10}
REC_STRT = 6


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


def chapter_starts(root):
    """graph.txt 的 nodes.<n>.breaks = [[首行, "NN 章 名称"], ...] → 每节点章节表。"""
    import re
    graph = load_json(os.path.join(root, "src", "common", "graph.txt"))
    table = {}
    for nid, node in (graph.get("nodes") or {}).items():
        starts = []
        for entry in node.get("breaks") or []:
            if not entry:
                continue
            row = int(entry[0])
            label = str(entry[1] if len(entry) > 1 else "")
            m = re.match(r"\s*(\d+)", label)
            # 前缀无编号的 break 是“章内小节”,沿用上一章;第一章前默认 0。
            starts.append((row, int(m.group(1)) if m else None))
        fixed = []
        current = 0
        for row, num in sorted(starts):
            if num is not None:
                current = num
            fixed.append((row, current))
        table[nid] = fixed
    return table


def load_source(root):
    common = os.path.join(root, "src", "common")
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
    call_clips = js_const(os.path.join(common, "call-index.js"), "callClips")
    for _cid, clip in (call_clips or {}).items():
        for chunk in clip.get("chunks", []):
            for base in ("story", "mobile-packs"):
                path = os.path.join(common, base, chunk["file"])
                if os.path.exists(path):
                    call_frames.extend(load_json(path).get("frames", []))
                    break
    return {
        "mail_index": js_const(os.path.join(common, "mail-index.js"), "mailIndex"),
        "letters": letters,
        "events": events,
        "event_files": js_const(os.path.join(common, "mobile-event-map.js"), "mobileEventFiles"),
        "windows": js_const(os.path.join(common, "mobile-windows.js"), "mobileWindows"),
        "keyphone": js_const(os.path.join(common, "key-phone-windows.js"), "keyPhoneWindows"),
        "call_index": js_const(os.path.join(common, "call-index.js"), "callIndex"),
        "call_clips": call_clips,
        "call_frames": call_frames,
    }


# ------------------------------------------------------------------ 底包读写

class BasePack:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        if self.data[:8] != MAGIC:
            raise SystemExit("%s: not a STEINPK3 pack" % path)
        self.version, self.total, sections = struct.unpack_from("<III", self.data, 8)
        self.sections = []          # (type, bytes, count)
        for i in range(sections):
            typ, off, count, size = struct.unpack_from("<IIII", self.data, 32 + i * 16)
            self.sections.append((typ, self.data[off:off + size], count))
        by_type = {t: d for t, d, _c in self.sections}
        self.strt = by_type[SEC_TYPES["STRT"]]
        self.txt = by_type[SEC_TYPES["TXT"]]
        self.strings = []
        for i in range(len(self.strt) // REC_STRT):
            off, length = struct.unpack_from("<IH", self.strt, i * REC_STRT)
            self.strings.append(self.txt[off:off + length * 2].decode("utf-16-le"))

    def index_of(self, text):
        try:
            return self.strings.index(text)
        except ValueError:
            return None


class Strings:
    """在底包字符串池上追加:已存在的串复用下标。"""

    def __init__(self, base):
        self.base = base
        self.extra = []
        self.lookup = {}

    def add(self, text):
        if not text:
            return 0xFFFF
        idx = self.base.index_of(text)
        if idx is not None:
            return idx
        got = self.lookup.get(text)
        if got is not None:
            return got
        idx = len(self.base.strings) + len(self.extra)
        if idx > 0xFFFE:
            raise SystemExit("string pool exhausted")
        self.extra.append(text)
        self.lookup[text] = idx
        return idx

    def build(self):
        strt = bytearray(self.base.strt)
        txt = bytearray(self.base.txt)
        for text in self.extra:
            off = len(txt)
            raw = text.encode("utf-16-le")
            txt += raw
            strt += struct.pack("<IH", off, len(raw) // 2)
        return bytes(strt), bytes(txt)


# ------------------------------------------------------------------ 事件编码

def build_events(src, pool, node_ids):
    """按源真实参数类型编码。[u8 kind][u8 argc][u16 arg...]"""
    seed_tab = bytearray()
    seed_ids = bytearray()
    seed_seen = {}
    calldesc = bytearray()
    callitem = bytearray()
    calldesc_seen = {}
    mevt = bytearray()
    mrow = bytearray()
    mnode = bytearray([0xFF, 0, 0, 0, 0, 0] * len(node_ids))
    node_first_row = {}
    node_row_count = {}
    counts = {k: 0 for k in EV_KIND}
    rows = 0
    bad = []
    pres_rows = []          # (id, kind_enum, owner, row, chapter)
    chapter_tbl = src.get("chapter_table") or {}
    node_chapter = {}

    def node_identity(node):
        text = str(node).lstrip("nN")
        return int(text) if text.isdigit() else -1

    def chapter_at(node, row):
        starts = chapter_tbl.get(node)
        if not starts:
            return -1
        chap = -1
        for start_row, num in starts:
            if row >= start_row:
                chap = num
            else:
                break
        return chap

    def add_seed(ids):
        nonlocal seed_tab, seed_ids
        key = tuple(ids)
        got = seed_seen.get(key)
        if got is not None:
            return got
        idx = len(seed_tab) // 4
        first = len(seed_ids) // 2
        for i in ids:
            seed_ids += struct.pack("<H", int(i) & 0xFFFF)
        seed_tab += struct.pack("<HH", first, len(ids))
        seed_seen[key] = idx
        return idx

    def add_call(args):
        nonlocal calldesc, callitem
        site, owner, kind, items, waiting = args
        site_i = pool.add(str(site))
        kind_i = pool.add(str(kind))
        item_first = len(callitem) // 6
        for item in items:
            callitem += struct.pack("<HHH", int(item.get("id", 0)) & 0xFFFF,
                                    pool.add(item.get("action") or ""),
                                    pool.add(item.get("text") or ""))
        key = (site_i, owner, kind_i, tuple((int(i.get("id", 0)),
                                             i.get("action") or "") for i in items),
               bool(waiting))
        got = calldesc_seen.get(key)
        if got is not None:
            return got
        idx = len(calldesc) // 8
        # 8 B 定长:site(2) kind(2) first_item(2) count(1) flags(1: bit0=waiting, bits1-7=owner)
        flags = (1 if waiting else 0) | ((int(owner) & 0x7F) << 1)
        calldesc += struct.pack("<HHHBB", site_i, kind_i, item_first,
                                len(items) & 0xFF, flags)
        calldesc_seen[key] = idx
        return idx

    for file_name, per_node in src["events"].items():
        file_no = int(file_name.split(".")[0]) & 0xFF
        for node, node_rows in per_node.items():
            node_first_row[node] = rows
            node_row_count[node] = len(node_rows)
            if node in node_ids:
                mnode[node_ids.index(node) * 6] = file_no
            for row in node_rows:
                cursor = int(row[0]) & 0xFFFFFFFF
                evs = row[1]
                base_off = len(mevt)
                for ev in evs:
                    kind = ev[0]
                    if kind not in EV_KIND:
                        bad.append("unknown kind %r" % (kind,))
                        continue
                    counts[kind] += 1
                    args = list(ev[1:])
                    if kind == "seed":
                        args = [add_seed(args[0] if args else [])]
                    elif kind == "call":
                        args = [add_call(args)]
                    elif kind == "presentation":
                        pres_rows.append((int(args[0]) & 0xFFFF, PRES_KIND.get(str(args[1]), 5),
                                          int(args[2]) & 0xFF if len(args) > 2 else 0,
                                          cursor & 0xFFFFFFFF, node_identity(node)))
                        pk = str(args[1]) if len(args) > 1 else ""
                        enum = PRES_KIND.get(pk)
                        if enum is None:
                            bad.append("unknown presentation kind %r (file %s node %s)" %
                                       (pk, file_name, node))
                            enum = 5   # other
                        args = [int(args[0]) & 0xFFFF, enum, int(args[2]) & 0xFF
                                if len(args) > 2 else 0]
                    if len(args) > 5:
                        bad.append("argc %d > 5 for %s" % (len(args), kind))
                    mevt.append(EV_KIND[kind])
                    mevt.append(len(args))
                    for arg in args:
                        if isinstance(arg, bool):
                            val = 1 if arg else 0
                        elif isinstance(arg, int):
                            val = arg & 0xFFFF
                        else:
                            bad.append("non-numeric arg %r in %s" % (arg, kind))
                            val = 0
                        mevt += struct.pack("<H", val)
                mrow += struct.pack("<IIBBH", cursor, base_off, file_no, len(evs), 0)
                rows += 1
    for i, nid in enumerate(node_ids):
        if nid in node_first_row:
            struct.pack_into("<HH", mnode, i * 6 + 2, node_first_row[nid],
                             node_row_count[nid])
    # PRES:按源完整 key(chapter,row,id,kind,owner) 稳定排序去重
    for nid, chap in src.get("node_chapter", {}).items():
        node_chapter[nid] = int(chap)
    for item in pres_rows:
        if item[4] < 0:
            bad.append("presentation missing source chapter (id=%d row=%d)" %
                       (item[0], item[3]))
    uniq = sorted({(c, r, i, k, o) for (i, k, o, r, c) in pres_rows if c >= 0})
    pres = bytearray()
    for chap, row, pid, kind, owner in uniq:
        pres += struct.pack("<HIHBB", chap & 0xFFFF, row & 0xFFFFFFFF,
                            pid & 0xFFFF, kind & 0xFF, owner & 0xFF)
    return {
        "PRES": bytes(pres),
        "MROW": bytes(mrow), "MEVT": bytes(mevt), "MNODE": bytes(mnode),
        "SEED": bytes(seed_tab), "SEED_ID": bytes(seed_ids),
        "CALLDESC": bytes(calldesc), "CALLDITEM": bytes(callitem),
    }, counts, rows, bad


# ------------------------------------------------------------------ 主流程

def build_mail(src, pool):
    mail_index = src["mail_index"]
    rows = list(enumerate(mail_index)) if isinstance(mail_index, list) else \
        sorted((int(k), v) for k, v in mail_index.items())
    if not rows:
        raise SystemExit("mail index is empty")
    count = max(r[0] for r in rows) + 1
    mail_tab = [b""] * count
    succ = bytearray()
    stamp = bytearray()
    type3 = 0
    bodies = 0
    for mid, entry in rows:
        sender = pool.add(entry[0] or "")
        title = pool.add(entry[1] or "")
        body = (src["letters"].get(mid) or {}).get("text") or ""
        body_i = pool.add(body)
        if body:
            bodies += 1
        succ_first = len(succ) // 2
        for rid in (entry[3] or []):
            succ += struct.pack("<H", int(rid) & 0xFFFF)
            target = mail_index[int(rid)] if int(rid) < len(mail_index) else None
            if target and len(target) > 2 and target[2] == 3:
                type3 += 1
        entry_stamp = (src["letters"].get(mid) or {}).get("stamp") or [0, 0]
        stamp += struct.pack("<HH", int(entry_stamp[0]) & 0xFFFF if entry_stamp else 0,
                             int(entry_stamp[1]) & 0xFFFF if len(entry_stamp) > 1 else 0)
        mail_tab[mid] = struct.pack("<HHHHBBH", sender, title, body_i, succ_first,
                                    int(entry[2]) & 0xFF if len(entry) > 2 else 0,
                                    len(entry[3] or []) & 0xFF, 0)
    for mid in range(count):
        if not mail_tab[mid]:
            mail_tab[mid] = struct.pack("<HHHHBBH", 0xFFFF, 0xFFFF, 0xFFFF, 0, 0, 0, 0)
    return {
        "MAIL": b"".join(mail_tab), "MAIL_SUCC": bytes(succ), "MAIL_STAMP": bytes(stamp),
    }, {"mail_count": count, "nonempty_bodies": bodies, "type3_succ": type3}


def build_windows(src, node_index, pool):
    win = bytearray()
    wopt = bytearray()
    keyphone = bytearray()
    for node, spec in src["windows"].items():
        if node not in node_index:
            continue
        first = len(wopt) // 10
        options = spec.get("options") or []
        for opt in options:
            if "id" not in opt or "action" not in opt:
                raise SystemExit("window option missing id/action: %r" % (opt,))
            wopt += struct.pack("<HHHHBB", pool.add(opt.get("text") or ""),
                                int(opt["id"]) & 0xFFFF,
                                int(opt.get("parent", 0)) & 0xFFFF,
                                node_index.get(opt.get("to"), 0) & 0xFFFF,
                                action_code(opt["action"]), 0)
        win += struct.pack("<HHHHHHBB",
                           node_index[node], int(spec.get("id", 0)) & 0xFFFF,
                           int(spec.get("limit", 0)) & 0xFFFF,
                           node_index.get(spec.get("skip"), 0) & 0xFFFF,
                           node_index.get(spec.get("read"), 0) & 0xFFFF,
                           first, len(options) & 0xFF, 0)
    for node, spec in src["keyphone"].items():
        if node not in node_index:
            continue
        keyphone += struct.pack("<HHHHHHHH",
                                node_index[node],
                                node_index.get(spec.get("skip"), 0) & 0xFFFF,
                                node_index.get(spec.get("go"), 0) & 0xFFFF,
                                int(spec.get("limit", 0)) & 0xFFFF,
                                int(spec.get("after", 0)) & 0xFFFF,
                                int(spec.get("sent", 0)) & 0xFFFF,
                                pool.add(str(spec.get("source") or "")), 0)
    return {"WINDOW": bytes(win), "WINOPT": bytes(wopt), "KEYPHONE": bytes(keyphone)}


# 源的 call 选项 action 是**字符串**(如 'call'),不是数字。这里用“排序后去重编号+1”
# 给出稳定非零编码,未知/缺失一律抛错,绝不用 0 掩盖。
# 固定 ABI(main/steins_mobile.h):不得按排序/出现顺序生成。
ACTION_NAMES = ["reply", "send", "call", "compose", "noCall", "noSend",
                "readDeferred", "expireDeferred"]
ACTION_FIXED = {"reply": 1, "send": 2, "call": 3, "compose": 4,
                "noCall": 5, "noSend": 6, "readDeferred": 7, "expireDeferred": 8}


def action_code(value):
    if value is None:
        return 0
    name = str(value)
    got = ACTION_FIXED.get(name)
    if got is None:
        raise SystemExit("unknown action %r (fixed ABI, no silent 0)" % (name,))
    return got


def build_call(src, pool):
    sites = bytearray()
    items = bytearray()
    clip_tab = bytearray()
    frames = bytearray()
    # 源 callClips 的 key 形如 "c0";按名字建立稳定下标,供 CALLITEM.clip 引用。
    clip_index = {str(k): i for i, k in enumerate(sorted((src["call_clips"] or {}).keys()))}
    for key, spec in sorted((src["call_index"] or {}).items()):
        site_s, _, _ = str(key).partition(":")
        first = len(items) // 10
        opts = spec.get("options") or []
        for opt in opts:
            items += struct.pack("<HHHBBBx", pool.add(opt.get("text") or ""),
                                 int(opt.get("id", 0)) & 0xFFFF,
                                 int(opt.get("common", 0)) & 0xFFFF,
                                 action_code(opt.get("action")),
                                 0xFF if opt.get("clip") is None
                                 else clip_index.get(str(opt.get("clip")), 0xFE),
                                 int(opt.get("join", 0)) & 0xFF)
        sites += struct.pack("<HHHBBBBH", int(site_s) & 0xFFFF, first,
                             pool.add(str(key)), int(spec.get("owner", 0)) & 0xFF,
                             0, len(opts) & 0xFF, int(spec.get("limit", 0)) & 0xFF, 0)
    for row in src["call_frames"]:
        frames += struct.pack("<HHH", pool.add(row[0] if row and row[0] else ""),
                              pool.add(row[1] if len(row) > 1 and row[1] else ""), 0)
    # 片段:源 callClips 的 key 是 "c0"/"c1" 这样的名字,按名字排序给连续帧区间
    offset = 0
    clip_tab = bytearray()
    for _cid, clip in sorted((src["call_clips"] or {}).items()):
        total = sum(int(c.get("count", 0)) for c in clip.get("chunks", []))
        clip_tab += struct.pack("<HHH", offset, total, offset)
        offset += total
    return {"CALLSITE": bytes(sites), "CALLITEM": bytes(items), "CLIP": bytes(clip_tab),
            "CFRAME": bytes(frames)}


def _crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True)
    ap.add_argument("--base-pack", default="assets/steins/steins_pack.bin")
    ap.add_argument("--out", default="")
    ap.add_argument("--report", default="")
    ap.add_argument("--plain", action="store_true",
                    help="未压缩可读候选(v4):TXT 保持 UTF-16 raw")
    args = ap.parse_args()

    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    out_dir = os.path.dirname(os.path.abspath(args.out)) if args.out else \
        os.path.join("build", "mobile-pack-%s-%d" % (stamp, os.getpid()))
    os.makedirs(out_dir, exist_ok=True)
    out_path = args.out or os.path.join(out_dir, "steins_pack.bin")

    base = BasePack(args.base_pack)
    src = load_source(args.source)
    pool = Strings(base)

    graph = load_json(os.path.join(args.source, "src", "common", "graph.txt"))
    node_ids = list(graph["nodes"])
    node_index = {nid: i for i, nid in enumerate(node_ids)}

    mail_sec, mail_stats = build_mail(src, pool)
    src["chapter_table"] = chapter_starts(args.source)
    graph = load_json(os.path.join(args.source, "src", "common", "graph.txt"))
    windows_sec = build_windows(src, node_index, pool)
    call_sec = build_call(src, pool)
    events_sec, event_counts, rows, bad = build_events(src, pool, node_ids)

    # 源节点 → 物化包节点映射:默认恒等(现主图已物化 65 个选择节点)
    mmap = bytearray()
    for i, nid in enumerate(node_ids):
        # 第一字段是**源节点下标**,第二字段是**包下标**:源 n0 落在包 73,
        # 因此两者不能互换,更不能写成恒等。
        src_no = int(str(nid).lstrip("nN")) if not isinstance(nid, int) else int(nid)
        mmap += struct.pack("<HHBB", src_no, i, 0, 0)

    mobile_meta = (
        "schema=4\n"
        "mail_count=%d\nnonempty_bodies=%d\ntype3_succ=%d\n"
        "event_rows=%d\nevent_count=%d\ncall_frames=%d\n"
        "seed_descs=%d\ncall_descs=%d\n" % (
            mail_stats["mail_count"], mail_stats["nonempty_bodies"],
            mail_stats["type3_succ"], rows, sum(event_counts.values()),
            len(src["call_frames"]), len(events_sec["SEED"]) // 4,
            len(events_sec["CALLDESC"]) // 8)).encode("utf-8")

    # CHOICE/CITEM side 段:必须在 pool.build() 之前把 source 串入池,否则索引悬空。
    KIND_CODE = {"mail": 0, "dmail": 1, "phone": 2, "reply": 3}
    dmail_map = {}
    phone_js = os.path.join(args.source, "src", "common", "phone-data.js")
    if os.path.exists(phone_js):
        text = open(phone_js, encoding="utf-8").read()
        # phone-data.js 里 dmail 是扁平映射:"SG06_23":233 —— 直接全文件抓这组键值。
        for k, v in re.findall(r'"(SG\d+_\d+)"\s*:\s*(\d+)', text):
            dmail_map[k] = int(v)
        m = None
        if False and m:
            for k, v in re.findall(r'"?(SG\d+_\d+)"?\s*:\s*\{[^}]*?id\s*:\s*(\d+)', m.group(1)):
                dmail_map[k] = int(v)
            if not dmail_map:
                for k, v in re.findall(r'"?(SG\d+_\d+)"?\s*:\s*(\d+)', m.group(1)):
                    dmail_map[k] = int(v)
    ch_meta = bytearray()
    ci_meta = bytearray()
    for nid, node in graph.get("nodes", {}).items():
        ch = node.get("choice")
        if not ch:
            continue
        kind = KIND_CODE.get(str(ch.get("kind")))
        if kind is None:
            raise SystemExit("unknown choice kind %r (node %s)" % (ch.get("kind"), nid))
        src_name = str(ch.get("source") or "")
        if kind == 3:                      # reply:outgoing = item.id
            outgoing = 0xFFFF if "id" not in ch else int(ch["id"]) & 0xFFFF
        else:                              # send/dmail:outgoing = phoneData.dmail[source]
            outgoing = dmail_map.get(src_name, 0xFFFF)
        ch_meta += struct.pack("<HHBBH", pool.add(src_name),
                               0xFFFF if "id" not in ch else int(ch["id"]) & 0xFFFF,
                               kind, 0, outgoing)
        for item in ch.get("items") or []:
            item_id = 0xFFFF if "id" not in item else int(item["id"]) & 0xFFFF
            ci_meta += struct.pack("<HH", item_id, 0)

    strt, txt = pool.build()
    # ---- v5 压缩 TXT:2 KiB 独立块,raw DEFLATE(wbits=-15),每块 CRC16 ----
    import zlib as _zlib
    TXT_BLOCK = 2048
    blocks = []
    for base_off in range(0, len(txt), TXT_BLOCK):
        raw = txt[base_off:base_off + TXT_BLOCK]
        co = _zlib.compressobj(9, _zlib.DEFLATED, -15)
        comp = co.compress(raw) + co.flush()
        blocks.append((comp, len(raw), _crc16(raw)))
    txt_comp = b"".join(c for c, _r, _c in blocks) if blocks else b""
    PLAIN = getattr(args, "plain", False)
    if PLAIN:
        blocks = []
    blk_tbl = bytearray()
    cur = 0
    for comp, raw_len, crc in blocks:
        assert raw_len <= TXT_BLOCK and len(comp) <= 0xFFFF
        blk_tbl += struct.pack("<IHH", cur, len(comp), crc)
        cur += len(comp)
    assert PLAIN or cur == len(txt_comp)
    if getattr(args, "plain", False):
        new_sections = [(SEC_TYPES["STRT"], strt, len(strt) // REC_STRT),
                        (SEC_TYPES["TXT"], txt, len(txt))]
    else:
        new_sections = [(SEC_TYPES["STRT"], strt, len(strt) // REC_STRT),
                        (SEC_TYPES["TXT"], txt_comp, len(blocks)),
                        (SEC_TYPES.get("TXTBLK", 40), bytes(blk_tbl), len(blocks)),
                        (SEC_TYPES.get("TXTRAW", 41), struct.pack("<I", len(txt)), 1)]
    # 其余主图段原样保留(顺序与 count 都不变),跳过旧的 STRT/TXT
    for typ, data, count in base.sections:
        if typ in (SEC_TYPES["STRT"], SEC_TYPES["TXT"]):
            continue
        new_sections.append((typ, data, count))
    payloads = {}
    payloads.update(mail_sec)
    payloads.update(windows_sec)
    payloads.update(call_sec)
    payloads.update({k: v for k, v in events_sec.items() if k not in payloads})
    for name in ("MAIL", "MAIL_SUCC", "MAIL_STAMP", "MROW", "MEVT", "MNODE", "CALLSITE",
                 "CALLITEM", "CLIP", "CFRAME", "WINDOW", "WINOPT", "KEYPHONE",
                 "SEED", "SEED_ID", "CALLDESC", "CALLDITEM", "PRES"):
        if name in payloads:
            payload = payloads[name]
            rec = {"MAIL": 12, "MAIL_SUCC": 2, "MAIL_STAMP": 4, "MROW": 12, "MNODE": 6,
                   "CALLSITE": 12, "CALLITEM": 10, "CLIP": 6, "CFRAME": 6,
                   "WINDOW": 14, "WINOPT": 10, "KEYPHONE": 16, "SEED": 4, "CALLDESC": 8, "CALLDITEM": 6,
                   "SEED_ID": 2, "MMAP": 6, "PRES": 10}.get(name)
            count = len(payload) // rec if rec else len(payload)
            new_sections.append((SEC_TYPES[name], payload, count))
    new_sections.append((SEC_TYPES["MMAP"], bytes(mmap), len(mmap) // 6))
    # side 段已经在 pool.build() 之前构造好,这里只负责追加。
    assert len(ch_meta) % 8 == 0, len(ch_meta)
    assert len(ci_meta) % 4 == 0, len(ci_meta)
    new_sections.append((SEC_TYPES["CHOICE_META"], bytes(ch_meta), len(ch_meta) // 8))
    new_sections.append((SEC_TYPES["CITEM_META"], bytes(ci_meta), len(ci_meta) // 4))
    new_sections.append((SEC_TYPES["MOBILE"], mobile_meta, len(mobile_meta)))

    header = 32
    table = 16 * len(new_sections)
    offset = header + table
    layout = []
    for typ, data, _count in new_sections:
        pad = (-offset) % 4
        offset += pad
        layout.append((typ, offset, len(data), pad))
        offset += len(data)
    total = offset

    out = bytearray()
    out += MAGIC
    out += struct.pack("<IIIIII", 4 if getattr(args, "plain", False) else 5, total, len(new_sections), 0, 0, 0)
    for idx, (typ, off, size, pad) in enumerate(layout):
        count = new_sections[idx][2]
        out += struct.pack("<IIII", typ, off, count, size)
    for idx, (typ, off, size, pad) in enumerate(layout):
        out += b"\x00" * pad
        assert len(out) == off, (NUM_TYPES.get(typ), len(out), off)
        out += new_sections[idx][1]
    assert len(out) == total
    open(out_path, "wb").write(out)

    sha = hashlib.sha256(out).hexdigest()

    def manifest_path(value, must_exist):
        path = Path(value).resolve()
        text = path.as_posix()
        if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in text):
            raise SystemExit("manifest path contains control characters: %r" % text)
        if must_exist and not path.exists():
            raise SystemExit("manifest path does not exist: %s" % text)
        return text

    # 把候选包与它的 base 都固化成不可变副本:之后 promote 不会让 manifest 指向被替换的路径。
    frozen = Path(out_dir) / "frozen"
    frozen.mkdir(parents=True, exist_ok=True)
    base_snapshot = frozen / "base-v3.bin"
    if not base_snapshot.exists():
        base_snapshot.write_bytes(Path(args.base_pack).read_bytes())
    report = {
        "pack": manifest_path(out_path, True),
        "size": len(out),
        "sha256": sha,
        "schema": 4,
        "base_pack": manifest_path(str(base_snapshot), True),
        "base_pack_sha256": hashlib.sha256(base_snapshot.read_bytes()).hexdigest(),
        "source_pack_used": manifest_path(args.base_pack, True),
        "counts": {
            "mail_count": mail_stats["mail_count"],
            "nonempty_bodies": mail_stats["nonempty_bodies"],
            "type3_succ": mail_stats["type3_succ"],
            "event_rows": rows,
            "events": sum(event_counts.values()),
            "event_kinds": event_counts,
            "call_frames": len(src["call_frames"]),
            "call_sites": len(src["call_index"] or {}),
            "call_descs": len(events_sec["CALLDESC"]) // 8,
            "seed_descs": len(events_sec["SEED"]) // 4,
            "new_strings": len(pool.extra),
            "windows": len(src["windows"]),
            "keyphone": len(src["keyphone"]),
        },
        "sections": {NUM_TYPES.get(t, str(t)): {"off": o, "size": s}
                     for t, o, s, _p in layout},
        "encoding_problems": bad,
        "fully_verified": False,
        "base_is_immutable": True,
        "note": "候选包:C 读回与 audit 通过前不要替换 assets/steins/steins_pack.bin",
    }
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    report_path = args.report or os.path.join(out_dir, "pack-ready.json")
    open(report_path, "w", encoding="utf-8").write(text + "\n")
    os.makedirs("build/mobile-integration", exist_ok=True)
    open("build/mobile-integration/pack-ready.json", "w", encoding="utf-8").write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
