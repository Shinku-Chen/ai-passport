#!/usr/bin/env python3
"""Generate a tiny synthetic pack for VM contract tests (not game content).

It exercises two source semantics that the Ep1 graph does not contain:
  * visibleChoices filtering where every option is filtered out -> fall back to
    offering all options;
  * the 500-hop locate limit on a self-loop.
"""
import struct
import zlib
from pathlib import Path

NONE = 0xFFFFFFFF
VERSION = 2


def u32(*v):
    return struct.pack('<' + 'I' * len(v), *(x & NONE for x in v))


def crc(b):
    return zlib.crc32(b) & NONE


def make():
    strings = bytearray(b'\0')
    offsets = {'': 0}

    def s(text):
        if text not in offsets:
            offsets[text] = len(strings)
            strings.extend(text.encode('utf-8') + b'\0')
        return offsets[text]

    nodes = [  # title, chapter, name, source
        (s('filter'), NONE, s('n0'), s('a.txt:*start')),
        (s('choice'), NONE, s('n1'), s('a.txt:*choice')),
        (s('end'), NONE, s('n2'), s('a.txt:*end')),
        (s('loop'), NONE, s('n3'), s('a.txt:*loop')),
    ]
    vars_ = [(s('x'), 0)]
    conds = [(1, 0, 9), (1, 0, 9)]          # x == 9, twice (both impossible)
    effects = [(0, 0, 0)]                    # x = 0 at n0
    edges = [
        (1, 0, 0, 0, 0, s('')),              # n0 next -> n1
        (2, 0, 1, 0, 0, s('A')),             # n1 choice A (filtered out)
        (2, 1, 1, 0, 0, s('B')),             # n1 choice B (filtered out)
        (NONE, 0, 0, 0, 0, s('')),           # n2 next -> end
        (3, 0, 0, 0, 0, s('')),              # n3 next -> n3 (hop limit)
    ]
    node_rows = [
        (0, 0, 0, 1, 0, 1, 0, 0) + nodes[0],
        (0, 0, 0, 0, 1, 0, 1, 2) + nodes[1],
        (0, 0, 0, 0, 3, 1, 0, 0) + nodes[2],
        (0, 0, 0, 0, 4, 1, 0, 0) + nodes[3],
    ]
    blob = bytearray(256)
    header = [VERSION, 0, 0, s('sorairo'), s('synthetic'), s('fixture'), 0]
    tables = [
        (node_rows, 12), ([(0, 0, 0, 0, 0, 0)], 6), ([(0, 0, 1, crc(b''))], 4),
        ([(0, 0)], 2), ([], 6), (vars_, 2), (conds, 3), (effects, 3), (edges, 6),
        ([], 2), ([], 3), ([], 4),
    ]
    table_offsets = []
    for rows, stride in tables:
        if isinstance(rows, int):
            rows = []
        header += [len(rows), len(blob)]
        table_offsets.append(len(blob))
        for row in rows:
            blob.extend(u32(*row))
    header += [len(blob), len(strings)]
    blob.extend(strings)
    # The empty text block is real (one frame is not used): keep it valid.
    struct.pack_into('<4I', blob, table_offsets[2], len(blob), 0, 1, crc(b'\0'))
    blob.extend(b'\0\0\0\0')
    blob[:8] = b'NINESC1\0'
    header[1] = len(blob)
    struct.pack_into('<' + 'I' * len(header), blob, 8, *header)
    struct.pack_into('<I', blob, 16, crc(blob))
    return bytes(blob)


if __name__ == '__main__':
    out = Path(__file__).with_name('filter-fallback.bin')
    out.write_bytes(make())
    print(out)
