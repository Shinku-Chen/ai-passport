#!/usr/bin/env python3
"""Independent font/inventory audit for the Steins AI Passport candidate.

Owned by the independent reviewer (worker w6h1l5xn0umxrwpm1t5tyjxu).
Does NOT import the author's font tool/writer. Required code points come from
the 075447 plain pack STR/TXT plus production UI C/H string literals
(comments/escapes/adjacent literals handled here). Coverage/non-empty/metrics
are verified against generated LVGL cmap/bitmap/dsc files directly.

Usage:
  python tools/steins_font_audit.py --snapshot <dir> --json <out.json> [--mutate]
"""
import argparse
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

UI_FILES = ["steins_app.c", "steins_ui.c", "steins_pack.c", "steins_pack.h",
            "steins_mobile.c", "steins_mobile.h", "steins_model.c",
            "steins_model.h", "steins_image.c", "steins_image.h"]
LOG_RE = re.compile(r"\bESP_LOG[EWIDV]\b|\bESP_LOGE\b|\bLOGE\b|\bLOGI\b|\bLOGD\b|\bLOGE\b")


def parse_font(path: Path):
    txt = path.read_text(encoding="utf-8", errors="replace")
    name = re.search(r"const lv_font_t\s+(\w+)\s*=", txt)
    fb = re.search(r"\.fallback\s*=\s*&(\w+)", txt)
    cm = txt.index("cmaps[] = {")
    cm_end = txt.index("};", cm)
    cmaps_text = txt[cm:cm_end]
    ds = txt.index("glyph_dsc[] = {")
    ds_end = txt.index("};", ds)
    dsc_text = txt[ds:ds_end]
    bs = txt.index("glyph_bitmap[] = {")
    bs = txt.index("{", bs) + 1
    be = txt.index("};", bs)
    bmp_text = txt[bs:be]
    dsc = []
    for m in re.finditer(r"\{\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),"
                         r"\s*\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),"
                         r"\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\}", dsc_text):
        dsc.append(tuple(int(m.group(i)) for i in range(1, 7)))
    bmp = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})", bmp_text))
    cmaps = []
    for c in re.finditer(r"\{[^{}]*?\.range_start\s*=\s*(\d+),\s*\.range_length\s*=\s*(\d+),"
                         r"[^{}]*?\.glyph_id_start\s*=\s*(\d+),[^{}]*?"
                         r"\.unicode_list\s*=\s*(\w+),[^{}]*?"
                         r"(?:\.list_length\s*=\s*(\d+),[^{}]*?)?"
                         r"\.type\s*=\s*(LV_FONT_FMT_TXT_CMAP_\w+)", cmaps_text, re.S):
        start, length, gid0 = int(c.group(1)), int(c.group(2)), int(c.group(3))
        uname = c.group(4)
        typ = c.group(6)
        arr = re.search(re.escape(uname) + r"\[\]\s*=\s*\{(.*?)\}", txt, re.S)
        vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", arr.group(1))] if arr else []
        cmaps.append({"start": start, "gid0": gid0, "type": typ, "vals": vals})
    cps = {}
    for c in cmaps:
        t = c["type"]
        if "FORMAT0_TINY" in t:
            for i, off in enumerate(c["vals"]):
                cps[c["start"] + off] = c["gid0"] + i
        elif "SPARSE_TINY" in t:
            # LVGL TINY/SPARSE_TINY: unicode_list stores offsets from range_start
            for i, off in enumerate(c["vals"]):
                cps[c["start"] + off] = c["gid0"] + i
        elif "SPARSE_FULL" in t:
            ofs = []
            m = re.search(r"glyph_id_ofs_list\s*=\s*(\w+)", cmaps_text)
            if m:
                a = re.search(re.escape(m.group(1)) + r"\[\]\s*=\s*\{(.*?)\}", txt, re.S)
                ofs = [int(x) for x in re.findall(r"\d+", a.group(1))] if a else []
            for i, cp in enumerate(c["vals"]):
                cps[cp] = c["gid0"] + (ofs[i] if i < len(ofs) else 0)
    return {"name": name.group(1) if name else path.stem,
            "fallback": fb.group(1) if fb else None,
            "cps": cps, "glyphs": dsc, "bitmap": bmp}


def glyph_bytes(font, cp):
    gid = font["cps"].get(cp)
    if gid is None or gid >= len(font["glyphs"]):
        return None
    idx, adv, w, h, ox, oy = font["glyphs"][gid]
    total = (w * h + 1) // 2  # 4bpp nibble-packed, no per-row padding
    raw = font["bitmap"][idx:idx + total]
    return {"adv": adv, "box": (w, h), "ofs": (ox, oy), "raw": raw,
            "set": any(raw) and w > 0 and h > 0, "idx": idx, "gid": gid}


def parse_metrics(path: Path):
    txt = path.read_text(encoding="utf-8", errors="replace")
    return {int(cp, 16): int(adv) for cp, adv in
            re.findall(r"\{0x([0-9A-Fa-f]+),\s*(\d+)\}", txt)}


def strip_comments(src: str):
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"':
                    break
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
        elif c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def decode_c_string(tok: str):
    body = tok[1:-1]
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.extend(c.encode("utf-8"))
            i += 1
            continue
        i += 1
        e = body[i] if i < len(body) else ""
        i += 1
        mp = {"n": 10, "r": 13, "t": 9, "0": 0, "\\": 92, '"': 34, "'": 39,
              "a": 7, "b": 8, "f": 12, "v": 11}
        if e in mp:
            out.append(mp[e])
        elif e == "x":
            h = re.match(r"[0-9A-Fa-f]{1,2}", body[i:])
            if h:
                out.append(int(h.group(0), 16))
                i += len(h.group(0))
        elif e == "u":
            out.extend(chr(int(body[i:i + 4], 16)).encode("utf-8"))
            i += 4
        elif e == "U":
            out.extend(chr(int(body[i:i + 8], 16)).encode("utf-8"))
            i += 8
        elif e.isdigit():
            o = e + re.match(r"[0-7]{0,2}", body[i:]).group(0)
            out.append(int(o, 8) & 0xFF)
            i += len(o) - 1
        else:
            out.extend(e.encode("utf-8"))
    return out.decode("utf-8", "replace")


def ui_literals(snap: Path):
    literals = []
    for fn in UI_FILES:
        p = snap / fn
        if not p.exists():
            continue
        src = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
        i, n = 0, len(src)
        while i < n:
            if src[i] != '"':
                i += 1
                continue
            start = i
            parts = []
            while i < n and src[i] == '"':
                j = i + 1
                while j < n:
                    if src[j] == "\\":
                        j += 2
                        continue
                    if src[j] == '"':
                        break
                    j += 1
                parts.append(src[i:j + 1])
                i = j + 1
                k = i
                while k < n and src[k] in " \t\r\n":
                    k += 1
                if k < n and src[k] == '"':
                    i = k
                else:
                    break
            line_start = src.rfind("\n", 0, start) + 1
            line_end = src.find("\n", i)
            line = src[line_start:line_end if line_end >= 0 else n]
            if LOG_RE.search(line) or line.lstrip().startswith("#") or "#include" in line:
                continue
            text = "".join(decode_c_string(t) for t in parts)
            if text:
                literals.append((fn, text))
    return literals


def pack_strings(path: Path):
    b = path.read_bytes()
    ver, total, sc = struct.unpack_from("<III", b, 8)
    if ver >= 5:
        raise ValueError("plain v4 pack required")
    sec = {}
    for i in range(sc):
        t, off, cnt, size = struct.unpack_from("<IIII", b, 32 + 16 * i)
        sec[t] = (off, cnt, size)
    soff, toff = sec[1][0], sec[2][0]
    txt = b[toff:toff + sec[2][2]]
    out = []
    for i in range(sec[1][1]):
        o, l = struct.unpack_from("<IH", b, soff + 6 * i)
        out.append(txt[o:o + 2 * l].decode("utf-16-le", "replace"))
    return out


def cps_of(strings):
    s = set()
    for x in strings:
        s.update(ord(c) for c in x)
    return s


def category(cp):
    if cp < 0x80:
        return "ascii"
    if 0x4E00 <= cp <= 0x9FFF or 0x3400 <= cp <= 0x4DBF:
        return "cjk"
    if 0x3000 <= cp <= 0x303F or 0xFF00 <= cp <= 0xFFEF or 0x2000 <= cp <= 0x206F:
        return "punct"
    if 0xE000 <= cp <= 0xF8FF:
        return "pua"
    return "other"


class Audit:
    def __init__(self, snap: Path):
        self.snap = snap
        self.checks = []

    def check(self, cid, ok, detail=""):
        self.checks.append({"id": cid, "ok": bool(ok), "detail": str(detail)})

    def run(self):
        snap = self.snap
        story = parse_font(snap / "steins_story_16.c")
        ui16 = parse_font(snap / "steins_ui_16.c")
        ui18 = parse_font(snap / "steins_ui_18.c")
        metrics = parse_metrics(snap / "steins_text_metrics.c")
        old_story = parse_font(snap / "steins_story_16_old.c")
        old_ui = parse_font(snap / "steins_ui_16_old.c")
        pack = pack_strings(snap / "plain.bin")
        ui = ui_literals(snap)
        pack_cps, ui_cps = cps_of(pack), cps_of(t for _f, t in ui)
        controls = {cp for cp in (pack_cps | ui_cps) if cp < 0x20 or cp == 0x7F}
        required = (pack_cps | ui_cps) - controls

        self.check("font.story_count", len(story["cps"]) == 3412, f"{len(story['cps'])}")
        self.check("font.ui16_count", len(ui16["cps"]) == 449, f"{len(ui16['cps'])}")
        self.check("font.ui18_count", len(ui18["cps"]) == 449, f"{len(ui18['cps'])}")
        self.check("font.metrics_count", len(metrics) == 3861, f"{len(metrics)}")
        union = set(story["cps"]) | set(ui16["cps"])
        self.check("font.metrics_set", set(metrics) == union,
                   f"missing={len(union-set(metrics))} extra={len(set(metrics)-union)}")
        self.check("font.story_fallback", story["fallback"] == "steins_ui_16",
                   f"{story['fallback']}")

        miss16 = sorted(cp for cp in required if cp not in story["cps"] and cp not in ui16["cps"])
        self.check("font.required_covered_16", not miss16,
                   f"missing {len(miss16)}: {[hex(c) for c in miss16[:8]]}")
        miss18 = sorted(cp for cp in required if cp not in ui18["cps"] and cp not in story["cps"])
        self.check("font.required_covered_18_chain", not miss18,
                   f"ui18 direct missing {len(miss18)}（经 story fallback）")

        empty, blank_pua, adv_bad = [], [], []
        for cp in sorted(required):
            g = glyph_bytes(story, cp)
            if g is None or not g["set"]:
                g2 = glyph_bytes(ui16, cp)
                if g2 is None or not g2["set"]:
                    if cp == 0x20:
                        continue
                    if 0xE000 <= cp <= 0xF8FF:
                        blank_pua.append(cp)
                        continue
                    empty.append(cp)
            if cp in metrics:
                g = glyph_bytes(story, cp) or glyph_bytes(ui16, cp)
                if g is not None and g["adv"] != metrics[cp]:
                    adv_bad.append((cp, g["adv"], metrics[cp]))
        self.check("font.glyph_nonempty", not empty,
                   f"empty visible {len(empty)}: {[hex(c) for c in empty[:8]]}; "
                   f"blank PUA allowed {len(blank_pua)}")
        self.check("font.metrics_adv", not adv_bad,
                   f"adv mismatch {len(adv_bad)}: {[(hex(c), a, b) for c, a, b in adv_bad[:5]]}")
        self.blank_pua = blank_pua

        pua_req = sorted(cp for cp in required if 0xE000 <= cp <= 0xF8FF)
        pua_missing = [cp for cp in pua_req if cp not in story["cps"]]
        self.check("font.pua_body", not pua_missing,
                   f"PUA required {len(pua_req)}, missing {len(pua_missing)}")

        def compare(old, new, tag):
            bad = []
            for cp in sorted(set(old["cps"]) & set(new["cps"])):
                a, b = glyph_bytes(old, cp), glyph_bytes(new, cp)
                if not a or not b:
                    continue
                if (a["adv"], a["box"], a["ofs"], a["raw"]) != (b["adv"], b["box"], b["ofs"], b["raw"]):
                    bad.append(cp)
            self.check(f"font.old_invariant.{tag}", not bad,
                       f"{len(bad)} changed: {[hex(c) for c in bad[:8]]}")
        compare(old_story, story, "story")
        compare(old_ui, ui16, "ui16")

        old_union = set(old_story["cps"]) | set(old_ui["cps"])
        added = sorted(union - old_union)
        cats = {}
        for cp in added:
            cats.setdefault(category(cp), []).append(cp)
        self.new_added = added
        self.cats = cats
        self.check("font.new_counts", True,
                   "added " + ", ".join(f"{k}={len(v)}" for k, v in sorted(cats.items())) +
                   "; examples " + ", ".join(
                       f"{k}:" + "".join(chr(c) for c in v[:6]) for k, v in sorted(cats.items())))

        keys = ["收信箱", "已发送", "联系人", "电话", "存档损坏"]
        found = {k: sum(1 for _f, t in ui if k in t) for k in keys}
        self.check("ui.inventory_present", all(v > 0 for v in found.values()), f"{found}")
        self.ui_stats = {"literals": len(ui), "unique_cps": len(ui_cps),
                         "controls": sorted(controls), "found": found,
                         "required_total": len(required),
                         "pack_cps": len(pack_cps), "ui_cps": len(ui_cps)}
        self.check("ui.controls", True, f"control cps {[hex(c) for c in sorted(controls)]}")
        self.check("ui.required_total", True,
                   f"required {len(required)} (pack {len(pack_cps)} ∪ ui {len(ui_cps)} - controls)")

        bad_all = []
        for cp in union:
            if cp not in metrics:
                continue
            g = glyph_bytes(story, cp) or glyph_bytes(ui16, cp)
            if g and g["adv"] != metrics[cp]:
                bad_all.append(cp)
        self.check("font.metrics_all", not bad_all, f"{len(bad_all)} mismatches")
        self.fonts = {"story": story, "ui16": ui16, "ui18": ui18}
        self.required = required
        return self


def mutate_and_run(snap: Path, outdir: Path, tool: Path):
    old_story = parse_font(snap / "steins_story_16_old.c")
    old_ui = parse_font(snap / "steins_ui_16_old.c")
    old_union = set(old_story["cps"]) | set(old_ui["cps"])
    pick = None
    for tag, fn in (("story", "steins_story_16.c"), ("ui16", "steins_ui_16.c")):
        font = parse_font(snap / fn)
        cps = [cp for cp in sorted(set(font["cps"]) - old_union) if 0x4E00 <= cp <= 0x9FFF]
        if cps:
            pick = (tag, fn, font, cps[0])
            break
    if not pick:
        print("MUTATE SKIP: no new CJK glyph")
        return 1
    tag, fn, font, cp = pick
    metrics = parse_metrics(snap / "steins_text_metrics.c")
    print(f"MUTATE target: {tag} U+{cp:04X} adv={metrics.get(cp)}")
    outdir.mkdir(parents=True, exist_ok=True)
    cases = []
    mtxt = (snap / "steins_text_metrics.c").read_text(encoding="utf-8")
    pat = re.compile(r"(\{0x%04X,\s*)(\d+)(\})" % cp)
    mtxt2, n = pat.subn(lambda m: m.group(1) + str(int(m.group(2)) + 1) + m.group(3), mtxt, count=1)
    cases.append(("adv_changed", "steins_text_metrics.c", mtxt2, n))
    g = glyph_bytes(font, cp)
    ftxt = (snap / fn).read_text(encoding="utf-8")
    bs = ftxt.index("glyph_bitmap[] = {")
    bs = ftxt.index("{", bs) + 1
    be = ftxt.index("};", bs)
    bmp_text = ftxt[bs:be]
    hexes = list(re.finditer(r"0x([0-9A-Fa-f]{1,2})", bmp_text))
    want = len(g["raw"])
    i0, i1 = g["idx"], g["idx"] + want - 1
    if i1 < len(hexes):
        stxt2 = (ftxt[:bs + hexes[i0].start(1)] + ("00 " * (want - 1)) + "00" +
                 ftxt[bs + hexes[i1].end(1):])
        cases.append((f"bitmap_zeroed_{tag}", fn, stxt2, 1))
    bad = 0
    for name, target_fn, content, n in cases:
        if not n:
            print(f"FONT-MUT {name}: SKIP (pattern not found)")
            bad += 1
            continue
        d = outdir / name
        d.mkdir(parents=True, exist_ok=True)
        for f in snap.iterdir():
            if f.is_file():
                (d / f.name).write_bytes(f.read_bytes())
        (d / target_fn).write_text(content, encoding="utf-8")
        rc = subprocess.run([sys.executable, str(tool), "--snapshot", str(d)],
                            capture_output=True).returncode
        print(f"FONT-MUT {name}: exit={rc} {'DETECTED' if rc == 1 else 'MISSED'}")
        if rc != 1:
            bad += 1
    return 0 if bad == 0 and len(cases) == 2 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--json")
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--mut-out")
    args = ap.parse_args()
    snap = Path(args.snapshot)
    a = Audit(snap).run()
    fails = [c for c in a.checks if not c["ok"]]
    summary = {"checks": len(a.checks), "failures": len(fails), "first_diffs": fails[:6]}
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    for c in fails[:8]:
        print(f"FAIL {c['id']}: {c['detail']}")
    print(f"FONT AUDIT {'PASS' if not fails else 'FAIL'} ({len(a.checks)} checks, {len(fails)} failures)")
    if args.json:
        Path(args.json).write_text(json.dumps({"summary": summary, "checks": a.checks},
                                              ensure_ascii=False, indent=1), encoding="utf-8")
    ok = not fails
    if args.mutate:
        mrc = mutate_and_run(snap, Path(args.mut_out or (snap.parent / "font-mutated")),
                             Path(__file__).resolve())
        ok = ok and mrc == 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
