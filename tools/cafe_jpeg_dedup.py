"""Lossless JPEG marker-segment dedup for the Cafe Stella pack.

The original JPEG bytes are never re-encoded.  Each JPEG is split into marker
segments and entropy bytes; exact identical marker segments (DQT/DHT/APP/SOF/
SOS/SOI/EOI) go to one shared pool, while entropy and unique bytes stay per
image.  A descriptor reconstructs the original byte stream in order.  The
runtime feeds those fragments directly to TJpgDec; it never builds a full JPEG
copy.
"""
from __future__ import annotations

import struct


def _u16(v: int) -> bytes:
    return struct.pack("<H", v)


def _u32(v: int) -> bytes:
    return struct.pack("<I", v)


def split_jpeg(raw: bytes):
    """Return [(is_header, bytes)] preserving the exact JPEG byte stream."""
    if raw[:2] != b"\xff\xd8":
        raise ValueError("not a JPEG (missing SOI)")
    parts = [(True, raw[:2])]
    pos = 2
    while pos < len(raw):
        if raw[pos] != 0xFF:
            raise ValueError(f"JPEG marker expected at {pos}")
        marker = raw[pos + 1]
        if marker == 0xD9:                 # EOI
            parts.append((True, raw[pos:pos + 2]))
            pos += 2
            break
        if marker in (0x01, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7):
            parts.append((True, raw[pos:pos + 2]))
            pos += 2
            continue
        if pos + 4 > len(raw):
            raise ValueError("truncated JPEG marker length")
        length = struct.unpack_from(">H", raw, pos + 2)[0]
        if length < 2 or pos + 2 + length > len(raw):
            raise ValueError(f"bad JPEG marker length at {pos}")
        if marker == 0xDA:                 # SOS: header, entropy, EOI
            parts.append((True, raw[pos:pos + 2 + length]))
            pos += 2 + length
            eoi = raw.rfind(b"\xff\xd9")
            if eoi < pos:
                raise ValueError("JPEG has no EOI")
            if eoi > pos:
                parts.append((False, raw[pos:eoi]))
            parts.append((True, raw[eoi:eoi + 2]))
            pos = eoi + 2
            if pos != len(raw):
                parts.append((False, raw[pos:]))
            break
        parts.append((True, raw[pos:pos + 2 + length]))
        pos += 2 + length
    if pos != len(raw):
        raise ValueError("JPEG trailing bytes")
    return parts


def dedup_image_records(records, data: bytes, image_count: int):
    """Return (new_data, new_records, pool_section, desc_section).

    Records are dicts from tools/cafe_pack.py with off/len/mask_off/mask_len.
    Image ids must be 0..image_count-1 and in order.
    """
    pool: list[bytes] = []
    pool_index: dict[bytes, int] = {}
    new_data = bytearray()
    new_records = []
    descriptors = []

    for rid, rec in enumerate(records):
        if rec["id"] != rid:
            raise ValueError("image ids must be contiguous")
        raw = data[rec["off"]:rec["off"] + rec["len"]]
        mask = data[rec["mask_off"]:rec["mask_off"] + rec["mask_len"]]
        parts = []
        literal = bytearray()

        def add_literal(chunk: bytes):
            for start in range(0, len(chunk), 0x7FFF):
                piece = chunk[start:start + 0x7FFF]
                if not piece:
                    continue
                parts.append((False, len(piece)))
                literal.extend(piece)

        for is_header, chunk in split_jpeg(raw):
            if is_header:
                if chunk not in pool_index:
                    if len(pool) >= 0x7FFF:
                        raise ValueError("JPEG pool index overflow")
                    pool_index[chunk] = len(pool)
                    pool.append(chunk)
                parts.append((True, pool_index[chunk]))
            else:
                add_literal(chunk)
        if not parts:
            raise ValueError("empty JPEG descriptor")
        off = len(new_data)
        new_data.extend(literal)
        mask_off = len(new_data)
        new_data.extend(mask)
        nr = dict(rec)
        nr["off"] = off
        nr["len"] = len(literal)
        nr["mask_off"] = mask_off
        nr["mask_len"] = len(mask)
        new_records.append(nr)
        descriptors.append(parts)

    if len(records) != image_count:
        raise ValueError("image count mismatch")
    # Shared segment pool: u32 count, u32 absolute offsets[count+1], bytes.
    pool_header = 4 + 4 * (len(pool) + 1)
    offsets = [pool_header]
    pool_bytes = bytearray()
    for segment in pool:
        pool_bytes.extend(segment)
        offsets.append(pool_header + len(pool_bytes))
    pool_section = bytearray(_u32(len(pool)))
    for off in offsets:
        pool_section.extend(_u32(off))
    pool_section.extend(pool_bytes)

    # Descriptor section: u32 count, u32 absolute offsets[count], descriptors.
    table_end = 4 + 4 * image_count
    blob = bytearray()
    desc_offsets = []
    for parts in descriptors:
        desc_offsets.append(table_end + len(blob))
        blob.extend(_u16(len(parts)))
        for is_shared, value in parts:
            if is_shared:
                blob.extend(_u16(value))
            else:
                blob.extend(_u16(value | 0x8000))
    desc_section = bytearray(_u32(image_count))
    for off in desc_offsets:
        desc_section.extend(_u32(off))
    desc_section.extend(blob)
    return bytes(new_data), new_records, bytes(pool_section), bytes(desc_section)
