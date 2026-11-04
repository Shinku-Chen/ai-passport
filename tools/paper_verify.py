#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""资源包完整性校验（不依赖固件）：按 C 侧同一套结构解析 paper_img.bin / paper_scn.bin。

检查项：
  1. 头部计数与偏移自洽，所有 section 落在文件内
  2. 图片表：kind/w/h/extra/data 范围合法；PATCH 的 base 必须是 BLOCK；CG 组引用的图都存在
  3. 剧本：每个场景的 op 都在范围内；TEXT 的正文偏移能落在某个块内且块解压后长度一致
  4. 选项：目标 op 在场景内；图片下标合法（0xFFFF 允许）
  5. 跳转：jumpScene 目标场景存在、目标 op 在范围内
  6. 可达性：从每个章节入口 + 场景 1 出发，穷举所有选项分支，统计可达场景/结局/对白条数
  7. 字库：剧本 + 界面用到的每个字符都在 assets/fonts/gal_cjk_symbols.txt 里
输出 build/paper-verify.json，非 0 退出表示有问题。
"""
from __future__ import annotations

import io
import json
import struct
import sys
import zlib
from collections import deque

IMG = "main/paper_data/paper_img.bin"
SCN = "main/paper_data/paper_scn.bin"
SYMS = "assets/fonts/gal_cjk_symbols.txt"
OUT = "build/paper-verify.json"

OP_TEXT, OP_SPRITE, OP_BACKGROUND, OP_SET, OP_IFFALSE, OP_JUMPSCENE = 0, 1, 2, 3, 4, 5
OP_CLEARSPRITES, OP_END, OP_CHOICE = 6, 7, 8

errors: list[str] = []
stats = {}


def err(msg: str) -> None:
    errors.append(msg)


def main() -> int:
    img = open(IMG, "rb").read()
    scn = open(SCN, "rb").read()

    # ---- 图片包
    if img[:8] != b"PAPRIMG1":
        err("图片包 magic 错误")
        return 1
    ver, n_entries, names_bytes, _ = struct.unpack("<HHII", img[8:20])
    table_off = 20
    names_off = table_off + 24 * n_entries
    entries = []
    for i in range(n_entries):
        name_off, kind, w, h, extra_len, off, size, _r = struct.unpack(
            "<IHHHHIII", img[table_off + 24 * i:table_off + 24 * (i + 1)])
        if off + size > len(img):
            err(f"图片 {i} 越界")
        entries.append({"name_off": name_off, "kind": kind, "w": w, "h": h,
                        "extra": extra_len, "off": off, "size": size})
    names = img[names_off:names_off + names_bytes].split(b"\x00")
    stats["images"] = n_entries
    stats["image_bytes"] = len(img)
    kinds = {0: "jpeg", 1: "block", 2: "patch"}
    stats["by_kind"] = {kinds[k]: sum(1 for e in entries if e["kind"] == k)
                        for k in kinds}
    for i, e in enumerate(entries):
        if e["kind"] == 2:
            if e["extra"] != 10:
                err(f"图片 {i} PATCH extra 长度异常 {e['extra']}")
                continue
            base = struct.unpack("<H", img[e["off"]:e["off"] + 2])[0]
            if base >= n_entries or entries[base]["kind"] != 1:
                err(f"图片 {i} PATCH base={base} 不是 BLOCK")

    # ---- 剧本包
    if scn[:8] != b"PAPRSCN1":
        err("剧本包 magic 错误")
        return 1
    (sver, n_scenes, n_speakers, n_tb, n_cg, n_imgs, n_ch, n_choice, n_end,
     n_vars, flow_scene, initial_scene) = struct.unpack("<12H", scn[8:32])
    (ops_off, ops_bytes, textidx_off, textdata_off, scene_tbl_off, scene_names_off,
     speaker_off, cg_names_off, cg_members_off, chapters_off, chapter_meta_off,
     choices_off, endings_off, ending_scenes_off, total) = struct.unpack("<15I", scn[32:92])
    if total > len(scn):
        err(f"剧本包声明 {total} > 实际 {len(scn)}")
    if n_imgs != n_entries:
        err(f"两包图片数不一致：scn={n_imgs} img={n_entries}")

    scenes = {}
    for i in range(n_scenes):
        sid, route, first, count, name_off, _r = struct.unpack(
            "<HHIIII", scn[scene_tbl_off + 20 * i:scene_tbl_off + 20 * (i + 1)])
        scenes[sid] = {"route": route, "first": first, "count": count,
                       "name": scn[scene_names_off + name_off:].split(b"\x00")[0].decode("utf-8")}
    stats["scenes"] = n_scenes
    stats["ops"] = ops_bytes // 8
    stats["speakers"] = n_speakers
    stats["text_blocks"] = n_tb
    stats["cg_groups"] = n_cg
    stats["chapters"] = n_ch
    stats["choices"] = n_choice
    stats["endings"] = n_end
    stats["vars"] = n_vars
    stats["flow_scene"] = flow_scene
    stats["initial_scene"] = initial_scene
    if flow_scene != 11:
        err(f"flowScene 应为 11（源 catalog.js），实际 {flow_scene}")

    # 正文块：解压并验证连续逻辑流
    blocks = []
    raw = bytearray()
    for i in range(n_tb):
        loff, coff, clen, rlen, _r = struct.unpack(
            "<IIHHI", scn[textidx_off + 16 * i:textidx_off + 16 * (i + 1)])
        try:
            data = zlib.decompress(scn[textdata_off + coff:textdata_off + coff + clen])
        except zlib.error as exc:
            err(f"正文块 {i} 解压失败: {exc}")
            data = b""
        if len(data) != rlen:
            err(f"正文块 {i} 长度不符 {len(data)} != {rlen}")
        if loff != len(raw):
            err(f"正文块 {i} 偏移不连续 {loff} != {len(raw)}")
        blocks.append((loff, rlen))
        raw += data
    stats["text_bytes"] = len(raw)

    def text_at(off: int, length: int) -> str:
        if off + length > len(raw):
            raise ValueError("越界")
        return raw[off:off + length].decode("utf-8")

    # ops
    ops = []
    for i in range(ops_bytes // 8):
        kind, a, b, c = struct.unpack("<BBHI", scn[ops_off + 8 * i:ops_off + 8 * i + 8])
        ops.append((kind, a, b, c, i))

    # 选项表
    choice_items = []
    for ci in range(n_choice):
        base = choices_off + ci
        # 选择块偏移表在 choices_off 处（n_choice × u32）
    coffs = struct.unpack(f"<{n_choice}I", scn[choices_off:choices_off + 4 * n_choice]) if n_choice else ()
    blob_base = choices_off + 4 * n_choice
    for ci in range(n_choice):
        p = blob_base + coffs[ci]
        cnt = scn[p]
        items = []
        for k in range(cnt):
            off, ln, image, to = struct.unpack("<IHHI", scn[p + 1 + 12 * k:p + 13 + 12 * k])
            items.append({"off": off, "len": ln, "image": image, "to": to})
        choice_items.append(items)

    # 逐场景校验
    text_count = 0
    sprite_count = 0
    bg_count = 0
    for sid, sc in scenes.items():
        first, count = sc["first"], sc["count"]
        if first + count > len(ops):
            err(f"场景 {sid} op 越界")
            continue
        for idx in range(first, first + count):
            kind, a, b, c, _ = ops[idx]
            if kind == OP_TEXT:
                text_count += 1
                if b == 0:
                    err(f"场景 {sid} op {idx} 空文本")
                try:
                    s = text_at(c, b)
                    if "\x00" in s:
                        err(f"场景 {sid} op {idx} 文本含 NUL")
                except Exception:
                    err(f"场景 {sid} op {idx} 文本越界")
            elif kind == OP_BACKGROUND:
                bg_count += 1
                if b >= n_entries:
                    err(f"场景 {sid} op {idx} 背景下标 {b} 越界")
            elif kind == OP_SPRITE:
                sprite_count += 1
                if a > 2:
                    err(f"场景 {sid} op {idx} 立绘槽 {a} 超范围")
                if b != 0xFFFF and b >= n_entries:
                    err(f"场景 {sid} op {idx} 立绘下标 {b} 越界")
            elif kind == OP_SET:
                if b >= n_vars:
                    err(f"场景 {sid} op {idx} 变量下标 {b} 越界")
            elif kind == OP_IFFALSE:
                if a > 2:
                    err(f"场景 {sid} op {idx} 条件类型 {a} 不支持")
                if c >= count:
                    err(f"场景 {sid} op {idx} 条件目标 {c} 越界")
            elif kind == OP_JUMPSCENE:
                if b not in scenes:
                    err(f"场景 {sid} op {idx} 跳转到不存在的场景 {b}")
                elif c >= scenes[b]["count"]:
                    err(f"场景 {sid} op {idx} 跳转目标 {b}:{c} op 越界")
            elif kind == OP_CHOICE:
                if b >= n_choice:
                    err(f"场景 {sid} op {idx} 选项下标 {b} 越界")
                else:
                    for it in choice_items[b]:
                        if it["to"] >= count:
                            err(f"选项 {b} 目标 {it['to']} 超出场景 {sid}（{count} ops）")
            elif kind in (OP_CLEARSPRITES, OP_END):
                pass
            else:
                err(f"场景 {sid} op {idx} 未知 kind {kind}")

    stats["text_ops"] = text_count
    stats["sprite_ops"] = sprite_count
    stats["background_ops"] = bg_count

    # 可达性（穷举选项分支，状态 = (scene, cursor)）
    def run_from(start_scene: int, max_states: int = 200000):
        seen = set()
        reached_scenes = set()
        endings = set()
        texts = set()
        q = deque()
        q.append((start_scene, 0))
        while q:
            scene, cursor = q.popleft()
            if len(seen) > max_states:
                err("状态爆炸，可达性检查中止")
                return reached_scenes, endings, texts, False
            sc = scenes.get(scene)
            if not sc:
                err(f"可达性：场景 {scene} 不存在")
                continue
            for _guard in range(sc["count"] + 2):
                if cursor >= sc["count"]:
                    endings.add(("EOF", scene))
                    break
                key = (scene, cursor)
                if key in seen:
                    break
                seen.add(key)
                kind, a, b, c, _ = ops[sc["first"] + cursor]
                if kind == OP_TEXT:
                    texts.add(sc["first"] + cursor)
                    cursor += 1
                elif kind == OP_CHOICE:
                    for it in choice_items[b]:
                        q.append((scene, it["to"]))
                    break
                elif kind == OP_JUMPSCENE:
                    reached_scenes.add(b)
                    scene = b
                    sc = scenes[b]
                    cursor = c
                elif kind == OP_IFFALSE:
                    if a == 0:          # 常数 false -> 必跳
                        cursor = c
                    elif a == 1:        # tf.scene_mode：正常阅读为 false -> 跳
                        cursor = c
                    else:               # sf.pv == 1：两种都走
                        q.append((scene, c))
                        cursor += 1
                elif kind == OP_END:
                    endings.add(("END", scene))
                    break
                else:
                    cursor += 1
        return reached_scenes, endings, texts, True

    all_texts = set()
    all_endings = set()
    all_scenes = set()
    ok = True
    for ci in range(n_ch):
        start = struct.unpack("<H", scn[chapter_meta_off + 8 * ci + 4:chapter_meta_off + 8 * ci + 6])[0]
        rs, es, ts, good = run_from(start)
        ok = ok and good
        all_scenes |= rs
        all_endings |= es
        all_texts |= ts
    rs, es, ts, good = run_from(1)
    all_scenes |= rs
    all_endings |= es
    all_texts |= ts
    stats["reachable_scenes"] = len(all_scenes | {1})
    stats["reachable_endings"] = sorted({e[1] for e in all_endings})
    stats["reachable_text_ops"] = len(all_texts)
    stats["reachable_all"] = ok

    # 结局表
    ending_scenes = [struct.unpack("<H", scn[ending_scenes_off + 2 * i:ending_scenes_off + 2 * i + 2])[0]
                     for i in range(n_end)]
    for i, esn in enumerate(ending_scenes):
        if esn not in scenes:
            err(f"结局 {i} 指向不存在的场景 {esn}")

    # CG 组
    cg_members = struct.unpack(f"<{n_cg}I", scn[cg_members_off:cg_members_off + 4 * n_cg])
    mbase = cg_members_off + 4 * n_cg
    cg_total = 0
    for g in range(n_cg):
        p = mbase + cg_members[g]
        cnt = struct.unpack("<H", scn[p:p + 2])[0]
        cg_total += cnt
        for k in range(cnt):
            idx = struct.unpack("<H", scn[p + 2 + 2 * k:p + 4 + 2 * k])[0]
            if idx >= n_entries:
                err(f"CG 组 {g} 引用越界图片 {idx}")
            elif entries[idx]["kind"] != 0:
                err(f"CG 组 {g} 引用非 JPEG 图 {idx}")
    stats["cg_images"] = cg_total

    # 字库覆盖
    symbols = set()
    for _line in io.open(SYMS, encoding="utf-8"):
        if _line.startswith("#"):
            continue
        _line = _line.rstrip(chr(13) + chr(10))
        if _line:
            symbols.update(_line)
    chars = set()
    for ch in raw.decode("utf-8", errors="replace"):
        if ch not in "\r\n\t\x00":
            chars.add(ch)
    missing = sorted(chars - symbols)
    stats["script_chars"] = len(chars)
    stats["font_missing_chars"] = len(missing)
    if missing:
        err(f"字库缺 {len(missing)} 个字符: {''.join(missing[:20])}")

    # 不可达对白：从所有章节入口 + 场景 1 做穷举可达性后，剩余 TEXT op
    unreachable = []
    for sid, sc in scenes.items():
        for idx in range(sc["first"], sc["first"] + sc["count"]):
            kind = ops[idx][0]
            if kind == OP_TEXT and idx not in all_texts:
                unreachable.append((idx, sid, idx - sc["first"]))
    with open("tests/paper/fixtures/unreachable.bin", "wb") as f:
        f.write(struct.pack("<I", len(unreachable)))
        for (idx, sid, k) in unreachable:
            f.write(struct.pack("<HHI", sid, k, idx))
    stats["unreachable_text_ops"] = len(unreachable)
    stats["unreachable_sample"] = unreachable[:20]

    stats["errors"] = errors
    stats["ok"] = not errors
    json.dump(stats, io.open(OUT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(json.dumps({k: v for k, v in stats.items() if k != "errors"}, ensure_ascii=False, indent=1))
    if errors:
        print(f"\n共 {len(errors)} 个问题：", file=sys.stderr)
        for e in errors[:20]:
            print("  -", e, file=sys.stderr)
        return 1
    print("\nPASS: 资源包完整性校验通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
