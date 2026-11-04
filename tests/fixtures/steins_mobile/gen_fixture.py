#!/usr/bin/env python3
"""从只读源快照生成手机事件机的宿主测试夹具（steins_mobile_fixture.h）。

数据源（只读，绝不修改）：
  <src>/src/common/graph.txt              节点帧数 / 图内 choice
  <src>/src/common/mobile-packs/*.txt     手机事件行（node -> rows）
  <src>/src/common/mobile-windows.js      延迟回复窗口
  <src>/src/common/key-phone-windows.js   关键电话窗口
  <src>/src/common/call-index.js          通话站点 / 选项 / clip
  <src>/src/common/mail-index.js          邮件 type 与后继
  <src>/src/common/mobile-event-map.js    节点 -> 事件文件（只用于断言覆盖）

用法：
  python tests/fixtures/steins_mobile/gen_fixture.py <clannad-like src root> [--out <header>]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# 事件 kind（与 main/steins_mobile.h 的 steins_mob_event_kind_t 同序）
EVENT_KIND = {
    "seed": 0, "receive": 1, "erase": 2, "readBit": 3, "replyBit": 4, "expire": 5,
    "queue": 6, "dispatch": 7, "rule": 8, "call": 9, "presentation": 10,
}
ACTION = {"reply": 1, "send": 2, "call": 3, "compose": 4, "noCall": 5, "noSend": 6,
          "readDeferred": 7, "expireDeferred": 8,
          # 图内 choice 的纯展示动作：'read'/'ignore' 不改变手机事件语义（源 choose 只特殊
          # 处理 noCall/noSend/reply/send，其余照常 incoming + 换节点）。
          "read": 0, "ignore": 0}
PRES = {"draft": 0, "send": 1, "view": 2}   # 与 pack v4 STEINS_PK_* 同值
CALL_KIND = {"phone": 0, "dmail": 1}

# 本夹具覆盖的场景节点（S1..S12 + K1）
EVENT_NODES = ["n0", "n3", "n32", "n246"]
GRAPH_NODES = ["n0", "n3", "n9", "n19", "n32", "n306"]
CLIP_NODES = ["c0"]


def load_js_const(path: Path, name: str):
    """读取 `export const NAME = {...};` / `[...]` —— 用定界符配对，兼容同文件多导出。"""
    text = path.read_text(encoding="utf-8")
    m = re.search(r"export const " + re.escape(name) + r"\s*=\s*", text)
    if not m:
        raise SystemExit(f"cannot find {name} in {path}")
    pos = m.end()
    begin = min(i for i in (text.find("{", pos), text.find("[", pos)) if i >= 0)
    stack: list[str] = []
    in_str = False
    esc = False
    for i in range(begin, len(text)):
        ch = text[i]
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
        elif ch in "{[":
            stack.append("}" if ch == "{" else "]")
        elif ch in "}]":
            if not stack or stack[-1] != ch:
                raise SystemExit(f"bad nesting in {name} at {i}")
            stack.pop()
            if not stack:
                return json.loads(text[begin:i + 1])
    raise SystemExit(f"unbalanced delimiters for {name}")


def c_str(s: str) -> str:
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    src = Path(args.source)
    common = src / "src" / "common"
    out = Path(args.out) if args.out else Path(__file__).with_name("steins_mobile_fixture.h")

    graph = json.loads((common / "graph.txt").read_text(encoding="utf-8"))
    nodes = graph["nodes"]

    # 事件行：node -> [(cursor, [event...])]
    events: dict[str, list] = {}
    for f in sorted((common / "mobile-packs").glob("*.txt")):
        for node, rows in json.loads(f.read_text(encoding="utf-8")).items():
            events[node] = rows

    windows = load_js_const(common / "mobile-windows.js", "mobileWindows")
    keyphones = load_js_const(common / "key-phone-windows.js", "keyPhoneWindows")
    calls = load_js_const(common / "call-index.js", "callIndex")
    mail = load_js_const(common / "mail-index.js", "mailIndex")

    site_keys = sorted(calls.keys())
    site_index = {k: i for i, k in enumerate(site_keys)}

    lines: list[str] = []
    add = lines.append
    add("// 由 tests/fixtures/steins_mobile/gen_fixture.py 从只读源快照生成，请勿手改。")
    add("// 覆盖：mail-index 646 条、事件节点 n0/n3/n32、窗口/关键电话/通话/图内 choice。")
    add("#pragma once")
    add("")
    add("#include <stdint.h>")
    add("")
    add("typedef struct { uint8_t type; uint8_t succ_count; } fx_mail_t;")
    add("typedef struct { uint32_t cursor; uint16_t first; uint8_t count; } fx_row_t;")
    add("typedef struct { uint8_t kind; uint8_t argc; int32_t arg[8]; } fx_event_t;")
    add("typedef struct { uint16_t node; uint16_t id; uint16_t limit; uint16_t skip; uint16_t read;")
    add("                 uint16_t first_opt; uint8_t opt_count; } fx_window_t;")
    add("typedef struct { uint8_t action; uint16_t id; uint16_t to; uint16_t parent; } fx_winopt_t;")
    add("typedef struct { uint16_t node; uint16_t skip; uint16_t go; uint16_t limit;")
    add("                 uint16_t after; uint16_t sent; } fx_keyphone_t;")
    add("typedef struct { uint16_t site; uint16_t limit; uint8_t owner; uint8_t first_item;")
    add("                 uint8_t item_count; } fx_callsite_t;")
    add("typedef struct { uint8_t action; uint16_t clip; uint8_t join; uint16_t id; } fx_callitem_t;")
    add("typedef struct { uint16_t node; uint32_t count; uint16_t choice_id;")
    add("                 uint16_t first_item; uint8_t item_count; } fx_node_t;")
    add("typedef struct { uint16_t to; uint16_t id; uint16_t source; uint8_t action; } fx_choice_item_t;")
    add("typedef struct { uint16_t node; uint32_t frames; } fx_clip_t;")
    add("typedef struct { uint32_t first; uint16_t count; } fx_seed_desc_t;")
    add("typedef struct { uint16_t node; uint32_t cursor; uint8_t event_index; uint16_t id;")
    add("                 uint8_t kind; uint8_t owner; uint16_t key; } fx_pres_t;")
    add("typedef struct { uint16_t chapter; uint32_t row; uint16_t id; uint8_t kind; uint8_t owner; } fx_pres_src_t;")
    add("")

    # ---------------- mail
    add(f"#define FX_MAIL_COUNT {len(mail)}")
    add("static const fx_mail_t FX_MAIL[] = {")
    succ: list[int] = []
    succ_off: list[int] = [0]
    for entry in mail:
        typ = entry[2]
        add("    {%d, %d}," % (typ, len(entry[3])))
        succ.extend(entry[3])
        succ_off.append(len(succ))
    add("};")
    add("static const uint16_t FX_MAIL_SUCC_OFF[] = {%s};" % ", ".join(str(x) for x in succ_off))
    add("static const uint16_t FX_MAIL_SUCC[] = {%s};" % ", ".join(str(x) for x in succ))
    add("")

    # ---------------- events
    seed_ids: list[int] = []
    seed_off: list[int] = [0]
    seed_desc: list[tuple[int, int]] = []
    for node in EVENT_NODES:
        rows = events.get(node)
        if not rows:
            raise SystemExit(f"no events for {node}")
        flat: list[tuple[int, int, int]] = []
        row_tbl: list[tuple[int, int, int]] = []
        for cursor, evs in rows:
            row_cursor = cursor
            first = len(flat)
            for ev in evs:
                kind = EVENT_KIND[ev[0]]
                if ev[0] == "seed":
                    ids = ev[1] if len(ev) > 1 and isinstance(ev[1], list) else []
                    desc_idx = len(seed_desc)
                    seed_desc.append((len(seed_ids), len(ids)))
                    flat.append((kind, 1, (desc_idx, 0, 0, 0, 0, 0, 0, 0)))
                    seed_ids.extend(int(x) for x in ids)
                    seed_off.append(len(seed_ids))
                elif ev[0] == "receive":
                    flat.append((kind, 4, (ev[1], ev[2], 1 if ev[3] else 0, ev[4], 0, 0, 0, 0)))
                elif ev[0] == "erase":
                    flat.append((kind, 2, (ev[1], ev[2], 0, 0, 0, 0, 0, 0)))
                elif ev[0] in ("readBit", "replyBit"):
                    flat.append((kind, 2, (ev[1], 1 if ev[2] else 0, 0, 0, 0, 0, 0, 0)))
                elif ev[0] == "expire":
                    flat.append((kind, 0, (0, 0, 0, 0, 0, 0, 0, 0)))
                elif ev[0] == "queue":
                    flat.append((kind, 2, (ev[1], ev[2], 0, 0, 0, 0, 0, 0)))
                elif ev[0] == "dispatch":
                    flat.append((kind, 1, (ev[1], 0, 0, 0, 0, 0, 0, 0)))
                elif ev[0] == "rule":
                    flat.append((kind, 1, (ev[1], 0, 0, 0, 0, 0, 0, 0)))
                elif ev[0] == "call":
                    # 契约：事件的 items 必须与 callIndex[site].options 的 (action,id) 序列一致。
                    spec = calls[ev[1]]["options"]
                    got = [(it.get("action"), it.get("id")) for it in ev[4]]
                    want = [(it.get("action"), it.get("id")) for it in spec]
                    if got != want:
                        raise SystemExit(f"call items mismatch at {node}@{row_cursor}: {got} != {want}")
                    flat.append((kind, 5, (site_index[ev[1]], ev[2], CALL_KIND[ev[3]],
                                           site_index[ev[1]], 1 if ev[5] else 0,
                                           0, 0, 0)))
                elif ev[0] == "presentation":
                    flat.append((kind, 3, (ev[1], PRES[ev[2]], ev[3], 0, 0, 0, 0, 0)))
                else:
                    raise SystemExit(f"unknown event {ev[0]}")
            row_tbl.append((cursor, first, len(evs)))
        safe = node.replace("-", "_")
        add(f"#define FX_ROWS_{safe}_COUNT {len(row_tbl)}")
        add(f"static const fx_row_t FX_ROWS_{safe}[] = {{")
        for cursor, first, count in row_tbl:
            add(f"    {{{cursor}u, {first}, {count}}},")
        add("};")
        add(f"static const fx_event_t FX_EV_{safe}[] = {{")
        for kind, argc, a in flat:
            add("    {%d, %d, {%d, %d, %d, %d, %d, %d, %d, %d}}," % (
                kind, argc, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]))
        add("};")
        add("")

    add(f"#define FX_SEED_DESC_COUNT {len(seed_desc)}")
    add("static const fx_seed_desc_t FX_SEED_DESC[] = {%s};" % ", ".join(
        "{%d, %d}" % d for d in seed_desc))
    add(f"#define FX_SEED_COUNT {len(seed_ids)}")
    add("static const uint16_t FX_SEED_IDS[] = {%s};" % ", ".join(str(x) for x in seed_ids))
    add("")

    # presentation key 目录（源契约：key = chapter:row:id:kind:owner，按 numeric 五元组排序编号）。
    # 全量 56 条先算出来（含不在本夹具节点集里的），夹具事件只引用其中的全局编号。
    pres_all = []
    for node, rows_ in events.items():
        for cursor, evs in rows_:
            for k, ev in enumerate(evs):
                if ev[0] == "presentation":
                    pres_all.append((int(node[1:]), int(cursor) & 0xFFFFFFFF, int(ev[1]),
                                     PRES[ev[2]], int(ev[3]), node, k))
    pres_all.sort(key=lambda r: (r[0], r[1], r[2], r[3], r[4]))
    pres_index = {(r[5], r[1], r[6]): i for i, r in enumerate(pres_all)}
    add(f"#define FX_PRES_SOURCE_COUNT {len(pres_all)}")
    add("static const fx_pres_src_t FX_PRES_SOURCE[] = {")
    for chapter, row, pid, kind, owner, _node, _k in pres_all:
        add("    {%d, %uu, %d, %d, %d}," % (chapter, row, pid, kind, owner))
    add("};")
    add("")
    add(f"#define FX_PRES_COUNT {len(pres_index)}")
    add("static const fx_pres_t FX_PRES[] = {")
    for node, rows_ in events.items():
        for cursor, evs in rows_:
            for k, ev in enumerate(evs):
                if ev[0] != "presentation":
                    continue
                key = pres_index[(node, int(cursor) & 0xFFFFFFFF, k)]
                add("    {%d, %uu, %d, %d, %d, %d, %d}," % (
                    int(node[1:]), int(cursor) & 0xFFFFFFFF, k, int(ev[1]), PRES[ev[2]],
                    int(ev[3]), key))
    add("};")
    add("")

    # ---------------- windows
    win_list = sorted(windows.items(), key=lambda kv: int(kv[0][1:]))
    winopts: list[tuple] = []
    win_off: dict[str, int] = {}
    add(f"#define FX_WINDOW_COUNT {len(win_list)}")
    add("static const fx_window_t FX_WINDOWS[] = {")
    for node, w in win_list:
        win_off[node] = len(winopts)
        for opt in w.get("options", []):
            winopts.append((ACTION[opt["action"]], opt["id"], int(opt["parent"]), int(opt["to"][1:])))
        add("    {%d, %d, %d, %d, %d, %d, %d}," % (
            int(node[1:]), int(w["id"]), int(w["limit"]), int(w["skip"][1:]),
            int(w["read"][1:]), win_off[node], len(w.get("options", []))))
    add("};")
    add("static const fx_winopt_t FX_WINOPTS[] = {")
    for action, wid, parent, to in winopts:
        add("    {%d, %d, %d, %d}," % (action, wid, to, parent))
    add("};")
    add("")

    # ---------------- keyphones
    kp_list = sorted(keyphones.items(), key=lambda kv: int(kv[0][1:]))
    add(f"#define FX_KEYPHONE_COUNT {len(kp_list)}")
    add("static const fx_keyphone_t FX_KEYPHONES[] = {")
    for node, w in kp_list:
        add("    {%d, %d, %d, %d, %d, %d}," % (
            int(node[1:]), int(w["skip"][1:]), int(w["go"][1:]), int(w["limit"]),
            int(w["after"]), int(w.get("sent", 0xFFFF))))
    add("};")
    add("")

    # ---------------- calls
    callitems: list[tuple] = []
    add(f"#define FX_CALLSITE_COUNT {len(site_keys)}")
    add("static const fx_callsite_t FX_CALLSITES[] = {")
    for i, key in enumerate(site_keys):
        spec = calls[key]
        items = spec["options"]
        add("    {%d, %d, %d, %d, %d}," % (i, int(spec["limit"]), int(spec["owner"]),
                                          len(callitems), len(items)))
        for it in items:
            clip = it.get("clip")
            callitems.append((ACTION[it["action"]],
                              (0x8000 + int(clip[1:]) if clip else 0xFFFF),
                              int(it.get("join", 0)), int(it.get("id", 0xFFFF))))
    add("};")
    add("static const fx_callitem_t FX_CALLITEMS[] = {")
    for action, clip, join, cid in callitems:
        add("    {%d, %d, %d, %d}," % (action, clip, join, cid))
    add("};")
    add("static const char *const FX_CALLSITE_KEYS[] = {%s};" %
        ", ".join(c_str(k) for k in site_keys))
    add("")

    # ---------------- graph nodes + choices
    node_rows: list[tuple] = []
    choice_items: list[tuple] = []
    add(f"#define FX_NODE_COUNT {len(GRAPH_NODES)}")
    add("static const fx_node_t FX_NODES[] = {")
    for name in GRAPH_NODES:
        node = nodes[name]
        count = sum(c["count"] for c in node["chunks"])
        ch = node.get("choice")
        cid = int(ch["id"]) if ch and ch.get("id") is not None else 0xFFFF
        first = len(choice_items)
        if ch:
            for it in ch["items"]:
                src_id = 0xFFFF
                if it.get("action") == "send" and ch.get("source"):
                    # 源 phoneData.dmail[choice.source]：这里直接查表
                    src_id = DMAIL_MAP.get(ch["source"], 0xFFFF)
                choice_items.append((int(it["to"][1:]), int(it.get("id", 0xFFFF)),
                                     src_id, ACTION[it["action"]]))
        add("    {%d, %d, %d, %d, %d}," % (int(name[1:]) if name[0] == "n" else int(name[1:]),
                                          count, cid, first, len(ch["items"]) if ch else 0))
    add("};")
    add("static const fx_choice_item_t FX_CHOICE_ITEMS[] = {")
    for to, cid, src_id, action in choice_items:
        add("    {%d, %d, %d, %d}," % (to, cid, src_id, action))
    add("};")
    add("")

    # ---------------- clips
    clips = load_js_const(common / "call-index.js", "callClips")
    add(f"#define FX_CLIP_COUNT {len(clips)}")
    add("static const fx_clip_t FX_CLIPS[] = {")
    for cid in sorted(clips, key=lambda s: int(s[1:])):
        frames = sum(ch["count"] for ch in clips[cid]["chunks"])
        add("    {0x%04X, %d}," % (0x8000 + int(cid[1:]), frames))
    add("};")
    add("")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    return 0


DMAIL_MAP = {"SG06_23": 233, "SG07_13": 253}


if __name__ == "__main__":
    sys.exit(main())
