#!/usr/bin/env python3
"""Build a tiny synthetic pack for the conditional-choice/effect VM tests.

This deliberately uses only conditions/effects that the real sorairo graph does
not contain (conditional choices, all-conditions, + effects, first-match next,
empty-filter fallback), so those exact source semantics are exercised.
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

NONE = 0xFFFFFFFF
OUT = Path(__file__).resolve().parents[1] / "tests" / "data" / "nine_conditions.bin"


class Strings:
    def __init__(self):
        self.data = bytearray(b"\0")
        self.offsets = {"": 0}
        self.chars = set()

    def add(self, value: str) -> int:
        if value not in self.offsets:
            self.offsets[value] = len(self.data)
            self.data.extend(value.encode() + b"\0")
        self.chars.update(value)
        return self.offsets[value]


def p32(*values: int) -> bytes:
    return struct.pack("<" + "I" * len(values), *(x & NONE for x in values))


def main() -> int:
    s = Strings()
    vars_ = ["A", "B", "C", "D"]
    keys = {name: i for i, name in enumerate(vars_)}
    texts = ["条件测试起点。", "条件分支中段。", "全部选项条件都不成立时的回退。"]
    text_block = bytearray()
    frame_text = []
    for text in texts:
        frame_text.append((len(text_block), len(text.encode())))
        text_block.extend(text.encode() + b"\0")
    block = zlib.compress(bytes(text_block), 9)
    block_crc = zlib.crc32(text_block) & NONE
    speaker = s.add("")
    title = s.add("合成条件测试")
    episode = s.add("sorairo")
    sha = s.add("0" * 40)
    chapter_title = s.add("测试章")
    chapter_preview = s.add("条件")

    # Conditions
    conds = [
        (1, keys["A"], 1),                 # 0 A == 1
        (1, keys["A"], 0),                 # 1 A == 0
        (6, keys["B"], 0),                 # 2 B >= 0
        (7, 0, 2),                         # 3 all(1,2)
        (1, keys["B"], 0),                 # 4 B == 0
        (7, 0, 2),                         # 5 all(A==0,B==0)
    ]
    effects = [
        (1, keys["C"], 2),                 # 0 C += 2 (n0, applied once)
        (0, keys["D"], 1),                 # 1 D = 1 (n1)
        (1, keys["C"], 1),                 # 2 C += 1 (hidden option; never chosen)
        (0, keys["B"], 2),                 # 3 B = 2 (visible option)
        (1, keys["C"], 1),                 # 4 C += 1 (visible option)
        (0, keys["D"], 1),                 # 5 complete ending
    ]
    labels = [s.add("被条件隐藏的选项"), s.add("条件成立的选项"), s.add("无条件选项"),
              s.add("回退选项")]
    edges = [
        # 0..1 n0 nexts: first false, second true (first-match)
        (1, 0, 1, 0, 0, 0),
        (1, 1, 3, 0, 0, 0),
        # 2..4 n1 choices
        (2, 0, 1, 2, 1, labels[0]),
        (2, 1, 3, 3, 2, labels[1]),
        (3, 0, 0, 0, 0, labels[2]),
        # 5 n2 next
        (3, 0, 0, 0, 0, 0),
        # 6..8 n3 choices, all conditions false -> fallback offers all
        (4, 0, 1, 0, 0, labels[3]),
        (4, 0, 1, 0, 0, labels[3]),
        (4, 0, 1, 0, 0, labels[3]),
        # 9 n4 next @end
        (NONE, 0, 0, 0, 0, 0),
    ]
    frames = [
        (0, NONE, 0, frame_text[0][0], frame_text[0][1], speaker),
        (0, NONE, 0, frame_text[1][0], frame_text[1][1], speaker),
        (0, NONE, 0, frame_text[2][0], frame_text[2][1], speaker),
    ]
    nodes = [
        # first_frame, frame_count, first_effect, effect_count, first_next,
        # next_count, first_choice, choice_count, title, chapter, name, script
        (0, 1, 0, 1, 0, 2, 0, 0, s.add("n0"), 0, s.add("n0"), s.add("test:*start")),
        (0, 0, 1, 1, 0, 0, 2, 3, s.add("n1"), 0, s.add("n1"), s.add("test:*dummy")),
        (1, 1, 0, 0, 5, 1, 0, 0, s.add("n2"), 0, s.add("n2"), s.add("test:*a")),
        (0, 0, 0, 0, 0, 0, 6, 3, s.add("n3"), 0, s.add("n3"), s.add("test:*fallback")),
        (2, 1, 5, 1, 9, 1, 0, 0, s.add("n4"), 0, s.add("n4"), s.add("test:*end")),
    ]
    chapters = [(chapter_title, 0, chapter_preview)]
    endings = [(s.add("合成结局"), keys["D"], s.add("条件测试结局"))]
    # Header/tables. Offsets are filled in one pass, like the production packer.
    blob = bytearray(256)
    header = [2, 0, 0, episode, title, sha, 0]
    for table, stride in ((nodes, 12), (frames, 6),
                          ([(0, 0, len(text_block), block_crc)], 4),
                          ([], 2), ([], 6),
                          ([(s.add(name), 1 if name == "C" else 0) for name in vars_], 8),
                          (conds, 3), (effects, 3), (edges, 6),
                          (chapters, 3), (endings, 3), ([], 3)):
        header.extend((len(table), len(blob)))
        for row in table:
            blob.extend(p32(*row))
    header.extend((len(blob), len(s.data)))
    blob.extend(s.data)
    block_table = header[12]
    struct.pack_into("<4I", blob, block_table, len(blob), len(block),
                     len(text_block), block_crc)
    blob.extend(block)
    blob[:8] = b"NINESC1\0"
    header[1] = len(blob)
    struct.pack_into("<" + "I" * len(header), blob, 8, *header)
    struct.pack_into("<8I", blob, 140, *([0x11223344] * 8))
    struct.pack_into("<I", blob, 16, zlib.crc32(blob) & NONE)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(blob)
    print(f"wrote {OUT} ({len(blob)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
