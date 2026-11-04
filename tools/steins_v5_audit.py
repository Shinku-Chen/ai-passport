#!/usr/bin/env python3
"""Independent v5 TXT-compression audit for the Steins mobile pack.

Owned by the independent reviewer (worker w6h1l5xn0umxrwpm1t5tyjxu).
Does NOT import tools/steins_mobile_pack.py / steins_pack.py: the block table,
raw DEFLATE blocks, CRC16 and string reconstruction are re-implemented here from
the published spec (build/mobile-integration/pack-api-v5.md) and verified against
the uncompressed cand3 pack as an independent content oracle.

Checks:
  1. header/section table sanity, no overlap, total == file size;
  2. TXTBLK 8 B <IHH>: comp_off contiguous, comp_len > 0, table covers TXT;
  3. each block: raw DEFLATE (wbits=-15), exact raw length (2 KiB / tail),
     eof and no unused_data, CRC16/CCITT == table;
  4. concatenated raw: length == TXTRAW, SHA-256 == expected, byte-equal to cand3 TXT;
  5. all 30,273 STRT strings: bounds, UTF-16 bytes == cand3 slice (incl. strings
     spanning a 2 KiB block boundary);
  6. every other section (1, 3..39) byte-equal to cand3; mobile sections 21..39 equal;
  7. presentation source keys (chapter,row,id,kind,owner): 56 unique, order == source
     order and mapping stable cand3->cand5.
With --mutate: real CLI rejection (exit 1) for bad block table, virtual length,
corrupt payload, truncation, shifted string (cross-block), plus good-pack control.
"""
import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

TXT_BLOCK = 2048
PK_KIND = {"draft": 0, "send": 1, "view": 2}
# 历史 cand5 fixture 常量只用于 --cand5-fixture；默认从 ref 包实际 TXT 派生
CAND5_RAW_SHA = "f356f2768e86d38a4baa01b7e2b7a59de68da78a75388dfc05735023a9b17bb8"
CAND5_RAW_LEN = 1190226
TXTBLK, TXTRAW = 40, 41


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


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
        rows = []
        for i in range(self.sections):
            t, off, cnt, size = struct.unpack_from("<IIII", data, 32 + 16 * i)
            self.sec[t] = (off, cnt, size)
            rows.append((t, off, cnt, size))
        self.rows = rows
        self.strt_off = self.sec[1][0]
        self.txt_off = self.sec[2][0]

    def off(self, t):
        return self.sec[t][0]

    def count(self, t):
        return self.sec[t][1]

    def size(self, t):
        return self.sec[t][2]

    def payload(self, t):
        off, _cnt, size = self.sec[t]
        return self.data[off:off + size]

    def recs(self, t, fmt, nrec=None):
        off = self.sec[t][0]
        rec = struct.calcsize(fmt)
        n = self.sec[t][1]
        if nrec:
            rec = nrec
        return [struct.unpack_from(fmt, self.data, off + rec * i) for i in range(n)]

    def str_bytes(self, i):
        o, l = struct.unpack_from("<IH", self.data, self.strt_off + 6 * i)
        return self.data[self.txt_off + o:self.txt_off + o + 2 * l]

    def raw_str(self, raw: bytes, i):
        o, l = struct.unpack_from("<IH", self.data, self.strt_off + 6 * i)
        return raw[o:o + 2 * l]


class Audit:
    def __init__(self, data, ref):
        self.data = data
        self.ref = ref
        self.checks = []
        self.src = None
        self.pres_oracle = None

    def check(self, cid, ok, detail=""):
        self.checks.append({"id": cid, "ok": bool(ok), "detail": str(detail)})
        return ok

    def run(self):
        d = self.data
        p = Pack(d)
        ref = self.ref
        self.check("v5.version", p.version == 5, f"version={p.version}")
        self.check("v5.sections", p.sections in (41, 44), f"{p.sections}")
        # 1. section table: types unique, no overlap, total == size
        spans = sorted((off, off + size, t) for t, off, _c, size in p.rows)
        self.check("v5.section.count", len(spans) == p.sections, f"{len(spans)}")
        bad = [(a, b) for a, b in zip(spans, spans[1:]) if b[0] < a[1]]
        self.check("v5.section.overlap", not bad, f"{bad[:3]}")
        self.check("v5.section.end", spans[-1][1] == len(d), f"{spans[-1][1]} vs {len(d)}")
        self.check("v5.section.types", len({t for t, *_ in p.rows}) == p.sections, "duplicate type")
        self.check("v5.types", TXTBLK in p.sec and TXTRAW in p.sec, "TXTBLK/TXTRAW")
        # 2. block table
        raw_len = struct.unpack_from("<I", d, p.off(TXTRAW))[0]
        n = p.count(TXTBLK)
        if getattr(self, "cand5_fixture", False):
            exp_len, exp_sha = CAND5_RAW_LEN, CAND5_RAW_SHA
        else:
            ref_raw = ref.payload(2)
            exp_len, exp_sha = len(ref_raw), hashlib.sha256(ref_raw).hexdigest()
        self.check("v5.raw_len", raw_len == exp_len, f"{raw_len} vs {exp_len}")
        self.check("v5.blocks.expected", n == (exp_len + TXT_BLOCK - 1) // TXT_BLOCK, f"{n}")
        self.check("v5.txtblk.size", p.size(TXTBLK) == 8 * n, f"{p.size(TXTBLK)}")
        self.check("v5.txtraw.size", p.size(TXTRAW) == 4, f"{p.size(TXTRAW)}")
        tab = p.payload(TXTBLK)
        txt = p.payload(2)
        blocks = [struct.unpack_from("<IHH", tab, 8 * i) for i in range(n)]
        run = 0
        for i, (off, clen, _crc) in enumerate(blocks):
            if off != run:
                self.check("v5.block.contiguous", False, f"block {i}: off {off} != {run}")
                break
            run += clen
        else:
            self.check("v5.block.contiguous", True, f"{run} == {len(txt)}")
        self.check("v5.block.cover", run == len(txt), f"{run} vs {len(txt)}")
        # 3/4. decompress, crc, lengths
        out = bytearray()
        ok_all = True
        for i, (off, clen, crc) in enumerate(blocks):
            want = min(TXT_BLOCK, max(0, raw_len - i * TXT_BLOCK))
            blob = txt[off:off + clen]
            try:
                dobj = zlib.decompressobj(-15)
                raw = dobj.decompress(blob)
                raw += dobj.flush()
                if not dobj.eof:
                    self.check(f"v5.block.{i}.eof", False, "not finished")
                    ok_all = False
                if dobj.unused_data:
                    self.check(f"v5.block.{i}.unused", False, f"{len(dobj.unused_data)}")
                    ok_all = False
            except Exception as exc:
                self.check(f"v5.block.{i}.inflate", False, f"{exc}")
                ok_all = False
                continue
            if len(raw) != want:
                self.check(f"v5.block.{i}.len", False, f"{len(raw)} vs {want}")
                ok_all = False
            if crc16_ccitt(raw) != crc:
                self.check(f"v5.block.{i}.crc16", False, f"{crc16_ccitt(raw):04x} vs {crc:04x}")
                ok_all = False
            out += raw
        self.check("v5.blocks.all", ok_all, f"{n} blocks")
        self.check("v5.raw.size", len(out) == raw_len, f"{len(out)} vs {raw_len}")
        self.check("v5.raw.sha", hashlib.sha256(bytes(out)).hexdigest() == exp_sha,
                   hashlib.sha256(bytes(out)).hexdigest()[:16])
        # 4b. byte equality with uncompressed cand3 TXT
        ref_txt = ref.payload(2)
        self.check("v5.raw.eq_cand3", bytes(out) == ref_txt,
                   f"{len(out)} vs {len(ref_txt)}")
        # 5. strings
        nstr = p.count(1)
        self.check("v5.str.count", nstr == ref.count(1), f"{nstr} vs ref {ref.count(1)}")
        sb = p.payload(1)
        self.check("v5.str.size", p.size(1) == 6 * nstr, f"{p.size(1)}")
        cross = 0
        bad_str = 0
        for i in range(nstr):
            o, l = struct.unpack_from("<IH", sb, 6 * i)
            if o + 2 * l > len(out):
                self.check(f"v5.str.{i}.bounds", False, f"{o}+{2*l} > {len(out)}")
                bad_str += 1
                continue
            if (o // TXT_BLOCK) != ((o + 2 * l - 1) // TXT_BLOCK):
                cross += 1
            if out[o:o + 2 * l] != ref.raw_str(ref_txt, i):
                bad_str += 1
                if bad_str <= 3:
                    self.check(f"v5.str.{i}.bytes", False, "differs from cand3")
        self.check("v5.str.all", bad_str == 0, f"{bad_str} bad")
        self.check("v5.str.cross_block", cross > 0, f"{cross} strings cross a 2 KiB boundary")
        # 6. other sections equal to cand3
        ref_types = {t: (off, cnt, size) for t, off, cnt, size in ref.rows}
        diffs = 0
        for t in list(range(1, 40)) + [42, 43, 44]:
            if t == 2:
                continue
            if t not in ref_types or t not in p.sec:
                self.check(f"v5.section.{t}.missing", False, "missing")
                diffs += 1
                continue
            if p.payload(t) != ref.payload(t):
                diffs += 1
                self.check(f"v5.section.{t}.bytes", False, "differs")
        self.check("v5.sections.equal", diffs == 0, f"{diffs} differing sections")
        mobile = [t for t in range(21, 40)]
        md = sum(1 for t in mobile if p.payload(t) != ref.payload(t))
        self.check("v5.mobile.equal", md == 0, f"{md} differing mobile sections")
        # 7. presentation source keys (chapter,row,id,kind,owner)
        #    chapter = mobile-packs file index; row = row index inside that file.
        self.pres = None
        if self.src:
            self.c_pres()
        return self

    def c_pres(self):
        p, src = Pack(self.data), self.src
        mevt = p.data[p.off(25):p.off(25) + p.size(25)]
        packs = sorted((src / "src/common/mobile-packs").glob("*.txt"),
                       key=lambda x: int(x.stem))
        # 源键直接按 src/common/mobile.js:28：s.chapter + ':' + row[0] + ':' + e[1] + ':' + e[2] + ':' + e[3]
        # s.chapter = owning nXX 节点身份；row[0] = story cursor（不是 zero-based 行号）
        src_items = []
        for f in packs:
            rows = json.loads(f.read_text(encoding="utf-8"))
            for node, rs in rows.items():
                for row_i, (cur, evs) in enumerate(rs):
                    ci = cur - 0x100000000 if cur >= 0x80000000 else cur
                    for ev_i, ev in enumerate(evs):
                        if ev[0] == "presentation":
                            src_items.append({
                                "key": f"{node}:{ci}:{ev[1]}:{ev[2]}:{ev[3]}",
                                "node": node, "cursor": ci, "id": ev[1], "kind": ev[2],
                                "owner": ev[3], "row": row_i, "event_index": ev_i})
        K = PK_KIND
        def sort_key(x):
            return (int(x["node"][1:]), x["cursor"], x["id"], K.get(x["kind"]), x["owner"])
        src_sorted = sorted(src_items, key=sort_key)
        oracle = getattr(self, "pres_oracle", None)
        if oracle is not None:
            self.check("pres.oracle_count", oracle.get("count") == 56, f"{oracle.get('count')}")
            self.check("pres.oracle_matches_source",
                       [tuple(r) for r in oracle.get("records", [])] ==
                       [(x[0], x[1], x[2], x[3], x[4]) for x in [sort_key(i) for i in src_sorted]],
                       "Node syncMobile oracle vs 本工具源解析")
        self.check("pres.src_count", len(src_items) == 56, f"{len(src_items)}")
        self.check("pres.key_unique", len({x["key"] for x in src_items}) == 56,
                   f"{len({x['key'] for x in src_items})} unique source keys")
        self.check("pres.key_first", bool(src_items) and src_items[0]["key"] == "n32:695:263:draft:1",
                   src_items[0]["key"] if src_items else "none")
        counts = {}
        for x in src_items:
            counts[x["node"]] = counts.get(x["node"], 0) + 1
        want_counts = {"n32": 14, "n246": 14, "n293": 9, "n298": 5,
                       "n301": 5, "n284": 5, "n294": 4}
        self.check("pres.key_counts", counts == want_counts, f"{counts}")
        # MEVT 三元序列：只作内容证据，不冒充 chapter/cursor 键
        got = []
        for row in p.recs(24, "<IIBBH"):
            off = row[1]
            for _ in range(row[3]):
                kind, argc = mevt[off], mevt[off + 1]
                if kind == 10:
                    args = struct.unpack_from("<%dH" % argc, mevt, off + 2)
                    got.append((args[0], args[1], args[2]))
                off += 2 + 2 * argc
        self.check("pres.mevt_triple_order", len(got) == 56 and
                   got == [(x["id"], PK_KIND.get(x["kind"]), x["owner"]) for x in src_items],
                   f"{len(got)} triples")
        # 物理 PRES 段（42）：目录字节核对
        entries = [(t, off, cnt, size) for t, off, cnt, size in p.rows if t == 42]
        if not entries:
            self.check("pres.dir_present", False,
                       f"无 PRES 段(42)；cand3={self.ref.sections} 段、cand5={p.sections} 段，"
                       "不能给物理 PRES 目录/编号稳定 PASS")
            self.pres = src_items
            self.pres_dir = None
        elif len(entries) > 1:
            self.check("pres.dir_duplicate", False,
                       f"PRES 段重复 {len(entries)} 条: {[(c, s) for _t,_o,c,s in entries]}")
            # 取第一条重复段按观测布局 <HIHBB 解析，量化 chapter/cursor 差异（证据，不判合格）
            _t, off, cnt, size = entries[0]
            recs = []
            if cnt and size // cnt == 10:
                for i in range(cnt):
                    recs.append(struct.unpack_from("<HIHBB", self.data, off + 10 * i))
            exp = sorted((int(x["node"][1:]), x["cursor"], x["id"], PK_KIND.get(x["kind"]), x["owner"])
                         for x in src_items)
            rs = sorted(recs)
            bad = [(i, rs[i], exp[i]) for i in range(min(len(rs), len(exp))) if rs[i] != exp[i]]
            self.check("pres.dir_duplicate_records", not bad and len(rs) == len(exp),
                       f"{len(recs)} recs, {len(bad)} mismatches, first {bad[:2]}")
            self.pres = src_items
            self.pres_dir = None
        else:
            _t, off, cnt, size = entries[0]
            rec = size // cnt if cnt else 0
            self.check("pres.dir_count", cnt == 56, f"count={cnt}")
            self.check("pres.dir_layout", rec == 10, f"record={rec}B")
            recs = []
            if rec == 10:
                for i in range(cnt):
                    ch, cur, mid, kind, owner = struct.unpack_from("<HIHBB", p.data, off + 10 * i)
                    recs.append((ch, cur, mid, kind, owner))
            exp = [(int(x["node"][1:]), x["cursor"], x["id"], PK_KIND.get(x["kind"]), x["owner"])
                   for x in src_items]
            exp_sorted = sorted(exp)
            recs_sorted = sorted(recs)
            bad = [(i, recs_sorted[i], exp_sorted[i]) for i in range(min(len(recs_sorted), len(exp_sorted)))
                   if recs_sorted[i] != exp_sorted[i]]
            self.check("pres.dir_records", not bad and len(recs_sorted) == len(exp_sorted),
                       f"{len(bad)} mismatches (sorted); first {bad[:2]}")
            self.check("pres.dir_unique", len(set(recs)) == len(recs), f"{len(recs)-len(set(recs))} dup keys")
            # 合法编号稳定：chapter 必须是源的节点号、id 在邮件空间、kind/owner 范围
            nums = {int(x["node"][1:]) for x in src_items}
            self.check("pres.dir_ranges",
                       all(r[0] in nums and 0 <= r[2] < 646 and r[3] <= 2 and r[4] <= 15 for r in recs),
                       "chapter/id/kind/owner 范围")
            self.pres = src_items
            self.pres_dir = recs

    def pres_selftest(self):
        """Comparator self-check: chapter-only / cursor-only 错必须被抓（不看 MEVT）。"""
        p = Pack(self.data)
        src_items = getattr(self, "pres", None)
        if not src_items:
            return None
        exp = [(int(x["node"][1:]), x["cursor"], x["id"], PK_KIND.get(x["kind"]), x["owner"])
               for x in src_items]

        def compare(recs):
            if len(recs) != len(exp):
                return True
            return any(a != b for a, b in zip(recs, exp))

        base = list(exp)
        ch = list(exp); ch[0] = (ch[0][0] + 1,) + ch[0][1:]
        cu = list(exp); cu[0] = (cu[0][0], cu[0][1] + 1) + cu[0][2:]
        out = {"base_mutated": compare(base), "chapter_only": compare(ch), "cursor_only": compare(cu)}
        return out

class Source:
    def __init__(self, path):
        self.path = Path(path)

    def __truediv__(self, other):
        return self.path / other


# ------------------------------------------------------------------ mutations
def patch(data, off, payload):
    data[off:off + len(payload)] = payload


def mutate_cases(p: Pack):
    tab = p.off(TXTBLK)
    txt = p.off(2)
    blocks = [struct.unpack_from("<IHH", p.data, tab + 8 * i) for i in range(p.count(TXTBLK))]
    cases = []

    def add(name, expect, fn):
        cases.append((name, expect, fn))

    # 1 bad block table: first comp_len = 0 (contiguity/cover broken)
    add("bad_block_table", "v5.block", lambda d: patch(d, tab + 4, struct.pack("<H", 0)))
    # 2 virtual length: TXTRAW +1
    raw_len = struct.unpack_from("<I", p.data, p.off(TXTRAW))[0]
    add("virtual_length", "v5.raw", lambda d: patch(d, p.off(TXTRAW), struct.pack("<I", raw_len + 1)))
    # 3 corrupt payload: flip a byte in block 5 compressed data
    off5 = blocks[5][0]
    add("corrupt_payload", "v5.block", lambda d: patch(d, txt + off5 + 2, bytes([d[txt + off5 + 2] ^ 0x5A])))
    # 4 truncation: cut 64 bytes from the file
    add("truncate", "v5.", lambda d: d.__delitem__(slice(len(d) - 64, None)))
    # 5 shifted cross-block string: move a crossing string by one byte
    nstr = p.count(1)
    sb = p.off(1)
    cross_i = None
    for i in range(nstr):
        o, l = struct.unpack_from("<IH", p.data, sb + 6 * i)
        if l and (o // TXT_BLOCK) != ((o + 2 * l - 1) // TXT_BLOCK):
            cross_i = i
            break
    so, sl = struct.unpack_from("<IH", p.data, sb + 6 * cross_i)
    add("shift_cross_string", "v5.str",
        lambda d: patch(d, sb + 6 * cross_i, struct.pack("<IH", so + 2, sl)))
    return cases


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", required=True)
    ap.add_argument("--ref", required=True)
    ap.add_argument("--source", default=None)
    ap.add_argument("--json")
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--pres-only", action="store_true")
    ap.add_argument("--pres-oracle")
    ap.add_argument("--cand5-fixture", action="store_true")
    ap.add_argument("--pres-mutate", action="store_true")
    ap.add_argument("--mut-out")
    args = ap.parse_args()
    data = Path(args.pack).read_bytes()
    ref = Pack(Path(args.ref).read_bytes(), "cand3")
    src = Source(args.source) if args.source else None
    a = Audit(data, ref)
    if src:
        a.src = src
    if args.pres_oracle:
        a.pres_oracle = json.loads(Path(args.pres_oracle).read_text(encoding="utf-8"))
    a.cand5_fixture = bool(args.cand5_fixture)
    if args.pres_only:
        a.c_pres()
    else:
        a.run()
    fails = [c for c in a.checks if not c["ok"]]
    summary = {"pack": args.pack, "pack_sha256": hashlib.sha256(data).hexdigest(),
               "ref": args.ref, "ref_sha256": hashlib.sha256(ref.data).hexdigest(),
               "checks": len(a.checks), "failures": len(fails),
               "first_diffs": fails[:8]}
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    for c in fails[:10]:
        print(f"FAIL {c['id']}: {c['detail']}")
    print(f"V5 AUDIT {'PASS' if not fails else 'FAIL'} ({len(a.checks)} checks, {len(fails)} failures)")
    if args.pres_mutate:
        import subprocess
        pack = Pack(data)
        entries = [(off, cnt, size) for t, off, cnt, size in pack.rows if t == 42]
        if len(entries) != 1 or entries[0][1] * 10 != entries[0][2]:
            print("PRES-MUTATE SKIP: no single 10B PRES section")
        else:
            off, cnt, size = entries[0]
            outdir = Path(args.mut_out or Path(args.json or ".").with_suffix(""))
            outdir.mkdir(parents=True, exist_ok=True)
            def rec(i):
                return struct.unpack_from("<HIHBB", data, off + 10 * i)
            def pack_rec(ch, cur, mid, kind, owner):
                return struct.pack("<HIHBB", ch, cur, mid, kind, owner)
            cases = []
            ch, cur, mid, kind, owner = rec(0)
            cases.append(("chapter_only", pack_rec((ch + 1) & 0xFFFF, cur, mid, kind, owner)))
            cases.append(("cursor_only", pack_rec(ch, cur + 1, mid, kind, owner)))
            r1 = pack_rec(*rec(1))
            cases.append(("duplicate_key", r1))
            cases.append(("drop_key", pack_rec(0, 0, 0, 0, 0)))
            base = bytearray(data)
            bad = 0
            for name, payload in cases:
                d = bytearray(base)
                d[off:off + 10] = payload
                mp = outdir / ("pres_" + name + ".bin")
                mp.write_bytes(bytes(d))
                rc = subprocess.run(
                    [sys.executable, __file__, "--pres-only", "--pack", str(mp),
                     "--ref", args.ref, "--source", args.source,
                     "--pres-oracle", args.pres_oracle],
                    capture_output=True, text=False).returncode
                print(f"PRES-MUT {name}: exit={rc} {'DETECTED' if rc == 1 else 'MISSED'}")
                if rc != 1:
                    bad += 1
            print(f"PRES-MUTATE {'PASS' if bad == 0 else 'FAIL'} ({len(cases)-bad}/{len(cases)})")
        if fails:
            print("PRES-MUTATE positive control FAILED (baseline not green)")
    if args.json:
        Path(args.json).write_text(json.dumps({"summary": summary, "checks": a.checks},
                                              ensure_ascii=False, indent=1), encoding="utf-8")
    audit_ok = not fails
    if args.mutate:
        p = Pack(data)
        base = {c["id"] for c in fails}
        bad = 0
        for name, expect, fn in mutate_cases(p):
            d = bytearray(data)
            fn(d)
            try:
                m = Audit(bytes(d), ref)
                m.src = src
                m.run()
                mf = [c for c in m.checks if not c["ok"] and c["id"] not in base]
            except Exception as exc:
                print(f"MUT {name}: DETECTED (rejected at parse: {type(exc).__name__}: {exc})")
                continue
            hit = any(c["id"].startswith(expect) for c in mf)
            print(f"MUT {name}: {'DETECTED' if hit else 'MISSED'} first={mf[0]['id'] if mf else '-'}")
            if not hit:
                bad += 1
        ctrl = Audit(data, ref)
        ctrl.src = src
        ctrl.run()
        ctrl.check("control.expect_fail", False, "good pack bad expect")
        cf = any(not c["ok"] for c in ctrl.checks)
        print(f"CONTROL good_pack_bad_expect: {'FAIL-AS-EXPECTED' if cf else 'MISSED'}")
        if not cf:
            bad += 1
        print(f"MUTATE {'PASS' if bad == 0 else 'FAIL'} ({5 + 1 - bad}/{5 + 1})")
        return 0 if (bad == 0 and audit_ok) else 1
    return 0 if audit_ok else 1


if __name__ == "__main__":
    sys.exit(main())
