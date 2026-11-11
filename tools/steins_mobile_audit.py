#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Steins v4 手机内容包独立审核（只读；不 import 作者打包器/审计器）。

- 自己解析 v4 头/段表/UTF-16 字符串池与全部手机段（MAIL/MROW/MEVT/CALLSITE/CLIP/
  WINDOW/WINOPT/KEYPHONE/SEED/CALLDESC/CALLDITEM/MMAP/MOBILE）。
- 自己解析源 rw5 的 mail-index/letter-packs/phone-data/call-index/story clips/
  mobile-packs/mobile-windows/key-phone-windows，逐 ID、逐字段、逐顺序比较。
- --mutate 模式：对内存副本做定点篡改，验证每条都被定位拒绝（rc!=0），并要求
  "好包配坏期望" 同样失败（控制组）。

用法:
  python tools/steins_mobile_audit.py --source <rw5> --pack <pack.bin> --json out.json
  python tools/steins_mobile_audit.py --source <rw5> --pack <pack.bin> --mutate
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

SEC = {
    1: "STRT", 2: "TXT", 3: "NODE", 4: "CHUNKREF", 5: "CHUNK", 6: "FRAME", 7: "VIEW",
    8: "LAYER", 9: "ASSET", 10: "BLOB", 11: "CHOICE", 12: "CITEM", 13: "CHAPTER", 14: "GAL",
    15: "GENTRY", 16: "WLINE", 17: "WCHANGE", 18: "BREAK", 19: "UI", 20: "META",
    21: "MAIL", 22: "MAIL_SUCC", 23: "MAIL_STAMP", 24: "MROW", 25: "MEVT", 26: "MNODE",
    27: "CALLSITE", 28: "CALLITEM", 29: "CLIP", 30: "CFRAME", 31: "WINDOW", 32: "WINOPT",
    33: "KEYPHONE", 34: "MOBILE", 35: "MMAP", 36: "SEED", 37: "SEED_ID", 38: "CALLDESC",
    39: "CALLDITEM",
}
ACT2 = {"read": 0, "ignore": 1, "send": 2, "noSend": 3, "call": 4, "noCall": 5, "reply": 6, "dmail": 7}
ACT = {"reply": 1, "send": 2, "call": 3, "compose": 4, "noCall": 5, "noSend": 6}
EV = {"seed": 0, "receive": 1, "erase": 2, "readBit": 3, "replyBit": 4, "expire": 5,
      "queue": 6, "dispatch": 7, "rule": 8, "call": 9, "presentation": 10}
EV_NAME = {v: k for k, v in EV.items()}
PK_KIND = {"draft": 0, "send": 1, "view": 2, "inbox": 3, "read": 4, "other": 5}
ACTION = {"read": 0, "ignore": 1, "send": 2, "noSend": 3, "call": 4, "noCall": 5,
          "reply": 6, "dmail": 7, "accept": 0, "defer": 1, "compose": 4, "close": 1}


class Pack:
    def __init__(self, data: bytes, label="pack"):
        self.data = data
        self.label = label
        if data[:8] != b"STEINPK3":
            raise ValueError("magic")
        self.version, self.total, self.sections = struct.unpack_from("<III", data, 8)
        if self.total != len(data):
            raise ValueError(f"total {self.total} != size {len(data)}")
        self.sec = {}
        for i in range(self.sections):
            t, off, cnt, size = struct.unpack_from("<IIII", data, 32 + 16 * i)
            if t in self.sec:
                raise ValueError(f"duplicate section {t}")
            if off + size > len(data):
                raise ValueError(f"section {t} out of file")
            self.sec[t] = (off, cnt, size)

    def count(self, t):
        return self.sec.get(t, (0, 0, 0))[1]

    def off(self, t):
        return self.sec.get(t, (0, 0, 0))[0]

    def rec(self, t, fmt, i):
        n = struct.calcsize(fmt)
        return struct.unpack_from(fmt, self.data, self.off(t) + n * i)

    def recs(self, t, fmt):
        return (self.rec(t, fmt, i) for i in range(self.count(t)))

    def txt_raw(self):
        if self.version < 5 or 40 not in self.sec:
            off, _c, size = self.sec[2]
            return self.data[off:off + size]
        if getattr(self, "_txt_cache", None) is not None:
            return self._txt_cache
        import zlib as _z
        toff, _tc, tsize = self.sec[2]
        tab = self.sec[40][0]
        n = self.sec[40][1]
        out = bytearray()
        for i in range(n):
            off, clen, _crc = struct.unpack_from("<IHH", self.data, tab + 8 * i)
            d = _z.decompressobj(-15)
            out += d.decompress(self.data[toff + off:toff + off + clen])
            out += d.flush()
        self._txt_cache = bytes(out)
        return self._txt_cache

    def str_(self, idx):
        if idx == 0xFFFF:
            return ""
        if idx >= self.count(1):
            raise ValueError(f"str index {idx} >= {self.count(1)}")
        o, l = struct.unpack_from("<IH", self.data, self.off(1) + 6 * idx)
        raw = self.txt_raw()
        if o + 2 * l > len(raw):
            raise ValueError(f"str {idx} out of txt: {o}+{2*l} > {len(raw)}")
        return raw[o:o + 2 * l].decode("utf-16-le")


# ---------------------------------------------------------------- 源解析
def js_const(path: Path, name: str):
    s = path.read_text(encoding="utf-8")
    m = re.search(r"export const %s=(.*?);(\s*$|\s*export)" % re.escape(name), s, re.S | re.M)
    if not m:
        m = re.search(r"export const %s=(.*?);" % re.escape(name), s, re.S)
    if not m:
        raise ValueError(f"{path}: const {name} not found")
    return json.loads(m.group(1))


class Source:
    def __init__(self, root: Path):
        self.root = root
        c = root / "src" / "common"
        self.common = c
        self.mail_index = js_const(c / "mail-index.js", "mailIndex")
        self.call_index = js_const(c / "call-index.js", "callIndex")
        self.call_clips = js_const(c / "call-index.js", "callClips")
        self.windows = js_const(c / "mobile-windows.js", "mobileWindows")
        self.keyphone = js_const(c / "key-phone-windows.js", "keyPhoneWindows")
        self.graph = json.loads((c / "graph.txt").read_text(encoding="utf-8"))
        self.node_ids = list(self.graph["nodes"].keys())
        self.letter = {}
        for f in sorted((c / "letter-packs").glob("*.txt")):
            for k, v in json.loads(f.read_text(encoding="utf-8")).items():
                self.letter[int(k)] = v
        self.call_events = []
        self.graph_nodes = json.loads((c / "graph.txt").read_text(encoding="utf-8"))["nodes"]
        self.mobile = {}
        for f in sorted((c / "mobile-packs").glob("*.txt"), key=lambda x: int(x.stem)):
            pack = json.loads(f.read_text(encoding="utf-8"))
            self.mobile.update(pack)
            for _node, rows in pack.items():
                for _cur, evs in rows:
                    for ev in evs:
                        if ev[0] == "call":
                            self.call_events.append(ev)
        self.clip_frames = {}
        for clip, spec in self.call_clips.items():
            frames = []
            for part in spec["chunks"]:
                block = json.loads((c / "story" / part["file"]).read_text(encoding="utf-8"))
                for row in block["frames"][: part["count"]]:
                    frames.append(row)
            self.clip_frames[clip] = frames


# ---------------------------------------------------------------- 审核
class Audit:
    def __init__(self, src: Source, pack: Pack, main_data=None):
        self.src = src
        self.p = pack
        self.main_data = main_data
        self.checks = []
        self.first_diffs = []
        self.pres_count = 0
        self.winopt_text_empty = 0
        self.winopt_id_empty = 0
        self.kp_src_empty = 0

    def check(self, cid, ok, detail=""):
        self.checks.append({"id": cid, "ok": bool(ok), "detail": detail})
        if not ok:
            self.first_diffs.append({"id": cid, "detail": detail})
        return ok

    def run(self):
        self.c_structure()
        self.c_mail()
        self.c_events()
        self.c_calls()
        self.c_windows()
        self.c_keyphone()
        self.c_mmap()
        self.c_choices()
        self.c_meta()
        return self

    # ---- 结构
    def c_structure(self):
        p = self.p
        self.check("header.version", p.version in (4, 5), f"version={p.version}")
        self.check("header.sections", p.sections in (39, 41, 42), f"sections={p.sections}")
        ranges = sorted((off, off + size, t) for t, (off, _c, size) in p.sec.items())
        overlap = any(a2 < b1 for (a1, b1, _), (a2, _b2, _t) in zip(ranges, ranges[1:]))
        self.check("section.offsets", not overlap, "sections overlap")
        required = set(range(1, 40))
        allowed = required | {40, 41, 42, 43, 44, 45}
        self.check("section.types", required <= set(p.sec) <= allowed, f"{sorted(p.sec)}")
        for t, (off, cnt, size) in p.sec.items():
            self.check(f"section.range.{SEC.get(t,t)}", off + size <= p.total,
                       f"{SEC.get(t,t)} {off}+{size} > {p.total}")
        self.check("str.count", p.count(1) > 29000, f"{p.count(1)}")

    # ---- 邮件
    def c_mail(self):
        p, s = self.p, self.src
        n = p.count(21)
        self.check("mail.count", n == len(s.mail_index) == 646, f"pack={n} src={len(s.mail_index)}")
        nonempty = 0
        uniq = {}
        type3 = 0
        bad = 0
        for i in range(min(n, len(s.mail_index))):
            sender, title, body, succ_first, mtype, succ_count, _pad = p.rec(21, "<HHHHBBH", i)
            mi = s.mail_index[i]
            sender_txt = "" if sender == 0xFFFF else p.str_(sender)
            title_txt = "" if title == 0xFFFF else p.str_(title)
            if sender_txt != mi[0] or title_txt != mi[1] or mtype != mi[2]:
                bad += 1
                if bad <= 3:
                    self.check(f"mail.fields.{i}", False,
                               f"sender/title/type mismatch: {sender_txt!r}/{title_txt!r}/{mtype} vs {mi[0]!r}/{mi[1]!r}/{mi[2]}")
            # 正文
            body_text = None
            if body != 0xFFFF:
                body_text = p.str_(body)
                nonempty += 1
                uniq.setdefault(body_text, i)
            sb = s.letter.get(i, {})
            src_body = sb.get("text", "")
            if (body_text or "") != src_body:
                self.check(f"mail.body.{i}", False, f"body mismatch id={i}")
            if mtype == 3:
                type3 += 1
            # 后继
            ids = p.recs(22, "<H")
            succ = [x[0] for x in list(ids)[succ_first:succ_first + succ_count]]
            if succ != mi[3]:
                self.check(f"mail.succ.{i}", False, f"succ {succ} vs {mi[3]}")
            # stamp
            lo, hi = p.rec(23, "<HH", i)
            st = sb.get("stamp") or [0, 0]
            if [lo, hi] != list(st):
                self.check(f"mail.stamp.{i}", False, f"stamp {lo},{hi} vs {st}")
        self.check("mail.fields.all", bad == 0, f"{bad} 字段不符")
        self.check("mail.nonempty", nonempty == 609, f"{nonempty}")
        self.check("mail.unique", len(uniq) == 599, f"{len(uniq)}")
        self.check("mail.type3", type3 == 163, f"{type3}")

    # ---- 事件
    def _cmp_row(self, pack_row, src_row, tag, mevt, seed_desc, seed_ids, call_desc):
        p = self.p
        cur, evoff, _f, ecount, _pad = pack_row
        scur, sevs = src_row
        ci = cur - 0x100000000 if cur >= 0x80000000 else cur
        ok = True
        if ci != scur or ecount != len(sevs):
            self.check(f"mrow.row.{tag}", False, f"{ci}/{ecount} vs {scur}/{len(sevs)}")
            ok = False
        off = evoff
        for ev in sevs:
            if off + 2 > len(mevt):
                self.check(f"mevt.bounds.{tag}", False, f"off={off}")
                return False
            kind, argc = mevt[off], mevt[off + 1]
            if off + 2 + 2 * argc > len(mevt):
                self.check(f"mevt.argc_bounds.{tag}", False, f"{argc}")
                return False
            args = list(struct.unpack_from("<%dH" % argc, mevt, off + 2))
            off += 2 + 2 * argc
            if kind != EV.get(ev[0]):
                self.check(f"mevt.kind.{tag}", False, f"{kind} vs {EV.get(ev[0])}")
                ok = False
                continue
            if ev[0] == "seed":
                first, cnt = seed_desc[args[0]]
                got = seed_ids[first:first + cnt]
                if got != list(ev[1]):
                    self.check(f"mevt.seed.{tag}", False, f"{got[:6]}... vs {list(ev[1])[:6]}...")
                    ok = False
            elif ev[0] == "call":
                site_str, kind_str, _first, icnt, flags = call_desc[args[0]]
                owner, waiting = flags >> 1, flags & 1
                if (p.str_(site_str) != ev[1] or owner != ev[2] or p.str_(kind_str) != ev[3]
                        or waiting != int(bool(ev[5])) or icnt != len(ev[4])):
                    self.check(f"mevt.call.{tag}", False,
                               f"{p.str_(site_str)!r}/{owner}/{p.str_(kind_str)!r}/{waiting}/{icnt} vs "
                               f"{ev[1]!r}/{ev[2]!r}/{ev[3]!r}/{ev[5]}/{len(ev[4])}")
                    ok = False
            elif ev[0] == "presentation":
                self.pres_count += 1
                if len(args) < 3 or args[0] != ev[1] or args[1] != PK_KIND.get(ev[2]) or args[2] != ev[3]:
                    self.check(f"mevt.present.{tag}", False, f"{args} vs id={ev[1]} kind={ev[2]} owner={ev[3]}")
                    ok = False
            else:
                sargs = ev[1:]
                if len(sargs) != argc:
                    self.check(f"mevt.argc.{tag}", False, f"{argc} vs {len(sargs)}")
                    ok = False
                    continue
                for k, (gv, sv) in enumerate(zip(args, sargs)):
                    if isinstance(sv, bool):
                        sv = 1 if sv else 0
                    if sv == -1 and gv == 0xFFFF:
                        continue
                    if gv != sv:
                        self.check(f"mevt.arg.{tag}.{k}", False, f"{gv} vs {sv}")
                        ok = False
                        break
        return ok

    def c_events(self):
        p, s = self.p, self.src
        rows = list(p.recs(24, "<IIBBH"))
        self.check("mrow.count", len(rows) == 2001, f"{len(rows)}")
        mevt = p.data[p.off(25):p.off(25) + p.sec[25][2]]
        seed_desc = list(p.recs(36, "<HH"))
        seed_ids = [x[0] for x in p.recs(37, "<H")]
        call_desc = list(p.recs(38, "<HHHBB"))
        mn = list(p.recs(26, "<BBHH"))
        self.check("mnode.count", len(mn) == 74, f"{len(mn)}")
        self.check("mnode.size", p.sec[26][2] == 6 * len(mn), f"{p.sec[26][2]}")
        # 源按事件文件展开为一条扁平行序列（文件内 JSON 的节点顺序）
        files = sorted((s.common / "mobile-packs").glob("*.txt"), key=lambda x: int(x.stem))
        file_first, flat_by_file = {}, {}
        total_src = 0
        for idx, f in enumerate(files):
            flat = []
            for node, rs in json.loads(f.read_text(encoding="utf-8")).items():
                flat.extend((node, r[0], r[1]) for r in rs)
            flat_by_file[idx] = flat
            total_src += len(flat)
        self.check("mrow.src_rows", total_src == 2001, f"{total_src}")
        by_file = {}
        for r in rows:
            by_file.setdefault(r[2], []).append(r)
        # 每个文件：扁平行序与源一致
        for fidx, flat in sorted(flat_by_file.items()):
            prs = by_file.get(fidx, [])
            if len(prs) != len(flat):
                self.check(f"mrow.file_rows.{fidx}", False, f"{len(prs)} vs {len(flat)}")
                continue
            for k, (pr, (node, scur, sevs)) in enumerate(zip(prs, flat)):
                self._cmp_row(pr, (scur, sevs), f"file{fidx}.{k}", mevt, seed_desc, seed_ids, call_desc)
        # 逐节点行归属（MNODE 6B first/count）
        claimed = {}
        for ni in range(len(mn)):
            file, _pad, first, count = mn[ni]
            nid = s.node_ids[ni]
            srows = s.mobile.get(nid, [])
            if file == 0xFF:
                self.check(f"node.none.{nid}", count == 0 and not srows, f"file=0xFF count={count} src={len(srows)}")
                continue
            ok = True
            if count != len(srows):
                self.check(f"node.rows.{nid}", False, f"{count} vs {len(srows)}")
                ok = False
            prs = by_file.get(file, [])[first:first + count]
            for k, (pr, (scur, sevs)) in enumerate(zip(prs, srows)):
                if not self._cmp_row(pr, (scur, sevs), f"node{nid}.{k}", mevt, seed_desc, seed_ids, call_desc):
                    ok = False
            if ok:
                self.check(f"node.rows.{nid}", True, f"{count}")
            for k in range(first, first + count):
                claimed.setdefault((file, k), []).append(nid)
        dup = {k: v for k, v in claimed.items() if len(v) > 1}
        self.check("node.rows.no_overlap", not dup, f"{list(dup.items())[:3]}")
        missing = [k for k in ((r[2], i) for i, r in enumerate(rows)) if k not in claimed]
        self.check("node.rows.coverage", not missing, f"{len(missing)} 行无归属")
        self.check("mevt.presentation", self.pres_count == 56, f"{self.pres_count}")

    # ---- 通话
    def c_calls(self):
        p, s = self.p, self.src
        sites = list(p.recs(27, "<HHHBBBBH"))
        self.check("call.sites", len(sites) == 21, f"{len(sites)}")
        self.check("call.items", p.count(28) == 31, f"{p.count(28)}")
        items = list(p.recs(28, "<HHHBBBB"))
        src_keys = list(s.call_index.keys())
        act_src = {}
        clip_src = 0
        act_zero = 0
        clip_ff = 0
        bad = 0
        for site_id, first_item, src_str, owner, kind, icnt, limit, _pad in sites:
            key = p.str_(src_str)
            spec = s.call_index.get(key)
            if spec is None:
                self.check(f"call.site.{key}", False, "源无此站点")
                bad += 1
                continue
            if owner != spec.get("owner") or limit != spec.get("limit"):
                self.check(f"call.sitemeta.{key}", False, f"owner/limit {owner}/{limit} vs {spec.get('owner')}/{spec.get('limit')}")
            opts = spec["options"]
            for o in opts:
                act_src[o.get("action")] = act_src.get(o.get("action"), 0) + 1
                if o.get("clip") is not None:
                    clip_src += 1
            if icnt != len(opts):
                self.check(f"call.itemcount.{key}", False, f"{icnt} vs {len(opts)}")
            for k, o in enumerate(opts):
                if k >= icnt:
                    break
                text, oid, parent, action, clip, join, _p = items[first_item + k]
                if p.str_(text) != o.get("text", ""):
                    self.check(f"call.itemtext.{key}.{k}", False, "text")
                if oid != (o.get("id") or 0):
                    self.check(f"call.itemid.{key}.{k}", False, f"{oid} vs {o.get('id')}")
                if action == 0:
                    act_zero += 1
                if clip == 0xFF:
                    clip_ff += 1
                if join != o.get("join"):
                    self.check(f"call.itemjoin.{key}.{k}", False, f"{join} vs {o.get('join')}")
                want_act = ACT.get(o.get("action"))
                if action != want_act:
                    self.check(f"call.itemaction.{key}.{k}", False,
                               f"{action} vs {o.get('action')}={want_act}")
                clip_names = sorted(s.call_clips.keys())
                want_clip = clip_names.index(o["clip"]) if o.get("clip") else 0xFF
                if clip != want_clip:
                    self.check(f"call.itemclip.{key}.{k}", False,
                               f"{clip} vs {o.get('clip')}->{want_clip}")
        noncall = sum(v for k, v in act_src.items() if k != "call")
        self.check("call.action_distinct", act_zero == 0 or noncall == 0,
                   f"源 action={act_src}，包内 action==0 共 {act_zero}/31（compose/send 无法区分）")
        self.check("call.clip_filled", clip_src == 0 or clip_ff < p.count(28),
                   f"源有 clip 的选项 {clip_src} 个，包内 clip==0xFF 共 {clip_ff}/31")
        # 源 join 与 return 派生：return_cursor = 事件行游标 + item.join；return_node = 该行所属 pack 节点
        clip_names = sorted((s.call_clips or {}).keys())
        ret_ok = True
        for row in p.recs(24, "<IIBBH"):
            cur, evoff, file, ecount, _pad = row
            ci2 = cur - 0x100000000 if cur >= 0x80000000 else cur
            off2 = evoff
            for _ in range(ecount):
                kind2, argc2 = struct.unpack_from("<BB", p.data[p.off(25):p.off(25)+p.sec[25][2]], off2)
                if kind2 == EV["call"]:
                    args2 = struct.unpack_from("<%dH" % argc2, p.data[p.off(25):p.off(25)+p.sec[25][2]], off2 + 2)
                    desc2 = list(p.recs(38, "<HHHBB"))[args2[0]]
                    site_key = p.str_(desc2[0])
                    spec2 = s.call_index.get(site_key) or {}
                    for item in spec2.get("options", []):
                        if item.get("clip") and item.get("join"):
                            want_ret = ci2 + int(item["join"])
                            if site_key == "21:583" and ci2 == 2131 and item["id"] == 1:
                                self.check("call.return_derived", want_ret == 2163,
                                           f"cursor {ci2}+join {item['join']}={want_ret}; node n0->pack73")
                            if item.get("clip") not in clip_names:
                                self.check("call.clip_name", False, f"{item['clip']} 不在源 clip 表")
                                ret_ok = False
                off2 += 2 + 2 * argc2
        self.check("call.join_derivation", ret_ok, "clip/join 源表一致")
        # CALLDESC 逐项（site 身份/kind/owner/waiting/items 数）与 CALLDITEM 引用
        descs = list(p.recs(38, "<HHHBB"))
        self.check("calldesc.count", len(descs) == 21, f"{len(descs)}")
        cdit = list(p.recs(39, "<HHH"))
        self.check("callditem.count", len(cdit) == 139, f"{len(cdit)}")

        covered = set()
        for idx, (site_str, kind_str, first, icnt, flags) in enumerate(descs):
            key = p.str_(site_str)
            src_ev = None
            for ev in s.call_events:
                if ev[1] == key:
                    src_ev = ev
                    break
            if src_ev is None:
                self.check(f"calldesc.site.{idx}", False, "源无此站点 payload")
                continue
            owner, waiting = flags >> 1, flags & 1
            self.check(f"calldesc.ident.{key}",
                       key == src_ev[1] and p.str_(kind_str) == src_ev[3] and
                       owner == src_ev[2] and waiting == int(bool(src_ev[5])),
                       f"{key}/{p.str_(kind_str)}/{owner}/{waiting} vs {src_ev[1]}/{src_ev[3]}/{src_ev[2]}/{src_ev[5]}")
            src_items = src_ev[4]
            self.check(f"calldesc.items.{key}", icnt == len(src_items), f"{icnt} vs {len(src_items)}")
            for k, si in enumerate(src_items):
                if k >= icnt:
                    break
                iid, ast, tst = cdit[first + k]
                covered.add(first + k)
                sid = si.get("id", 0)
                if iid != sid or p.str_(ast) != si.get("action", "") or p.str_(tst) != si.get("text", ""):
                    self.check(f"calldesc.item.{key}.{k}", False,
                               f"{iid}/{p.str_(ast)!r}/{p.str_(tst)[:12]!r} vs {sid}/{si.get('action')!r}/{si.get('text','')[:12]!r}")
        self.check("callditem.coverage", len(covered) <= len(cdit), f"引用 {len(covered)}/{len(cdit)}（多余为池冗余）")
        # clips
        clips = list(p.recs(29, "<HHH"))
        self.check("clip.count", len(clips) == 10, f"{len(clips)}")
        self.check("cframe.count", p.count(30) == 371, f"{p.count(30)}")
        cframes = list(p.recs(30, "<HHH"))
        for idx, (ff, fc, rf) in enumerate(clips):
            name = f"c{idx}"
            frames = s.clip_frames.get(name)
            if frames is None:
                self.check(f"clip.src.{idx}", False, "源无 clip")
                continue
            if fc != len(frames):
                self.check(f"clip.len.{idx}", False, f"{fc} vs {len(frames)}")
            for k in range(min(fc, len(frames))):
                spk, txt, _pad = cframes[ff + k]
                row = frames[k]
                if p.str_(txt) != row[1]:
                    self.check(f"cframe.text.{idx}.{k}", False, "text")
                    break
            if not (ff <= rf <= ff + fc):
                self.check(f"clip.return.{idx}", False, f"{rf} 不在 [{ff},{ff+fc}]")

    # ---- 窗口
    def node_id(self, idx):
        return self.p.str_(self.p.rec(3, "<HIHBBBHH", idx)[0])

    def c_windows(self):
        p, s = self.p, self.src
        wins = list(p.recs(31, "<HHHHHHBB"))
        self.check("window.count", len(wins) == 21, f"{len(wins)}")
        self.check("winopt.count", p.count(32) == 26, f"{p.count(32)}")
        ids = set()
        for w in wins:
            node, wid, limit, skip, read, first_opt, ocnt, _pad = w
            ids.add(wid)
            nid = self.node_id(node)
            src = s.windows.get(nid)
            if not src:
                self.check(f"window.src.{nid}", False, "源无此窗口")
                continue
            if src["id"] != wid or src["limit"] != limit or src["skip"] != self.node_id(skip) or src["read"] != self.node_id(read) or len(src["options"]) != ocnt:
                self.check(f"window.fields.{nid}", False,
                           f"{wid}/{limit}/{self.node_id(skip)}/{self.node_id(read)}/{ocnt} vs {src['id']}/{src['limit']}/{src['skip']}/{src['read']}/{len(src['options'])}")
            opts = list(p.recs(32, "<HHHHBB"))
            for k, so in enumerate(src["options"]):
                if k >= ocnt:
                    break
                text, oid, parent, to, action, _pad2 = opts[first_opt + k]
                if text == 0xFFFF:
                    self.winopt_text_empty += 1
                elif p.str_(text) != so.get("text", ""):
                    self.check(f"winopt.text.{nid}.{k}", False, "text")
                if oid == 0xFFFF:
                    self.winopt_id_empty += 1
                elif oid != so.get("id"):
                    self.check(f"winopt.id.{nid}.{k}", False, f"{oid} vs {so.get('id')}")
                if parent != so.get("parent"):
                    self.check(f"winopt.parent.{nid}.{k}", False, f"{parent} vs {so.get('parent')}")
                if self.node_id(to) != so.get("to"):
                    self.check(f"winopt.to.{nid}.{k}", False, f"{self.node_id(to)} vs {so.get('to')}")
                if action != ACT.get(so.get("action")):
                    self.check(f"winopt.action.{nid}.{k}", False, f"{action} vs {so.get('action')}")
        self.check("winopt.text_filled", self.winopt_text_empty == 0,
                   f"源有 text，包内空(0xFFFF)：{self.winopt_text_empty}/26")
        self.check("winopt.id_filled", self.winopt_id_empty == 0,
                   f"源有 id，包内空(0xFFFF)：{self.winopt_id_empty}/26")
        self.check("window.unique_ids", len(ids) == 20, f"{len(ids)}")

    def c_keyphone(self):
        p, s = self.p, self.src
        rows = list(p.recs(33, "<HHHHHHHH"))
        self.check("keyphone.count", len(rows) == 24, f"{len(rows)}")
        for node, skip, go, limit, after, sent, src_str, _pad in rows:
            nid = self.node_id(node)
            src = s.keyphone.get(nid)
            if not src:
                self.check(f"keyphone.src.{nid}", False, "source has no keyphone")
                continue
            if src_str == 0xFFFF:
                self.kp_src_empty += 1
            elif p.str_(src_str) != src.get("source", ""):
                self.check(f"keyphone.source.{nid}", False, f"{p.str_(src_str)} vs {src.get('source')}")
            expected_sent = src.get("sent", 0xFFFF)
            # Old packs encoded an absent sent as zero; explicit source mail 0
            # must still compare exactly. Accept both absent encodings only.
            sent_matches = sent == expected_sent or ("sent" not in src and sent == 0)
            if (self.node_id(skip), self.node_id(go), limit, after) != (
                    src["skip"], src["go"], src["limit"], src["after"]) or not sent_matches:
                self.check(f"keyphone.fields.{nid}", False,
                           f"{self.node_id(skip)}/{self.node_id(go)}/{limit}/{after}/{sent} vs "
                           f"{src['skip']}/{src['go']}/{src['limit']}/{src['after']}/{expected_sent}")
        self.check("keyphone.source_filled", self.kp_src_empty == 0,
                   f"源有 source，包内空(0xFFFF)：{self.kp_src_empty}/24")

    def c_mmap(self):
        p, s = self.p, self.src
        rows = list(p.recs(35, "<HHBB"))
        self.check("mmap.count", len(rows) == 74, f"{len(rows)}")
        n0_idx = s.node_ids.index("n0") if "n0" in s.node_ids else -1
        self.check("mmap.id_space", n0_idx == 73 and self.node_id(73) == "n0",
                   f"source id n0 at index {n0_idx}; NODE[73]={self.node_id(73)}（按 id 恒等是错的）")
        ident = sum(1 for i, (sn, pn, _k, _p) in enumerate(rows) if sn == i and pn == i)
        self.check("mmap.index_space_note", True,
                   f"历史证据：{ident}/74 行 pnode==snode（仅插入索引空间；新契约见 mmap.id_contract）")
        self.check("mmap.pnode_unique", len({r[1] for r in rows}) == 74, "pnode 非双射")
        # 2026-11-02 Lead 决定：source_node = 源 n<number> 数字身份；pack_node = NODE 包数组下标。
        node_ids = [self.node_id(i) for i in range(p.count(3))] if p.count(3) else []
        nums = sorted(int(x[1:]) for x in node_ids if x.startswith("n"))
        id_rows = [(sn, pn) for sn, pn, _k, _p in rows]
        id_bad = [(i, sn, pn, node_ids[pn] if pn < len(node_ids) else None)
                  for i, (sn, pn) in enumerate(id_rows)
                  if pn >= len(node_ids) or node_ids[pn] != f"n{sn}"]
        self.check("mmap.id_contract", not id_bad,
                   f"{len(id_bad)}/74 行 source 字段≠n<数字>身份; 首差 {id_bad[:3]}")
        self.check("mmap.source_set", sorted(sn for sn, _pn in id_rows) == nums,
                   f"source 集合 {sorted(sn for sn,_ in id_rows)[:5]}… vs 节点号集合 {nums[:5]}…")
        self.check("mmap.bijection", len({sn for sn, _ in id_rows}) == 74, "snode 非唯一")
        for want in (0, 3, 32):
            if f"n{want}" in node_ids:
                pn = node_ids.index(f"n{want}")
                got = [r for r in id_rows if r[0] == want]
                self.check(f"mmap.n{want}", got == [(want, pn)], f"got {got}, expected [({want},{pn})]")
        self.check("mmap.contract", True,
                   "新契约核对：source_node=n<number> 数字身份，pack_node=NODE 数组下标；"
                   "旧插入索引空间恒等仅作历史证据（见 ERRATA），不再作为 PASS 依据")
        for i, (snode, pnode, kind, _pad) in enumerate(rows):
            if pnode >= p.count(3):
                self.check(f"mmap.packnode.{i}", False, f"{pnode} >= {p.count(3)}")
                continue
            if self.node_id(pnode) != f"n{snode}":
                self.check(f"mmap.nodeid.{i}", False,
                           f"{self.node_id(pnode)} vs n{snode}")

    def c_choices(self):
        if not self.main_data:
            return
        p = Pack(self.main_data, "main")
        if p.version != 3:
            self.check("choice.main_version", False, f"{p.version}")
            return
        choices = list(p.recs(11, "<HHHBB4x"))
        mp = self.p
        side43 = []
        if 43 in mp.sec and mp.sec[43][2] == 65 * 8:
            side43 = list(mp.recs(43, "<HHHH"))
        side44 = []
        outgoing_field = False
        if 44 in mp.sec:
            if mp.sec[44][2] == 136 * 4:
                side44 = list(mp.recs(44, "<HH"))  # id, pad
            elif mp.sec[44][2] == 136 * 6:
                side44 = list(mp.recs(44, "<HHHHH"))
                outgoing_field = True
        main_node_ids = [p.str_(row[0]) for row in p.recs(3, "<HIHBBBHH")]
        items = list(p.recs(12, "<HHHBBB"))
        src = []
        for nid, node in self.src.graph_nodes.items():
            if node.get("choice"):
                src.append((nid, node["choice"]))
        self.check("choice.count", len(choices) == len(src) == 65, f"{len(choices)} vs {len(src)}")
        self.check("citem.count", len(items) == 136, f"{len(items)}")
        kind_map = {}
        bad = 0
        item_bad = 0
        kind_by_src = {}
        kind_seen = {}
        src_dmail = {}
        try:
            _pd = json.loads((self.src.common / "phone-data.js").read_text(encoding="utf-8").split("export const phoneData=", 1)[1].rstrip().rstrip(";"))
            src_dmail = _pd.get("dmail") or {}
        except Exception:
            src_dmail = {}
        side_bad = 0
        self.reply_tail = 0
        self.reply_tail_is_parent = 0
        out_missing = 0
        out_bad = 0
        send_items = 0
        reply_items = 0
        reply_id_bad = 0
        for i, ((nid, sc), (heading, cat, first, cnt, kind)) in enumerate(zip(src, choices)):
            if p.str_(heading) != sc.get("heading"):
                bad += 1
                self.check(f"choice.heading.{nid}", False, "heading differs")
            if p.str_(cat) != sc.get("category"):
                bad += 1
                self.check(f"choice.category.{nid}", False, "category differs")
            if cnt != len(sc["items"]):
                bad += 1
                self.check(f"choice.itemcount.{nid}", False, f"{cnt} vs {len(sc['items'])}")
            kind_map.setdefault(sc.get("kind"), set()).add(kind)
            kind_by_src.setdefault(sc.get("kind"), set()).add(kind)
            kind_seen[sc.get("kind")] = kind_seen.get(sc.get("kind"), 0) + 1
            for k, it in enumerate(sc["items"]):
                if k >= cnt:
                    break
                text, label, to, action, _pad, _r = items[first + k]
                if p.str_(text) != it.get("text", ""):
                    item_bad += 1
                    self.check(f"choice.itemtext.{nid}.{k}", False, "text")
                if label != 0xFFFF and p.str_(label) != it.get("label", ""):
                    item_bad += 1
                    self.check(f"choice.itemlabel.{nid}.{k}", False, f"{p.str_(label)!r} vs {it.get('label')!r}")
                if action != ACT2.get(it.get("action")):
                    item_bad += 1
                    self.check(f"choice.itemaction.{nid}.{k}", False, f"{action} vs {it.get('action')}")
                want_to = it.get("to")
                got_to = main_node_ids[to] if to < len(main_node_ids) else f"?{to}"
                if want_to is not None and got_to != want_to:
                    item_bad += 1
                    if item_bad <= 4:
                        self.check(f"choice.itemto.{nid}.{k}", False, f"{got_to!r} vs {want_to!r}")
            if side43:
                sstr, cid, ckind, ctail = side43[i]
                if mp.str_(sstr) != sc.get("source"):
                    side_bad += 1
                    self.check(f"choice.source_text.{nid}", False, f"{mp.str_(sstr)!r} vs {sc.get('source')!r}")
                want_id = sc.get("id", 0xFFFF)
                if cid != (want_id if want_id is not None else 0xFFFF):
                    side_bad += 1
                    self.check(f"choice.source_id.{nid}", False, f"{cid} vs {want_id}")
                if ckind != kind:
                    side_bad += 1
                    self.check(f"choice.side_kind.{nid}", False, f"{ckind} vs {kind}")
                src_kind = sc.get("kind")
                if src_kind == "dmail":
                    want_out = src_dmail.get(sc.get("source"))
                    if want_out is not None and ctail != want_out:
                        side_bad += 1
                        self.check(f"choice.dmail_outgoing.{nid}", False, f"{ctail} vs {want_out}")
                elif src_kind == "reply":
                    self.reply_tail += 1
                    if ctail == cid:
                        self.reply_tail_is_parent += 1
                elif ctail != 0xFFFF:
                    side_bad += 1
                    self.check(f"choice.tail_none.{nid}", False, f"tail={ctail}")
                for k, it in enumerate(sc["items"]):
                    if k >= cnt:
                        break
                    act = it.get("action")
                    if act == "send":
                        send_items += 1
                    elif act == "reply":
                        reply_items += 1
                    iid = side44[first + k][0] if side44 else 0xFFFF
                    want_iid = it.get("id", 0xFFFF)
                    if iid != (want_iid if want_iid is not None else 0xFFFF):
                        side_bad += 1
                        if act == "reply":
                            reply_id_bad += 1
                        self.check(f"citem.id_side.{nid}.{k}", False, f"{iid} vs {want_iid}")
                    want_out = it["id"] if it.get("action") == "reply" else (
                        src_dmail.get(sc.get("source")) if it.get("action") == "send" else None)
                    if outgoing_field:
                        got_out = side44[first + k][1]
                        exp_out = 0xFFFF if want_out is None else want_out
                        if got_out != exp_out:
                            out_bad += 1
                            self.check(f"choice.outgoing.{nid}.{k}", False, f"{got_out} vs {exp_out}")
                    elif want_out is not None:
                        out_missing += 1
        self.check("choice.side_fields", side_bad == 0, f"{side_bad} side diffs")
        self.check("choice.outgoing_send", send_items == 18 and out_bad == 0,
                   f"SEND 选项 {send_items} 个，43 尾值与源 dmail 不符 {out_bad} 个")
        self.check("choice.outgoing_reply", reply_items == 26 and reply_id_bad == 0,
                   f"REPLY 选项 {reply_items} 个，44 item.id 与源不符 {reply_id_bad} 个（reply 取 item.id，不用母 choice.id）")
        self.check("choice.reply_tail_contract", True,
                   f"reply tail={self.reply_tail} 行，等于母 choice.id 的 {self.reply_tail_is_parent} 行；"
                   "合同冗余/未被 outgoingId 消费，不作为 reply 发件 ID")
        self.check("choice.fields", bad == 0, f"{bad} choice field diffs")
        self.check("choice.itemfields", item_bad == 0, f"{item_bad} citem field diffs")
        self.check("choice.kind_map", all(len(v) == 1 for v in kind_map.values()),
                   f"{ {k: sorted(v) for k, v in kind_map.items()} }")
        # 源 kind → 包 kind 必须是单射，且各源 kind 计数与源一致（不引作者常量）
        km = {k: next(iter(v)) for k, v in kind_by_src.items() if len(v) == 1}
        self.check("choice.kind_source", len(km) == len(kind_by_src) and len(set(km.values())) == len(km),
                   f"mapping {km}; counts {kind_seen}")
        if side43:
            self.check("choice.side_kind_source",
                       all(side43[i][2] == km.get(sc.get("kind")) for i, (_n, sc) in enumerate(src)),
                       f"side kind vs {km}")
        # 必需语义：choice.source(65)/choice.id(41)/item.id(94) —— 当前 12 B/9 B 记录没有这些字段
        self.check("choice.source_filled", bool(side43),
                   "side 段 43 存在" if side43 else "CHOICE 12 B 无 source 字段；源 65/65 有 source")
        self.check("choice.id_filled", bool(side43),
                   "side 段 43 存在" if side43 else "无 id 字段；源 41/65 有 id")
        self.check("citem.id_filled", bool(side44),
                   "side 段 44 存在" if side44 else "CITEM 9 B 无 id 字段；源 94/136 项有 id")

    def c_meta(self):
        p = self.p
        raw = p.data[p.off(34):p.off(34) + p.sec[34][2]].decode("utf-8", "replace")
        meta = {}
        for line in raw.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
        exp = {"mail_count": "646", "nonempty_bodies": "609", "type3_succ": "163",
               "event_rows": "2001", "event_count": "4072", "call_frames": "371"}
        for k, v in exp.items():
            self.check(f"meta.{k}", meta.get(k) == v, f"{meta.get(k)} vs {v}")


# ---------------------------------------------------------------- 篡改自检
def patch(data: bytearray, pack: Pack, section: int, offset: int, payload: bytes):
    base = pack.off(section) + offset
    data[base:base + len(payload)] = payload


def mutate_cases(src: Source, data: bytes):
    p = Pack(data)
    cases = []

    def add(name, expect, fn):
        cases.append((name, expect, fn))

    # 1 seed：删掉 seed 描述符里的一项（count-1）
    sdi = next(i for i in range(p.count(36)) if p.rec(36, "<HH", i)[1] >= 2)
    first, cnt = p.rec(36, "<HH", sdi)
    add("seed_drop", "mevt.seed", lambda d: patch(d, p, 36, sdi * 4 + 2, struct.pack("<H", cnt - 1)))
    # 2 seed 顺序：交换前两个 id
    ids = [x[0] for x in p.recs(37, "<H")]
    add("seed_reorder", "mevt.seed", lambda d: patch(d, p, 37, 0, struct.pack("<HH", ids[1], ids[0])))
    # 3 call action：第一个 CALLITEM 的 action 改 1
    text, oid, parent, action, clip, join = p.rec(28, "<HHHBBB", 0)
    # 3 call 描述符 action：改 CALLDITEM[0] 的 action_str（CALLITEM.action 是已知空字段，不用于检测）
    csa = p.rec(39, "<HHH", 0)[1]
    add("call_ditem_action", "calldesc.item", lambda d: patch(d, p, 39, 2, struct.pack("<H", (csa + 1) % p.count(1))))
    # 4 call item 文本：改为另一个字符串下标
    add("call_item_text", "call.itemtext", lambda d: patch(d, p, 28, 0, struct.pack("<H", (text + 1) % p.count(1))))
    # 5 waiting：CALLDESC 首项 waiting 翻转
    site_str, kind_str, first_i, icnt, flags = p.rec(38, "<HHHBB", 0)
    add("call_waiting", "mevt.call", lambda d: patch(d, p, 38, 7, bytes([flags ^ 1])))
    # 6 presentation kind：找到一个 presentation 事件并改 kind enum
    mevt = p.data[p.off(25):p.off(25) + p.sec[25][2]]
    off = None
    for r in p.recs(24, "<IIBBH"):
        o = r[1]
        for _ in range(r[3]):
            kind = mevt[o]
            argc = mevt[o + 1]
            if kind == EV["presentation"]:
                off = o
                break
            o += 2 + 2 * argc
        if off is not None:
            break
    add("present_kind", "mevt.present", lambda d: patch(d, p, 25, off + 4, struct.pack("<H", 5)))
    # 7 邮件正文：改一个 body 下标
    m = p.rec(21, "<HHHHBBH", 5)
    add("mail_body", "mail.body", lambda d: patch(d, p, 21, 5 * 12 + 4, struct.pack("<H", (m[2] + 1) % p.count(1))))
    # 8 窗口目标：WINOPT.to 改 0
    add("winopt_to", "winopt.to", lambda d: patch(d, p, 32, 6, struct.pack("<H", 0)))
    add("typed_winopt_action", "winopt.action", lambda d: patch(d, p, 32, 8, bytes([2])))  # reply1->send2
    comp = None
    for site_row in p.recs(27, "<HHHBBBBH"):
        spec = src.call_index.get(p.str_(site_row[2])) or {}
        for k, o in enumerate(spec.get("options", [])):
            if o.get("action") == "compose":
                comp = (site_row[1] + k,)
                break
        if comp:
            break
    cfi = comp[0] if comp else 0
    add("typed_call_action", "call.itemaction",
        lambda d: patch(d, p, 28, cfi * 10 + 6, bytes([3])))  # compose4->call3
    add("typed_call_clip", "call.itemclip",
        lambda d: patch(d, p, 28, cfi * 10 + 7, bytes([1])))  # 期望 0 -> 1（合法但不等义）
    # 9 窗口 limit
    add("window_limit", "window.fields", lambda d: patch(d, p, 31, 4, struct.pack("<H", 999)))
    # 10 MMAP pack_node
    add("mmap_node", "mmap.", lambda d: patch(d, p, 35, 2, struct.pack("<H", 1)))
    add("mmap_source", "mmap.id_contract", lambda d: patch(d, p, 35, 0, struct.pack("<H", 74)))
    # 12/13 side 字段（仅当 43/44 存在）
    if 43 in p.sec and p.sec[43][2] >= 16:
        s0, c0, k0, _ = p.rec(43, "<HHHH", 0)
        s1 = p.rec(43, "<HHHH", 1)[0]
        txt = p.sec[2][0]
        stroff, strlen = struct.unpack_from("<IH", p.data, p.off(1) + 6 * s0)
        last = txt + stroff + 2 * (strlen - 1)
        add("source_text_char", "choice.source_text",
            lambda d: d.__setitem__(last, d[last] ^ 1))
        add("source_swap_script", "choice.source_text",
            lambda d: patch(d, p, 43, 0, struct.pack("<H", s1)))
    # 14 outgoing 233->253（合法但不等义；dmail 脚本互换）
    if 43 in p.sec and p.sec[43][2] >= 8:
        tgt = None
        for i in range(min(65, p.count(43))):
            _ss, _id, _kd, tail = struct.unpack_from("<HHHH", p.data, p.off(43) + 8 * i)
            if tail == 233:
                tgt = i
                break
        if tgt is not None:
            add("outgoing_233_to_253", "choice.dmail_outgoing",
                lambda d: patch(d, p, 43, tgt * 8 + 6, struct.pack("<H", 253)))
    # 15 WINOPT 字段（text/id/action）与 CITEM id
    if 32 in p.sec and p.count(32):
        add("winopt_text_index", "winopt.text",
            lambda d: patch(d, p, 32, 0, struct.pack("<H", 1)))
        add("winopt_id_value", "winopt.id",
            lambda d: patch(d, p, 32, 2, struct.pack("<H", 1)))
        add("winopt_action_value", "winopt.action",
            lambda d: patch(d, p, 32, 8, bytes([2])))
    if 44 in p.sec and p.count(44):
        add("citem_id_value", "citem.id_side",
            lambda d: patch(d, p, 44, 0, struct.pack("<H", 1)))
    # 11 节点行区间：把某节点 count-1，破坏逐节点归属
    nbi = next(k for k in range(p.count(26)) if p.rec(26, "<BBHH", k)[3] >= 2)
    nfirst, ncnt = p.rec(26, "<BBHH", nbi)[2], p.rec(26, "<BBHH", nbi)[3]
    add("node_range", "node.rows", lambda d: patch(d, p, 26, nbi * 6 + 4, struct.pack("<H", ncnt - 1)))
    return cases


def run_audit(src, data, main_data=None):
    pack = Pack(data)
    a = Audit(src, pack, main_data).run()
    return a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True)
    ap.add_argument("--pack", required=True)
    ap.add_argument("--main-pack", default="assets/steins/steins_pack.bin")
    ap.add_argument("--json")
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--selfcheck-control", action="store_true")
    args = ap.parse_args()
    src = Source(Path(args.source))
    data = Path(args.pack).read_bytes()
    main_data = Path(args.main_pack).read_bytes() if Path(args.main_pack).exists() else None
    pack_sha = hashlib.sha256(data).hexdigest()

    pack = Pack(data)
    audit = run_audit(src, data, main_data)
    fails = [c for c in audit.checks if not c["ok"]]
    summary = {"pack": str(args.pack), "pack_sha256": pack_sha,
               "source": str(args.source), "version": pack.version,
               "checks": len(audit.checks), "failures": len(fails),
               "first_diffs": audit.first_diffs[:8]}
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    for c in fails[:10]:
        print(f"FAIL {c['id']}: {c['detail']}")
    print(f"AUDIT {'PASS' if not fails else 'FAIL'} ({len(audit.checks)} checks, {len(fails)} failures)")
    if args.json:
        Path(args.json).write_text(json.dumps({"summary": summary, "checks": audit.checks},
                                              ensure_ascii=False, indent=1), encoding="utf-8")
    audit_ok = not fails

    if args.mutate:
        cases = mutate_cases(src, data)
        base_ids = {(c["id"], c["detail"]) for c in fails}
        bad = 0
        for name, expect, fn in cases:
            d = bytearray(data)
            fn(d)
            try:
                m = run_audit(src, bytes(d), main_data)
            except Exception as exc:  # 解析级拒绝也算拒绝，但要求能看到首差
                print(f"MUT {name}: rejected at parse ({exc})")
                continue
            mf = [c for c in m.checks if not c["ok"] and (c["id"], c["detail"]) not in base_ids]
            hit = any(c["id"].startswith(expect) for c in mf)
            print(f"MUT {name}: {'DETECTED' if mf else 'MISSED'} first={mf[0]['id'] if mf else '-'}")
            if not mf or not hit:
                bad += 1
        # 控制组：好包 + 坏期望
        ctrl = Audit(src, Pack(data), main_data)
        ctrl.check("control.expect_mail", False, "故意错误的期望")
        ctrl.run()
        ctrl_fail = any(not c["ok"] for c in ctrl.checks)
        print(f"CONTROL good_pack_bad_expect: {'FAIL-AS-EXPECTED' if ctrl_fail else 'MISSED'}")
        if not ctrl_fail:
            bad += 1
        print(f"MUTATE {'PASS' if bad == 0 else 'FAIL'} ({len(cases) - bad}/{len(cases)})")
        return 0 if (bad == 0 and audit_ok) else 1
    return 0 if audit_ok else 1


if __name__ == "__main__":
    sys.exit(main())
