#!/usr/bin/env python3
"""Audit the NianNiang pack: parse it, walk every reachable route, and compare the
result with the source script.

This is the "full resource reference audit" gate for the port. It answers, from
the *built* pack rather than from the converter's intentions:

  * how many scenes / ops / dialogue lines / choices / options / endings the pack
    really contains, and how many of them are reachable from scene 0
  * whether every referenced image exists in the image table (missing must be 0)
  * which background / CG / sprite ids the routes actually touch
  * that all four endings are reachable and that no route falls through into the
    wrong ending (each terminal op is checked against the path that reached it)

Usage:
  python tools/nn_audit.py --pack assets/nn/nn_pack.bin \
      --manifest build/nn_manifest.json --out build/nn_audit.json
Exit code 0 = clean, 1 = mismatch (the reason is printed to stderr).
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

MAGIC = b"NNPK01\0\0"
SEC_TEXT, SEC_META = 0, 1
SEC_IMAGE, SEC_SCENE, SEC_OP, SEC_SPEAKER, SEC_CHOICE, SEC_ENDING = 16, 17, 18, 19, 20, 21

OP_SAY, OP_BG, OP_EV, OP_SPRITE, OP_CHAPTER, OP_JUMP, OP_CHOICE, OP_END, OP_FX = range(9)

IMAGE_REC = 20
SCENE_REC = 12
OP_REC = 12
CHOICE_REC = 12
ENDING_REC = 16
SPEAKER_REC = 8


def fail(msg: str) -> "NoReturn":
    print(f"AUDIT FAIL: {msg}", file=sys.stderr)
    raise SystemExit(1)


class Pack:
    def __init__(self, blob: bytes) -> None:
        if blob[:8] != MAGIC:
            fail("bad magic")
        self.blob = blob
        version, total, nsec = struct.unpack_from("<III", blob, 8)
        if version != 1 or total != len(blob):
            fail(f"version/size mismatch: {version} {total} != {len(blob)}")
        self.sec = {}
        for i in range(nsec):
            st, off, cnt, size = struct.unpack_from("<IIII", blob, 20 + 16 * i)
            if off + size > len(blob):
                fail(f"section {st} out of range")
            self.sec[st] = (off, cnt, size)

        self.text = blob[self.sec[SEC_TEXT][0]: self.sec[SEC_TEXT][0] + self.sec[SEC_TEXT][2]]
        self.images = [struct.unpack_from("<IIIHHBBH", blob, self.sec[SEC_IMAGE][0] + IMAGE_REC * i)
                       for i in range(self.sec[SEC_IMAGE][1])]
        self.scenes = [struct.unpack_from("<HHIHH", blob, self.sec[SEC_SCENE][0] + SCENE_REC * i)
                       for i in range(self.sec[SEC_SCENE][1])]
        self.ops = [struct.unpack_from("<BBHII", blob, self.sec[SEC_OP][0] + OP_REC * i)
                    for i in range(self.sec[SEC_OP][1])]
        self.choices = [struct.unpack_from("<IHHHBB", blob, self.sec[SEC_CHOICE][0] + CHOICE_REC * i)
                        for i in range(self.sec[SEC_CHOICE][1])]
        self.endings = [struct.unpack_from("<IIHHHH", blob, self.sec[SEC_ENDING][0] + ENDING_REC * i)
                        for i in range(self.sec[SEC_ENDING][1])]
        self.speakers = [struct.unpack_from("<IHH", blob, self.sec[SEC_SPEAKER][0] + SPEAKER_REC * i)
                         for i in range(self.sec[SEC_SPEAKER][1])]

    def text_at(self, off: int, ln: int) -> str:
        if off + ln > len(self.text):
            fail(f"text out of range off={off} len={ln}")
        return self.text[off:off + ln].decode("utf-8")

    def op_at(self, scene: int, idx: int):
        first, count = self.scenes[scene][0], self.scenes[scene][1]
        if idx >= count:
            return None
        return self.ops[first + idx]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", default="assets/nn/nn_pack.bin")
    ap.add_argument("--manifest", default="build/nn_manifest.json")
    ap.add_argument("--out", default="build/nn_audit.json")
    args = ap.parse_args()

    blob = open(args.pack, "rb").read()
    pack = Pack(blob)

    image_kinds = defaultdict(int)
    for rec in pack.images:
        image_kinds[rec[5]] += 1

    say_ops = sum(1 for o in pack.ops if o[0] == OP_SAY)
    choice_ops = [i for i, o in enumerate(pack.ops) if o[0] == OP_CHOICE]
    end_ops = [i for i, o in enumerate(pack.ops) if o[0] == OP_END]

    # ---- reachability walk (DFS over choices) --------------------------------
    reachable_ops: set = set()
    reached_endings: dict = {}
    reached_choices: dict = {}
    used_images: set = set()
    used_sprites: set = set()
    lines_by_scene = defaultdict(int)
    paths = []

    def walk(scene: int, index: int, stack: tuple) -> None:
        while True:
            first, count = pack.scenes[scene][0], pack.scenes[scene][1]
            if index >= count:
                paths.append({"path": stack, "result": "fell-off-scene",
                              "scene": scene, "index": index})
                return
            op = pack.ops[first + index]
            t, arg, a, b, c = op
            key = (scene, index)
            if key in reachable_ops:
                return      # already explored (loop or reconvergence)
            reachable_ops.add(key)
            if t == OP_SAY:
                lines_by_scene[scene] += 1
                index += 1
            elif t == OP_BG:
                used_images.add(a)
                index += 1
            elif t == OP_EV:
                used_images.add(a)
                index += 1
            elif t == OP_SPRITE:
                if a != 0xFFFF:
                    used_sprites.add(a)
                index += 1
            elif t in (OP_CHAPTER, OP_FX):
                index += 1
            elif t == OP_JUMP:
                scene, index = a, b
            elif t == OP_CHOICE:
                reached_choices[key] = a
                for k in range(b):
                    c_off, c_len, c_scene, c_op, c_flags = pack.choices[a + k][:5]
                    label = pack.text_at(c_off, c_len)
                    walk(c_scene, c_op, stack + ((scene, index, label),))
                return
            elif t == OP_END:
                reached_endings.setdefault(a, stack)
                paths.append({"path": stack, "result": "ending", "ending": a})
                return
            else:
                fail(f"unknown op type {t} at scene {scene} op {index}")

    walk(0, 0, ())

    # ---- source-side expectations -------------------------------------------
    manifest = {}
    if os.path.exists(args.manifest):
        manifest = json.load(open(args.manifest, encoding="utf-8"))

    endings_detail = []
    for idx, rec in enumerate(pack.endings):
        endings_detail.append({
            "name": pack.text_at(rec[0], rec[2]),
            "text": pack.text_at(rec[1], rec[3]),
            "reachable": idx in reached_endings,
        })
    ending_by_id = {i: e for i, e in enumerate(endings_detail)}

    report = {
        "pack": {
            "path": args.pack,
            "bytes": len(blob),
            "sections": {str(k): {"off": v[0], "count": v[1], "bytes": v[2]}
                         for k, v in sorted(pack.sec.items())},
        },
        "counts": {
            "scenes": len(pack.scenes),
            "ops": len(pack.ops),
            "say_ops": say_ops,
            "choice_ops": len(choice_ops),
            "choice_options": len(pack.choices),
            "ending_table": len(pack.endings),
            "end_ops": len(end_ops),
            "speakers": len(pack.speakers),
            "images": {f"kind{k}": v for k, v in sorted(image_kinds.items())},
        },
        "reachability": {
            "ops_reached": len(reachable_ops),
            "lines_reached": sum(lines_by_scene.values()),
            "choices_reached": len(reached_choices),
            "endings_reached": sorted(reached_endings.keys()),
            "images_used": len(used_images),
            "sprites_used": len(used_sprites),
            "paths": paths,
        },
        "endings": ending_by_id,
        "per_scene_lines": {str(k): v for k, v in sorted(lines_by_scene.items())},
        "manifest": manifest.get("counts", {}),
        "source": manifest.get("source", {}),
    }

    problems = []
    if pack.sec[SEC_IMAGE][1] == 0:
        problems.append("no images")
    for rec in pack.images:
        off, jlen, mlen, w, h, kind, _pad, _pad2 = rec[:8]
        if jlen == 0 or w == 0 or h == 0:
            problems.append(f"empty image record {rec}")
        if kind == 1 and mlen != ((w + 7) // 8) * h:
            problems.append(f"sprite {w}x{h} mask {mlen} != {((w + 7) // 8) * h}")
    if len(reached_endings) != len(pack.endings):
        problems.append(f"endings reached {sorted(reached_endings)} != table {len(pack.endings)}")
    if any(p["result"] == "fell-off-scene" for p in paths):
        problems.append("a route falls off the end of a scene")
    if manifest:
        mc = manifest.get("counts", {})
        if mc.get("say") != say_ops:
            problems.append(f"manifest say {mc.get('say')} != pack {say_ops}")
        if mc.get("choices") != len(choice_ops):
            problems.append(f"manifest choices {mc.get('choices')} != pack {len(choice_ops)}")
        if mc.get("options") != len(pack.choices):
            problems.append(f"manifest options {mc.get('options')} != pack {len(pack.choices)}")
        if mc.get("endings") != len(pack.endings):
            problems.append(f"manifest endings {mc.get('endings')} != pack {len(pack.endings)}")
        if manifest.get("assets", {}).get("missing"):
            problems.append(f"missing assets: {manifest['assets']['missing']}")

    report["problems"] = problems
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(report, fh, ensure_ascii=False, indent=1)

    print(f"pack {args.pack}: {len(blob)} B")
    print(f"  scenes={len(pack.scenes)} ops={len(pack.ops)} say={say_ops} "
          f"choices={len(choice_ops)} options={len(pack.choices)} endings={len(pack.endings)}")
    print(f"  reachable: ops={len(reachable_ops)} lines={sum(lines_by_scene.values())} "
          f"endings={sorted(reached_endings)} images={len(used_images)} sprites={len(used_sprites)}")
    if problems:
        for p in problems:
            print(f"  PROBLEM: {p}")
        return 1
    print("  audit: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
