#!/usr/bin/env python3
"""Measure the source RINE/phone payload, chapter cards and per-save state.

Read-only: parses the steins0-rw5 snapshot and prints machine-readable budget
data used before extending the pack/runtime.  It does not import the packer.

Usage:
  python tools/steins0_rine_measure.py <steins0-rw5 checkout> \
      --out build/steins0_rine_measure.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
from collections import Counter, OrderedDict
from pathlib import Path

PHONE_DIR = "src/common/phone"
REPLY_DIR = "src/common/reply"
STAMP_DIR = "src/common/phone-stamps"
CARD_DIR = "src/common/chapter-cards"
STORY_DIR = "src/common/story"


def load_json(path: Path):
    return json.loads(path.read_bytes().decode("utf-8"))


def source_text_codepoints(root: Path) -> set[int]:
    cps: set[int] = set()
    for p in (root / STORY_DIR).rglob("*.txt"):
        d = load_json(p)
        for row in d.get("frames", []):
            for cell in (row[0], row[1]):
                cps.update(ord(c) for c in str(cell))
    return cps


class Source:
    def __init__(self, root: Path):
        self.root = root
        self.graph = load_json(root / "src/common/graph.txt")
        self.phone = {}
        for p in sorted((root / PHONE_DIR).glob("*.txt"), key=lambda x: int(x.stem)):
            self.phone[int(p.stem)] = load_json(p)
        self.replies = {}
        for p in sorted((root / REPLY_DIR).glob("*.txt")):
            self.replies[p.stem] = load_json(p)
        self.cards = {}
        for p in sorted((root / CARD_DIR).glob("*.jpg")):
            self.cards[int(p.stem)] = p
        self.stamps = {}
        for p in sorted((root / STAMP_DIR).glob("*.png"), key=lambda x: int(x.stem)):
            self.stamps[int(p.stem)] = p

    def phone_text(self, msg_id: int) -> str:
        group, index = divmod(msg_id, 256)
        rows = self.phone.get(group, [])
        return rows[index] if index < len(rows) else ""


def pua_stats(strings):
    cps = Counter()
    for s in strings:
        cps.update(s)
    pua = {ord(c) for c in cps if 0xE000 <= ord(c) <= 0xF8FF}
    return {
        "codepoints": sum(cps.values()),
        "distinct": len(cps),
        "pua_distinct": len(pua),
        "pua_ids": sorted(cp - 0xE000 for cp in pua),
        "utf8_bytes": sum(len(s.encode("utf-8")) for s in strings),
    }


def simulate_max_messages(source: Source):
    """Walk every graph branch once (no reply choices) and record message life."""
    nodes = source.graph["nodes"]
    totals = {}
    for nid, node in nodes.items():
        totals[nid] = sum(len(load_json(source.root / STORY_DIR / c["file"]).get("frames", []))
                          for c in node["chunks"])

    def total(node):
        return totals[node]

    max_msgs = 0
    max_threads = 0
    paths = 0

    def deliver(state, key, base, outgoing):
        if base in state["replied"] or key in state["msgs"]:
            return
        state["msgs"][key] = (base, outgoing)
        state["order"] += 1

    def events(state, node, cursor):
        for idx, ev in enumerate(nodes[node].get("events", [])):
            if ev["at"] > cursor:
                break
            typ = ev["type"]
            if typ == "closeChoice":
                state["pending"] = None
            elif typ == "rineReset":
                state["windows"].clear()
                if ev.get("scope") != "display":
                    state["msgs"].clear()
                    state["read"].clear()
                    state["replied"].clear()
                    state["order"] = 0
            elif typ == "rineReply":
                state["windows"][ev["base"]] = ev
            elif typ == "expireRine":
                state["windows"].pop(ev["base"], None)
                if ev.get("readMessages") and ev["base"] not in state["replied"]:
                    if any(b == ev["base"] and k in state["read"]
                           for k, (b, _o) in state["msgs"].items()):
                        lower = (ev.get("scope") or "") + ""
                        for i, m in enumerate(ev["readMessages"]):
                            base = m.get("base", (m["id"] // 256) * 256)
                            deliver(state, f"f:{node}:{idx}:{i}", base,
                                    bool(m.get("outgoing")))
            elif typ == "message":
                base = ev.get("base", (ev["id"] // 256) * 256)
                deliver(state, f"s:{node}:{idx}", base, bool(ev.get("outgoing")))

    def walk(node, cursor, endings, state):
        nonlocal max_msgs, paths
        for _ in range(8000):
            events(state, node, cursor)
            if cursor < total(node):
                cursor += 1
                if len(state["msgs"]) > max_msgs:
                    max_msgs = len(state["msgs"])
                continue
            cond = nodes[node].get("condition")
            if cond:
                node = cond["yes"] if endings & 1 else cond["no"]
                cursor = 0
                continue
            choice = nodes[node].get("choice")
            if choice:
                for item in choice["items"]:
                    state2 = {
                        "msgs": dict(state["msgs"]), "read": set(state["read"]),
                        "replied": set(state["replied"]),
                        "windows": dict(state["windows"]), "order": state["order"],
                        "pending": state.get("pending"),
                    }
                    walk(item["to"], 0, endings, state2)
                paths += 1
                return
            if "endingId" in nodes[node]:
                if len(state["msgs"]) > max_msgs:
                    max_msgs = len(state["msgs"])
                return
            return

    for seed in (0, 1):
        walk("n0", 0, seed, {"msgs": {}, "read": set(), "replied": set(),
                             "windows": {}, "order": 0, "pending": None})
    return max_msgs, paths


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--out", default="build/steins0_rine_measure.json")
    args = ap.parse_args()
    root = Path(args.source)
    src = Source(root)
    report = {"source": str(root)}

    # --- phone message texts
    msg_ids = set()
    for node in src.graph["nodes"].values():
        for ev in node.get("events", []):
            if ev.get("type") == "message":
                msg_ids.add(int(ev["id"]))
            if ev.get("type") == "expireRine":
                for m in ev.get("readMessages", []):
                    msg_ids.add(int(m["id"]))
    reply_text_ids = set()
    reply_sequences = 0
    reply_choices = 0
    stamp_ids = set()
    for key, record in src.replies.items():
        for rep in record.get("replies", []):
            reply_choices += 1
            reply_text_ids.add(int(rep["id"]))
            for row in rep.get("sequence", []):
                reply_sequences += 1
                reply_text_ids.add(int(row["id"]))
                if row.get("stamp"):
                    stamp_ids.add(int(row["id"]))
    for ev in (ev for node in src.graph["nodes"].values() for ev in node.get("events", [])):
        if ev.get("type") == "message" and ev.get("stamp"):
            stamp_ids.add(int(ev["id"]))
        for m in ev.get("readMessages", []):
            if m.get("stamp"):
                stamp_ids.add(int(m["id"]))
    all_ids = sorted(msg_ids | reply_text_ids)
    phone_strings = [src.phone_text(i) for i in all_ids]
    phone_strings = [s for s in phone_strings if s]
    unique_phone = sorted(set(phone_strings))
    report["phone_text"] = {
        "message_ids": len(all_ids),
        "nonempty": len(phone_strings),
        "unique": len(unique_phone),
        "unique_utf8_bytes": sum(len(s.encode("utf-8")) for s in unique_phone),
        "total_utf8_bytes": sum(len(s.encode("utf-8")) for s in phone_strings),
        **pua_stats(phone_strings),
    }

    # --- reply records/sequences
    seq_rows = []
    for key, record in src.replies.items():
        for rep in record.get("replies", []):
            for row in rep.get("sequence", []):
                seq_rows.append(row)
    report["reply"] = {
        "files": len(src.replies),
        "choices": reply_choices,
        "sequence_messages": reply_sequences,
        "file_bytes": sum((root / REPLY_DIR / f"{k}.txt").stat().st_size
                          for k in src.replies),
        "choice_text_ids": len(reply_text_ids),
    }

    # --- contacts
    ph = src.graph["phone"]
    contact_strings = []
    for cid in ph.get("contacts", []):
        for table in ("names", "contactNames", "contactNumbers"):
            val = ph.get(table, {}).get(str(cid))
            if val:
                contact_strings.append(val)
    report["contacts"] = {
        "count": len(ph.get("contacts", [])),
        "reply_bases": len(ph.get("replyBases", [])),
        **pua_stats(contact_strings),
    }

    # --- stamps
    stamp_rows = []
    for sid in sorted(stamp_ids):
        p = src.stamps.get(sid)
        if p:
            stamp_rows.append({"id": sid, "bytes": p.stat().st_size})
    report["stamps"] = {
        "referenced": len(stamp_rows),
        "files": len(src.stamps),
        "source_bytes": sum(r["bytes"] for r in stamp_rows),
        "all_source_bytes": sum(p.stat().st_size for p in src.stamps.values()),
    }

    # --- chapter cards
    card_rows = [{"group": g, "bytes": p.stat().st_size} for g, p in src.cards.items()]
    report["chapter_cards"] = {
        "count": len(card_rows),
        "source_bytes": sum(r["bytes"] for r in card_rows),
        "groups": [r["group"] for r in card_rows],
    }

    # --- event/catalog sizing
    events = [ev for node in src.graph["nodes"].values() for ev in node.get("events", [])]
    report["events"] = {
        "total": len(events),
        "by_type": dict(Counter(ev["type"] for ev in events)),
        "message_events": sum(1 for ev in events if ev.get("type") == "message"),
        "stamp_message_events": sum(1 for ev in events
                                    if ev.get("type") == "message" and ev.get("stamp")),
    }
    # Worst-case active messages: no-reply simulation plus all reply sequence
    # messages that can be added by replying (upper bound; window lifetime can
    # only remove, not add, later inbound messages).
    max_msgs, paths = simulate_max_messages(src)
    max_active = max_msgs + max((len(rep.get("sequence", [])) + 1)
                                for record in src.replies.values()
                                for rep in record.get("replies", [])) * len(src.replies)
    # Delivered message catalog: unique delivery keys from events + reply seqs +
    # fallback entries.
    catalog = 0
    for node in src.graph["nodes"].values():
        for ev in node.get("events", []):
            if ev.get("type") == "message":
                catalog += 1
            elif ev.get("type") == "expireRine":
                catalog += len(ev.get("readMessages", []))
    catalog += sum(1 + len(rep.get("sequence", [])) for record in src.replies.values()
                   for rep in record.get("replies", []))
    # Compact per-save RINE state estimate: delta-zigzag(varint) catalog refs +
    # one read bit per ref + replied/window bitmasks + event cursor/pending.
    refs = max_active
    varint_bytes = refs * 2      # conservative: no delta compression assumed
    read_bits = (refs + 7) // 8
    rine_save = 8 + varint_bytes + read_bits + 4 + 4 + 4
    report["state"] = {
        "max_active_messages_no_reply": max_msgs,
        "paths_walked": paths,
        "max_active_upper_with_replies": max_active,
        "catalog_entries": catalog,
        "rine_save_bytes_upper": rine_save,
        "slots": 31,
        "rine_nvs_bytes_31_upper": rine_save * 31,
        "nvs_partition_bytes": 0x6000,
        "core_save_bytes_31": 31 * 64,
    }

    # --- font delta
    story_cps = source_text_codepoints(root)
    new_cps = set()
    for s in unique_phone + contact_strings:
        new_cps.update(ord(c) for c in s)
    missing = {cp for cp in new_cps if cp not in story_cps}
    pua_missing = {cp for cp in missing if 0xE000 <= cp <= 0xF8FF}
    report["font_delta"] = {
        "phone_distinct_codepoints": len(new_cps),
        "not_in_story_text": len(missing),
        "pua_ids_not_in_story": len(pua_missing),
        "pua_ids": sorted(cp - 0xE000 for cp in pua_missing),
        "estimated_4bpp_glyph_bytes": len(pua_missing) * 130,
    }

    # --- pack table estimates
    report["pack_estimate"] = {
        "message_catalog_entries": catalog,
        "catalog_record_bytes": catalog * 16,
        "reply_choice_records": reply_choices * 16,
        "reply_sequence_refs": reply_sequences * 2,
        "phone_text_bytes": sum(len(s.encode("utf-8")) for s in unique_phone),
        "strings_table_bytes": (len(unique_phone) + len(contact_strings) + 16) * 8,
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
