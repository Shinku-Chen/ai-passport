#!/usr/bin/env python3
"""生成图像回归夹具：真实 JPEG + 真实调色板 BLOCK（含透明像素与滤波行）。

固件端的 tjpgd / BLOCK 解码器由 tests/test_anemoi_image.c 直接读这些字节，
在 Canvas 上按真实像素断言；坏图夹具（截断 JPEG、越界块表）也在这里生成。
"""
import io
import json
import os
import struct
import zlib

from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests", "anemoi",
                   "fixtures")


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def make_jpeg():
    im = Image.new("RGB", (32, 16), (240, 20, 20))
    for x in range(16, 32):
        for y in range(16):
            im.putpixel((x, y), (20, 210, 40))
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=95, subsampling=0)
    data = buf.getvalue()
    open(os.path.join(OUT, "jpeg_split.jpg"), "wb").write(data)
    open(os.path.join(OUT, "jpeg_truncated.jpg"), "wb").write(data[:len(data) // 2])
    return len(data)


def make_block():
    # 4x4：调色板 [红, 绿, 蓝(半透明), 透明]，索引矩阵已知
    palette = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 128), (0, 0, 0, 0)]
    indices = [
        [0, 1, 2, 3],
        [1, 2, 3, 0],
        [2, 3, 0, 1],
        [3, 0, 1, 2],
    ]
    rows = 16
    w = h = 4
    header = struct.pack("<HHHHHHI", w, h, len(palette), rows, 1, 0, (w + 1) * rows)
    pal = b"".join(struct.pack("<H", rgb565(*p[:3])) for p in palette)
    alpha = bytes(p[3] for p in palette)
    # 一行 = 1 字节滤波类型 + 4 个索引；滤波 0（none）
    raw = b"".join(bytes([0]) + bytes(row) for row in indices)
    stream = zlib.compress(raw, 9)
    block = header + pal + alpha + struct.pack("<I", len(stream)) + stream
    open(os.path.join(OUT, "block_4x4.bin"), "wb").write(block)
    # 坏块：块长表声明超过载荷
    bad = bytearray(block)
    struct.pack_into("<I", bad, len(header) + len(pal) + len(alpha), 0xFFFF)
    open(os.path.join(OUT, "block_bad_len.bin"), "wb").write(bytes(bad))
    # 坏块：调色板下标越界（索引 9 但只有 4 色）
    raw_bad = b"".join(bytes([0]) + bytes([9, 1, 2, 3]) for _ in range(h))
    stream_bad = zlib.compress(raw_bad, 9)
    bad_idx = header + pal + alpha + struct.pack("<I", len(stream_bad)) + stream_bad
    open(os.path.join(OUT, "block_bad_index.bin"), "wb").write(bad_idx)
    expect = {
        "w": w, "h": h, "rows": rows,
        "palette": [{"r": p[0], "g": p[1], "b": p[2], "a": p[3]} for p in palette],
        "indices": indices,
    }
    open(os.path.join(OUT, "block_4x4.json"), "w", encoding="utf-8").write(
        json.dumps(expect, indent=1) + "\n")
    return len(block)


def make_pack_with_images():
    """把夹具包成最小 ANEMPK1（IMAGE/BLOB 两段），供 C 测试直接打开。"""
    # 用一个完整包做基础不合适（太大）；这里只做最小合法包：24 段，除 IMAGE/BLOB 外全空
    jpeg = open(os.path.join(OUT, "jpeg_split.jpg"), "rb").read()
    block = open(os.path.join(OUT, "block_4x4.bin"), "rb").read()
    bad_jpeg = open(os.path.join(OUT, "jpeg_truncated.jpg"), "rb").read()
    bad_block = open(os.path.join(OUT, "block_bad_len.bin"), "rb").read()
    blob = bytearray()
    images = []

    def add(payload, w, h, kind):
        off = len(blob)
        blob.extend(payload)
        images.append(struct.pack("<IIHHBBH", off, len(payload), w, h, kind, 0, 0))

    add(jpeg, 32, 16, 0)      # 0 jpeg
    add(block, 4, 4, 1)       # 1 block
    add(bad_jpeg, 32, 16, 0)  # 2 截断 jpeg
    add(bad_block, 4, 4, 1)   # 3 坏块
    blob.extend(struct.pack("<H", 0x1234))
    images.append(struct.pack("<IIHHBBH", len(blob) - 2, 2, 0, 0, 2, 0, 0))  # 4 纯色
    # 场景表必须存在（至少一个槽：scene id 1，区间 [0,0)）
    scene = struct.pack("<II", 0, 0)
    scnid = struct.pack("<H", 1)
    text = b"a\x00b\x00"
    txidx = struct.pack("<II", 0, 2)
    payloads = {
        0: text, 1: txidx, 2: b"", 3: scene, 4: scnid, 16: b"".join(images), 17: bytes(blob),
    }
    counts = {0: len(text), 1: 2, 2: 0, 3: 2, 4: 1, 16: 5, 17: len(blob)}
    offset = 28 + 16 * 24
    body = bytearray()
    sections = []
    for sec in range(24):
        data = payloads.get(sec, b"")
        sections.append((sec, offset + len(body), counts.get(sec, 0), len(data)))
        body.extend(data)
    total = offset + len(body)
    head = bytearray(b"ANEMPK1\x00")
    head += struct.pack("<HHHHIII", 1, 28, 24, 0, total, 0, 0)
    for sec, off, cnt, size in sections:
        head += struct.pack("<IIII", sec, off, cnt, size)
    out = bytearray(head) + body
    crc = zlib.crc32(bytes(out[28:])) & 0xFFFFFFFF
    struct.pack_into("<I", out, 20, crc)
    path = os.path.join(OUT, "images_pack.bin")
    open(path, "wb").write(bytes(out))
    return len(out)


def _pack_sections(payloads, counts):
    """把 {section: bytes} + {section: count} 组装成 ANEMPK1。"""
    offset = 28 + 16 * 24
    body = bytearray()
    sections = []
    for sec in range(24):
        data = payloads.get(sec, b"")
        sections.append((sec, offset + len(body), counts.get(sec, 0), len(data)))
        body.extend(data)
    total = offset + len(body)
    head = bytearray(b"ANEMPK1" + bytes([0]))
    head += struct.pack("<HHHHIII", 1, 28, 24, 0, total, 0, 0)
    for sec, off, cnt, size in sections:
        head += struct.pack("<IIII", sec, off, cnt, size)
    out = bytearray(head) + body
    crc = zlib.crc32(bytes(out[28:])) & 0xFFFFFFFF
    struct.pack_into("<I", out, 20, crc)
    return bytes(out)


def build_helper_fixture():
    """最小 ANEMPK1：验证 micro return 的 flags/c 契约、嵌套 helper、字符串/无值返回。

    helpers：
      $$fx_num(p)    -> p + 1            旧格式 return：flags=0、c=1（修复前会被当成 0）
      $$fx_str()     -> "hello"          新格式 return：flags=1、c=1
      $$fx_none()    -> 无值 return       flags=0、c=0 → 0
      $$fx_nested(p) -> $$fx_num(p) * 2   嵌套调用，验证内层值被使用
    ops（scene 900）：set a:0..a:3 = 四个 helper 的结果，然后一句对白 + end。
    """
    texts = ["hi", "hello", "a:0", "a:1", "a:2", "a:3", "cur_call.$x",
             "$$fx_num", "$$fx_str", "$$fx_none", "$$fx_nested",
             "fixture", "900_scene", "p"]
    ids = {t: i for i, t in enumerate(texts)}
    text_blob = b""
    txidx = []
    for t in texts:
        txidx.append(len(text_blob))
        text_blob += t.encode() + bytes([0])
    txidx.append(len(text_blob))
    txidx_blob = b"".join(struct.pack("<I", x) for x in txidx)

    expr = bytearray()

    def e_num(n):
        off = len(expr)
        expr.extend(struct.pack("<Bi", 1, n))
        return off

    def e_var(vid):
        off = len(expr)
        expr.extend(struct.pack("<BH", 3, vid))
        return off

    def e_bin(op, a, b):
        off = len(expr)
        expr.extend(struct.pack("<BB", 6, op))
        expr.extend(_expr_bytes(a))
        expr.extend(_expr_bytes(b))
        return off

    def _expr_bytes(off_in_expr):
        # 子表达式必须内联；调用者传入的是“要重新写一遍”的闭包结果，这里用重放实现
        raise SystemExit("unused")

    # 直接按顺序拼树，避免偏移重写：每个节点构造时先写头，再写子节点。
    def call(name_text_id, arg_count):
        off = len(expr)
        expr.extend(struct.pack("<BHB", 7, name_text_id, 0))
        expr.extend(struct.pack("<B", arg_count))
        return off

    def num(n):
        expr.extend(struct.pack("<Bi", 1, n))

    def var(vid):
        expr.extend(struct.pack("<BH", 3, vid))

    def binop(op):
        expr.extend(struct.pack("<BB", 6, op))

    # CALL 表（每条完整调用表达式）
    calls = bytearray()
    call_offsets = []

    def add_call(body_builder):
        off = len(calls)
        body_builder()
        call_offsets.append(off)

    def c_num():
        call(ids["$$fx_num"], 1)
        num(5)

    def c_str():
        call(ids["$$fx_str"], 0)

    def c_none():
        call(ids["$$fx_none"], 0)

    def c_nested():
        call(ids["$$fx_nested"], 1)
        num(7)

    add_call(c_num)
    add_call(c_str)
    add_call(c_none)
    add_call(c_nested)

    # HELPER 体里的返回值表达式（放在第二个 expr blob 里也行；这里共用一个 EXPR 段）
    ret_num_off = len(expr)
    binop(1)  # '+'
    var(6)    # cur_call.$x
    num(1)
    ret_str_off = len(expr)
    expr.extend(struct.pack("<BH", 2, ids["hello"]))
    ret_nested_off = len(expr)
    binop(3)  # '*'
    call(ids["$$fx_num"], 1)
    var(6)
    num(2)

    micro = bytearray()
    micro_offsets = {}
    def add_helper(name, params, body, legacy_return):
        micro_offsets[name] = len(micro) // 12
        start = len(micro)
        body(legacy_return)
        return len(micro) // 12 - start

    def body_num(legacy):
        micro.extend(struct.pack("<BBHII", 6, 0 if legacy else 1, 0, ret_num_off, 1))

    def body_str(_legacy):
        micro.extend(struct.pack("<BBHII", 6, 1, 0, ret_str_off, 1))

    def body_none(_legacy):
        micro.extend(struct.pack("<BBHII", 6, 0, 0, 0, 0))

    def body_nested(_legacy):
        micro.extend(struct.pack("<BBHII", 6, 1, 0, ret_nested_off, 1))

    helper_rows = bytearray()

    def helper_row(name, params, op_count):
        return struct.pack("<HHIHH", ids[name], ids[params] if params else 0,
                           micro_offsets[name], op_count, 0xFFFF)

    # 注意：micro_count 必须在写完后回填；这里先记录，最后统一 pack
    specs = [("$$fx_num", "p", body_num, True), ("$$fx_str", "", body_str, False),
             ("$$fx_none", "", body_none, False), ("$$fx_nested", "p", body_nested, False)]
    rows = []
    for name, params, body, legacy in specs:
        start = len(micro) // 12
        body(legacy)
        count = len(micro) // 12 - start
        rows.append(struct.pack("<HHIHH", ids[name], ids[params] if params else 0,
                                start, count, 0xFFFF))
    helper_blob = b"".join(rows)

    # SET 表：a:0..a:3 = CALL 表第 0..3 条
    set_rows = []
    for i in range(4):
        set_rows.append(struct.pack("<HHI", i, 0, 0))  # expr 占位，稍后改成 CALL 偏移
    # SET.expr 指向 EXPR 段里的调用表达式；把 CALL 表内容也放进 EXPR 段
    call_expr_offsets = []
    for idx in range(4):
        off = len(expr)
        # 重放调用体（与 CALL 表里的字节完全相同）
        if idx == 0:
            call(ids["$$fx_num"], 1); num(5)
        elif idx == 1:
            call(ids["$$fx_str"], 0)
        elif idx == 2:
            call(ids["$$fx_none"], 0)
        else:
            call(ids["$$fx_nested"], 1); num(7)
        call_expr_offsets.append(off)
    set_blob = b"".join(struct.pack("<HHI", i, 0, call_expr_offsets[i]) for i in range(4))

    ops = bytearray()
    kinds = []

    def op_set(set_index):
        ops.extend(struct.pack("<BBHII", 20, 0, set_index, 0, 0))

    for i in range(4):
        op_set(i)
    ops.extend(struct.pack("<BBHII", 0, 0, 0xFFFF, ids["hi"], 0))  # text
    ops.extend(struct.pack("<BBHII", 16, 0, 0, 0, 0))              # end

    scene = struct.pack("<II", 0, 6)
    scnid = struct.pack("<H", 900)
    entry = struct.pack("<HHI", 900, 0, 0)
    chap = struct.pack("<HHHHI", 900, ids["fixture"], 0, ids["900_scene"], 0)
    var_blob = b"".join(struct.pack("<H", ids[n]) for n in ["a:0", "a:1", "a:2", "a:3",
                                                            "cur_call.$x"])
    meta = b"title=fixture
"
    payloads = {
        0: text_blob, 1: txidx_blob, 2: bytes(ops), 3: scene, 4: scnid, 5: entry,
        6: chap, 7: var_blob, 8: bytes(expr), 9: b"", 10: b"", 11: set_blob,
        16: b"", 17: b"", 19: helper_blob, 20: bytes(micro), 22: b"", 23: meta,
    }
    counts = {0: len(text_blob), 1: len(txidx), 2: len(ops) // 12, 3: 2, 4: 1, 5: 1,
              6: 1, 7: 5, 8: len(expr), 11: 4, 19: 4, 20: len(micro) // 12, 23: len(meta)}
    pack = _pack_sections(payloads, counts)
    path = os.path.join(OUT, "helper_fixture.bin")
    open(path, "wb").write(pack)
    expect = {"a0": 6, "a1": "hello", "a2": 0, "a3": 16}
    open(os.path.join(OUT, "helper_fixture.json"), "w", encoding="utf-8").write(
        json.dumps(expect, indent=1) + "
")
    return len(pack)


def main():
    os.makedirs(OUT, exist_ok=True)
    print("jpeg", make_jpeg())
    print("block", make_block())
    print("pack", make_pack_with_images())
    print("helper fixture", build_helper_fixture())


if __name__ == "__main__":
    main()
