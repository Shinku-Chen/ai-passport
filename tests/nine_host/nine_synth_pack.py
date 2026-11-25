#!/usr/bin/env python3
"""Build tiny NINESC2 packs from tests/nine_host/synth_cases.json.

The same JSON drives tools/nine_trace.cjs (source engine.js trace) and the C
reader test, so both sides see identical graphs and choice policies. Text is
stored in one bounded zlib text block; there are no images or chapters.
"""
from __future__ import annotations
import argparse, json, struct, zlib
from pathlib import Path

NONE = 0xffffffff
COND = {'==': 1, '!=': 2, '<': 3, '<=': 4, '>': 5, '>=': 6}


def p32(*values):
    return struct.pack('<' + 'I' * len(values), *(v & NONE for v in values))


class Strings:
    def __init__(self):
        self.data = bytearray(b'\0')
        self.offsets = {'': 0}

    def add(self, text):
        text = str(text or '')
        if text not in self.offsets:
            self.offsets[text] = len(self.data)
            self.data.extend(text.encode('utf-8') + b'\0')
        return self.offsets[text]


def build(case: dict) -> bytes:
    if 'chain' in case:
        case = dict(case)
        case['nodes'] = {f'n{i}': {'count': 0,
                                   'nexts': [{'to': f'n{i + 1}'}] if i + 1 < case['chain'] else [{'to': '@end'}]}
                         for i in range(case['chain'])}
    names = list(case['nodes'])
    index = {name: i for i, name in enumerate(names)}
    var_names = list(case['vars'])
    var_index = {name: i for i, name in enumerate(var_names)}
    persistent = set(case.get('persistent', []))
    strings = Strings()
    strings.add('haruiro')
    strings.add('synth' + case['name'])
    strings.add('tests/nine_host/synth_cases.json')

    text = bytearray()
    frames = []
    for name in names:
        node = case['nodes'][name]
        for body in node.get('frames', []):
            raw = body.encode('utf-8')
            frames.append((0, NONE, 0, len(text), len(raw), strings.add('')))
            text.extend(raw + b'\0')
    if len(text) > 8192:
        raise ValueError('synthetic text block too large')

    conditions, effects = [], []

    def condition(when):
        if not when:
            return
        if when[0] == 'all':
            for sub in when[1:]:
                condition(sub)
            conditions.append((7, 0, len(when) - 1))
        else:
            conditions.append((COND[when[1]], var_index[when[0]], int(when[2])))

    def effect(items):
        for key, op, value in items:
            effects.append((0 if op == '=' else 1, var_index[key], int(value)))

    edges, node_records = [], []
    cursor = 0
    for name in names:
        node = case['nodes'][name]
        count = node.get('count', len(node.get('frames', [])))
        first_frame = cursor
        cursor += count
        first_effect = len(effects)
        effect(node.get('effects', []))
        for end in node.get('complete', []):
            effect([(end, '=', 1)])
        effect_count = len(effects) - first_effect
        nexts = node.get('nexts', [])
        choices = node.get('choices', [])
        first_next = len(edges)
        for edge in nexts:
            cf = len(conditions)
            condition(edge.get('when'))
            ef = len(effects)
            effect(edge.get('effects', []))
            edges.append((index[edge['to']] if edge['to'] != '@end' else NONE,
                          cf, len(conditions) - cf, ef, len(effects) - ef,
                          strings.add('')))
        first_choice = len(edges)
        for edge in choices:
            cf = len(conditions)
            condition(edge.get('when'))
            ef = len(effects)
            effect(edge.get('effects', []))
            edges.append((index[edge['to']] if edge['to'] != '@end' else NONE,
                          cf, len(conditions) - cf, ef, len(effects) - ef,
                          strings.add(edge['text'])))
        node_records.append((first_frame, count, first_effect, effect_count,
                             first_next, len(nexts), first_choice, len(choices),
                             strings.add(node.get('title', name)), NONE,
                             strings.add(name), strings.add('synth:' + name)))

    variables = [(strings.add(name), 1 if name in persistent else 0) for name in var_names]
    blob = bytearray(256)
    header = [2, 0, 0, strings.add('haruiro'), strings.add('synth' + case['name']),
              strings.add('tests/nine_host/synth_cases.json'), index[case['start']]]
    blocks = [bytes(text)] if text else []
    for table, size in ((node_records, 12), (frames, 6),
                        ([(0, 0, len(text), zlib.crc32(bytes(text)) & NONE)] if text else [], 4),
                        ([], 2), ([], 6), (variables, 2), (conditions, 3),
                        (effects, 3), (edges, 6), ([], 3), ([], 3), ([], 3)):
        header.extend((len(table), len(blob)))
        for record in table:
            blob.extend(p32(*record))
    header.extend((len(blob), len(strings.data)))
    loop_key = var_index.get('loop1', NONE)
    header.extend((loop_key, NONE, 3, 0))
    header.extend([0] * 25)
    blob.extend(strings.data)
    block_offset = header[12]
    if blocks:
        compressed = zlib.compress(blocks[0], 9)
        struct.pack_into('<4I', blob, block_offset, len(blob), len(compressed),
                         len(blocks[0]), zlib.crc32(blocks[0]) & NONE)
        blob.extend(compressed)
    blob[:8] = b'NINESC2\0'
    header[1] = len(blob)
    struct.pack_into('<' + 'I' * len(header), blob, 8, *header)
    struct.pack_into('<I', blob, 16, zlib.crc32(blob) & NONE)
    return bytes(blob)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cases', type=Path, default=Path('tests/nine_host/synth_cases.json'))
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    data = json.loads(args.cases.read_text('utf-8'))
    args.out.mkdir(parents=True, exist_ok=True)
    for case in data['cases']:
        (args.out / (case['name'] + '.bin')).write_bytes(build(case))
        print(case['name'], len(build(case)))


if __name__ == '__main__':
    main()
