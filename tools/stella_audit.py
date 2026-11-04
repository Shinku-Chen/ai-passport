#!/usr/bin/env python3
"""Independent semantic audit of the Stella pack against the source checkout.

This intentionally does not import tools/stella_pack.py and does not read a
build report. Asset IDs are read from the pack's own META mapping
(`asset.<index>=<id>`), so every source image reference is resolved to a
concrete asset table index and can be re-audited from the artifact alone.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

BS = "\\"
TEXT, NAME, SCENE, OPS, GALLERY, ASSET, BLOB, META = range(8)


def extract(path: Path, marker: str):
    s = path.read_text(encoding="utf-8")
    i = s.index(marker) + len(marker)
    while s[i] != "{":
        i += 1
    depth = 0
    instr = False
    esc = False
    for j in range(i, len(s)):
        c = s[j]
        if instr:
            if esc:
                esc = False
            elif c == BS:
                esc = True
            elif c == '"':
                instr = False
            continue
        if c == '"':
            instr = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return json.loads(s[i:j + 1])
    raise ValueError("unbalanced JSON")


def parse_sections(pack: bytes):
    if len(pack) < 20 or pack[:8] != b"STELPK01":
        raise ValueError("bad pack magic")
    version, total, nsec = struct.unpack_from("<III", pack, 8)
    if version != 1 or total != len(pack):
        raise ValueError("bad pack header")
    if len(pack) < 20 + nsec * 16:
        raise ValueError("truncated section table")
    secs = {}
    for i in range(nsec):
        typ, off, count, size = struct.unpack_from("<IIII", pack, 20 + i * 16)
        if typ in secs or off > len(pack) or size > len(pack) - off:
            raise ValueError(f"bad section {typ}")
        secs[typ] = (off, count, size)
    for typ in (TEXT, NAME, SCENE, OPS, GALLERY, ASSET, BLOB, META):
        if typ not in secs:
            raise ValueError(f"missing section {typ}")
    return secs


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: stella_audit.py <source-rw5> <pack>", file=sys.stderr)
        return 2
    src = Path(sys.argv[1])
    pack = Path(sys.argv[2]).read_bytes()
    errors: list[str] = []
    counts = {"chunks": 0, "scenes": 0, "text": 0, "choice": 0, "jump": 0, "end": 0,
              "source_refs": 0, "asset_ids": 0, "gallery_groups": 0, "gallery_entries": 0}

    try:
        secs = parse_sections(pack)
    except ValueError as e:
        print(f"FAIL: {e}")
        return 1

    def text(off: int, n: int) -> str:
        t_off, _, t_size = secs[TEXT]
        if off > t_size or n > t_size - off:
            raise ValueError("text range")
        return pack[t_off + off:t_off + off + n].decode("utf-8")

    def name(idx: int) -> str:
        n_off, n_count, n_size = secs[NAME]
        if idx == 65535:
            return ""
        if idx >= n_size // 8:
            raise ValueError(f"name index {idx}")
        off, n, _ = struct.unpack_from("<IHH", pack, n_off + 8 * idx)
        return text(off, n)

    # Asset mapping written into META; no build report dependency.
    m_off, _, m_size = secs[META]
    meta = pack[m_off:m_off + m_size].decode("utf-8")
    asset_ids: dict[str, int] = {}
    assets = extract(src / "src/common/assets.js", "export const assets=")
    primary_ids: dict[str, int] = {}
    alias_ids: dict[str, int] = {}
    for line in meta.splitlines():
        if line.startswith("asset."):
            key, value = line.split("=", 1)
            try:
                idx = int(key[6:])
            except ValueError:
                errors.append(f"bad asset meta key {key}")
                continue
            if value in primary_ids:
                errors.append(f"duplicate primary asset id {value}")
            primary_ids[value] = idx
        elif line.startswith("asset_id."):
            key, value = line.split("=", 1)
            try:
                idx = int(value)
            except ValueError:
                errors.append(f"bad asset alias {line}")
                continue
            if key[9:] in alias_ids:
                errors.append(f"duplicate asset alias {key[9:]}")
            alias_ids[key[9:]] = idx
    a_off, a_count, a_size = secs[ASSET]
    if len(primary_ids) != a_count:
        errors.append(f"primary asset map {len(primary_ids)} != asset table {a_count}")
    asset_ids = alias_ids if alias_ids else primary_ids
    for aid, idx in primary_ids.items():
        if asset_ids.get(aid) != idx:
            errors.append(f"primary asset {aid} missing/wrong alias")
    if len(asset_ids) != len(assets) + 1:
        errors.append(f"asset alias map {len(asset_ids)} != source assets {len(assets)}")
    for aid, idx in asset_ids.items():
        if idx < 0 or idx >= a_count:
            errors.append(f"asset {aid} index {idx} out of range")
    counts["asset_ids"] = len(asset_ids)

    cat = extract(src / "src/common/catalog.js", "export const catalog=")
    gallery = json.loads((src / "src/common/gallery.txt").read_text(encoding="utf-8"))
    if "__title__" not in asset_ids:
        errors.append("missing __title__ asset")
    for aid in assets:
        if aid not in asset_ids:
            errors.append(f"source asset {aid} missing from pack mapping")
    for aid in asset_ids:
        if aid != "__title__" and aid not in assets:
            errors.append(f"pack asset {aid} not in source assets.js")
        if aid in asset_ids and (asset_ids[aid] < 0 or asset_ids[aid] >= a_count):
            errors.append(f"asset {aid} index {asset_ids[aid]} out of range")

    scene_off, scene_count, scene_size = secs[SCENE]
    if scene_size != scene_count * 28:
        errors.append("scene table size/count mismatch")
    scene_ids: set[int] = set()
    next_targets: dict[int, int] = {}
    parsed_scenes: dict[int, list[bytes]] = {}
    for i in range(scene_count):
        sid, route, flags, nm, nxt, first, nops, pad, doff, dlen, raw = \
            struct.unpack_from("<HBBHHIHHIII", pack, scene_off + i * 28)
        if sid in scene_ids:
            errors.append(f"duplicate scene {sid}")
        scene_ids.add(sid)
        next_targets[sid] = nxt
        if nxt != 65535 and str(nxt) not in cat["scenes"]:
            errors.append(f"scene {sid} next {nxt}")
        if nm >= secs[NAME][2] // 8:
            errors.append(f"scene {sid} name index {nm}")
            continue
        if name(nm) != cat["scenes"].get(str(sid), {}).get("name", ""):
            errors.append(f"scene {sid} name mismatch")
        ops_off, _, ops_size = secs[OPS]
        if doff > ops_size or dlen > ops_size - doff or dlen != raw or raw % 24:
            errors.append(f"scene {sid} op range")
            continue
        raw_ops = [pack[ops_off + doff + j * 24: ops_off + doff + (j + 1) * 24]
                   for j in range(dlen // 24)]
        parsed_scenes[sid] = raw_ops

    # Verify scene/route/flags against the source catalog and jump layout.
    for sid, raw_ops in parsed_scenes.items():
        d = src / "src/common/story" / str(sid)
        src_ops: list[dict] = []
        files = 0
        if d.is_dir():
            for fn in sorted(d.iterdir(), key=lambda p: int(p.stem)):
                if fn.suffix == ".txt":
                    files += 1
                    src_ops += json.loads(fn.read_text(encoding="utf-8"))
        counts["chunks"] += files
        if not files:
            errors.append(f"scene {sid} has no source chunk")
        counts["scenes"] += 1
        expected = [o for o in src_ops if o.get("op") != "jumpScene"]
        if len(expected) != len(raw_ops):
            errors.append(f"scene {sid} op count {len(expected)} != {len(raw_ops)}")
        for j, o in enumerate(expected[:len(raw_ops)]):
            op = raw_ops[j]
            typ, bodies, bg, b0, b1, b2, speaker, tx, ln, arg, _ = \
                struct.unpack_from("<BBHHHHHIHIH", op)
            if o.get("op") == "text":
                counts["text"] += 1
                want_bg = asset_ids.get(o.get("background") or "", 65535)
                want_bodies = [asset_ids.get(x, 65535) for x in (o.get("bodies") or [])]
                want_bodies += [65535] * (3 - len(want_bodies))
                if (typ != 0 or text(tx, ln) != (o.get("text") or "") or
                        name(speaker) != (o.get("speaker") or "") or
                        bg != want_bg or list((b0, b1, b2)) != want_bodies or
                        bodies != len(o.get("bodies") or [])):
                    errors.append(f"scene {sid}/{j} text/speaker/image mismatch")
            elif o.get("op") == "choice":
                counts["choice"] += 1
                items = o.get("items") or []
                if len(items) != 1:
                    errors.append(f"scene {sid}/{j} source choice item count")
                elif (typ != 1 or text(tx, ln) != (items[0].get("text") or "") or
                      arg != items[0].get("to")):
                    errors.append(f"scene {sid}/{j} choice mismatch")
                if arg >= len(raw_ops):
                    errors.append(f"scene {sid}/{j} choice target {arg}")
                elif raw_ops[arg][0] not in (0, 1):
                    errors.append(f"scene {sid}/{j} choice target op type")
            elif o.get("op") == "end":
                counts["end"] += 1
                if typ != 2 or text(tx, ln) != cat.get("endings", {}).get(str(sid), "阅读完毕"):
                    errors.append(f"scene {sid}/{j} ending mismatch")
            else:
                errors.append(f"scene {sid}/{j} unknown source op {o.get('op')}")
        jumps = [(j, o) for j, o in enumerate(src_ops) if o.get("op") == "jumpScene"]
        for j, o in jumps:
            counts["jump"] += 1
            if j != len(src_ops) - 1 or o.get("to", 0) != 0 or next_targets.get(sid) != o.get("scene"):
                errors.append(f"scene {sid}/{j} jumpScene/next mismatch")

    # Gallery: source images/labels/order must agree with the pack tables.
    g_off, g_count, g_size = secs[GALLERY]
    group_bytes = g_count * 8
    if group_bytes > g_size or (g_size - group_bytes) % 4:
        errors.append("gallery table size")
    entries = (g_size - group_bytes) // 4
    counts["gallery_groups"] = g_count
    counts["gallery_entries"] = entries
    if g_count != len({x.get("group") or x.get("source") for x in gallery}):
        errors.append("gallery group count mismatch")
    if entries != len(gallery):
        errors.append("gallery entry count mismatch")
    src_group_order: list[str] = []
    src_group_labels: dict[str, str] = {}
    for item in gallery:
        gid = item.get("group") or item.get("source")
        if gid not in src_group_labels:
            src_group_order.append(gid)
            src_group_labels[gid] = item.get("label", gid)
    for i in range(min(g_count, 65535)):
        label, first, count, _ = struct.unpack_from("<HHHH", pack, g_off + i * 8)
        if first > entries or count > entries - first:
            errors.append(f"gallery group {i} range")
        if i < len(src_group_order) and name(label) != src_group_labels[src_group_order[i]]:
            errors.append(f"gallery group {i} label")
    for i, item in enumerate(gallery):
        if i >= entries:
            break
        asset, label, _ = struct.unpack_from("<HHI", pack, g_off + group_bytes + i * 4)
        if asset != asset_ids.get(item.get("image"), 65535):
            errors.append(f"gallery entry {i} asset mapping")
        gid = item.get("group") or item.get("source")
        if name(label) != src_group_labels.get(gid, gid):
            errors.append(f"gallery entry {i} label")

    # All source image references (story + gallery) must resolve in the pack.
    refs = set()
    for sid in cat["scenes"]:
        d = src / "src/common/story" / str(sid)
        if not d.is_dir():
            continue
        for fn in sorted(d.iterdir()):
            if fn.suffix != ".txt":
                continue
            for o in json.loads(fn.read_text(encoding="utf-8")):
                if o.get("op") == "text":
                    if o.get("background"):
                        refs.add(o["background"])
                    refs.update(o.get("bodies") or [])
    refs |= {x["image"] for x in gallery}
    counts["source_refs"] = len(refs)
    for ref in refs:
        if ref not in asset_ids:
            errors.append(f"referenced source image missing: {ref}")

    print(f"chunks={counts['chunks']} scenes={counts['scenes']} text={counts['text']} "
          f"choice={counts['choice']} jump={counts['jump']} end={counts['end']}")
    print(f"asset_ids={counts['asset_ids']} source_refs={counts['source_refs']} "
          f"gallery={counts['gallery_groups']}/{counts['gallery_entries']}")
    for e in errors[:30]:
        print("ERROR:", e)
    if errors:
        print(f"FAIL: {len(errors)} audit errors")
        return 1
    print("PASS: independent source dialogue/asset/gallery audit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
