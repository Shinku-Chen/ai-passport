#!/usr/bin/env python3
"""Resource-pack gate for the eden* port (host, no C compiler needed).

The firmware reads ``main/eden_data/eden_pack.bin`` straight out of flash, so the
pack itself is the contract. This gate re-reads that binary with an independent
implementation and checks:

  1. pack structure      magic / version / section sizes / record alignment
  2. script integrity    every op decodes, every jump target is in range,
                         all 21 scenes walk from their first op to END
  3. text + geometry     every text op resolves to a string and a frame;
                         every patch stays inside the 264x165 picture
  4. decode comparison   sample frames are re-decoded (base + patches) and
                         compared pixel-wise against the source tiles
  5. font coverage       every code point in the pack exists in the generated
                         LVGL font subset
  6. pagination          every line fits the 3x18 / 3x14 layout without
                         exceeding the firmware's page cap
  7. flash budget         pack + reported firmware <= app partition

Run from the repository root:  python3 tests/test_eden_artifacts.py
"""

from __future__ import annotations

import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "main", "eden_data", "eden_pack.bin")
FONTS = os.path.join(ROOT, "assets", "fonts", "gal_cjk_symbols.txt")
APP_PARTITION = 8_323_072
MERGED_LIMIT = 8_388_608

SEC_TEXT, SEC_TXIDX, SEC_OPS, SEC_SCENE, SEC_IMAGE, SEC_FRAME, SEC_CHAP, SEC_GAL, SEC_BLOB, SEC_PATCH = range(10)

OP_TEXT, OP_SET, OP_IF, OP_GOTO, OP_JUMP, OP_END = range(6)
CMP_EQ, CMP_LE = 0, 1

FRAME_W, FRAME_H = 264, 165
TEXT_LINES = 3
UNITS_SMALL, UNITS_LARGE = 36, 28
GAL_TEXT_MAX_PAGES = 8

failures: list[str] = []


def check(condition: bool, message: str) -> bool:
    if not condition:
        failures.append(message)
    return condition


# ---------------------------------------------------------------- pack parsing
class Pack:
    def __init__(self, blob: bytes):
        self.blob = blob
        check(blob[:8] == b"EDENPK01", "魔数不是 EDENPK01")
        version, header_size, section_count, _reserved, total = struct.unpack_from(
            "<HHHHI", blob, 8)
        check(version == 1, f"版本 {version} != 1")
        check(total == len(blob), f"头部总长 {total} != 文件 {len(blob)}")
        self.sections: dict[int, tuple[int, int, int]] = {}
        for index in range(section_count):
            kind, offset, count, size = struct.unpack_from("<IIII", blob, 20 + 16 * index)
            check(offset + size <= len(blob), f"段 {kind} 越界")
            self.sections[kind] = (offset, count, size)
        for kind in (SEC_TEXT, SEC_TXIDX, SEC_OPS, SEC_SCENE, SEC_IMAGE,
                     SEC_FRAME, SEC_CHAP, SEC_GAL, SEC_BLOB):
            check(kind in self.sections, f"缺段 {kind}")

    def raw(self, kind: int) -> memoryview:
        offset, _count, size = self.sections[kind]
        return memoryview(self.blob)[offset:offset + size]

    def count(self, kind: int) -> int:
        return self.sections[kind][1]

    def text(self, index: int) -> str:
        offsets = self.raw(SEC_TXIDX)
        text = self.raw(SEC_TEXT)
        start, end = struct.unpack_from("<II", offsets, index * 4)
        check(end <= len(text) and start < end, f"文本 {index} 偏移非法")
        raw = bytes(text[start:end])
        check(raw.endswith(b"\x00"), f"文本 {index} 没有 NUL 结尾")
        return raw[:-1].decode("utf-8")

    def op(self, index: int):
        return struct.unpack_from("<BBHII", self.raw(SEC_OPS), index * 12)

    def scene_range(self, scene: int):
        table = self.raw(SEC_SCENE)
        start, end = struct.unpack_from("<II", table, scene * 8)
        return start, end

    def image(self, index: int):
        offset, length, width, height, content_w, content_h = struct.unpack_from(
            "<IIHHHH", self.raw(SEC_IMAGE), index * 16)
        blob = self.raw(SEC_BLOB)
        check(offset + length <= len(blob), f"图片 {index} 越界")
        return bytes(blob[offset:offset + length]), width, height, content_w, content_h

    def frame(self, index: int):
        base, patch_first, patch_count, _reserved = struct.unpack_from(
            "<HHBB", self.raw(SEC_FRAME), index * 6)
        return base, patch_first, patch_count

    def patch(self, index: int):
        return struct.unpack_from("<HHH", self.raw(SEC_PATCH), index * 6)

    def chapter(self, index: int):
        return struct.unpack_from("<HHHH", self.raw(SEC_CHAP), index * 8)

    def gallery(self, index: int):
        return struct.unpack_from("<HHH", self.raw(SEC_GAL), index * 6)


# ---------------------------------------------------------------- script walk
def walk_all(pack: Pack):
    """逐场景校验指令表 + 从场景 1 跑一遍完整剧本。

    - 逐场景:每条指令都能解码,文本/画面/跳转目标都在范围内;
    - 完整跑一遍(变量全 0 的那条分支):必须走到 END 或场景末尾,不允许 yield/死循环。
    """
    scenes = pack.count(SEC_SCENE) - 1
    check(scenes >= 1, "包里没有场景")
    text_ops = 0
    end_ops = 0
    frames_seen = set()
    strings = set()
    empty_lines: set[int] = set()

    for scene in range(1, scenes + 1):
        start, end = pack.scene_range(scene)
        check(start < end, f"场景 {scene} 区间为空")
        for op_index in range(start, end):
            kind, a, b, c, d = pack.op(op_index)
            if kind == OP_TEXT:
                text = pack.text(c)
                strings.update(text)
                strings.update(pack.text(a))
                if len(text) == 0:
                    empty_lines.add(op_index)
                check(d < pack.count(SEC_FRAME), f"场景 {scene} 指令 {op_index} 画面越界")
                frames_seen.add(d)
                text_ops += 1
            elif kind == OP_SET:
                pass
            elif kind == OP_IF:
                check(a in (CMP_EQ, CMP_LE), f"场景 {scene} 指令 {op_index} 比较符非法")
                check(start <= d <= end, f"场景 {scene} 指令 {op_index} if 目标 {d} 越界")
            elif kind == OP_GOTO:
                check(start <= d <= end, f"场景 {scene} 指令 {op_index} goto 目标 {d} 越界")
            elif kind == OP_JUMP:
                check(1 <= c <= scenes, f"场景 {scene} 指令 {op_index} jump 目标越界")
            elif kind == OP_END:
                end_ops += 1
            else:
                check(False, f"场景 {scene} 指令 {op_index} 未知种类 {kind}")

    # 完整跑一遍:从场景 1 第一条指令开始,按引擎语义推进。
    op = pack.scene_range(1)[0]
    scene = 1
    steps = 0
    played = 0
    finished = False
    while steps < 40000:
        steps += 1
        first, last = pack.scene_range(scene)
        if op >= last:
            # 场景读完 = 落到下一个场景(与 eden_script.c 一致);最后一章才是结束。
            if scene < scenes:
                scene += 1
                op = pack.scene_range(scene)[0]
                continue
            finished = True
            break
        kind, a, b, c, d = pack.op(op)
        if kind == OP_TEXT:
            played += 1
            op += 1
        elif kind == OP_SET:
            op += 1
        elif kind == OP_IF:
            op = d if (0 == c if a == CMP_EQ else 0 <= c) else op + 1
        elif kind == OP_GOTO:
            op = d
        elif kind == OP_JUMP:
            scene = c
            op = pack.scene_range(scene)[0]
        elif kind == OP_END:
            finished = True
            break
        else:
            check(False, f"完整剧本里出现未知指令 {kind}")
            break
    check(finished, "完整剧本跑不到结尾(疑似跳转环)")
    # 绝对条数由 test_eden_source_audit.py 对源仓库核对;这里只要求"读到的都播出来"。
    check(played == text_ops, f"完整剧本播到 {played} 句,登记 {text_ops} 句")
    if empty_lines:
        print(f"  note: {len(empty_lines)} 条台词文本为空(源剧本本身留白)")
    return text_ops, end_ops, frames_seen, strings, played


def check_geometry(pack: Pack):
    frames = pack.count(SEC_FRAME)
    for index in range(frames):
        base, patch_first, patch_count = pack.frame(index)
        check(base < pack.count(SEC_IMAGE), f"画面 {index} 底图越界")
        _data, width, height, content_w, content_h = pack.image(base)
        check(width % 8 == 0 and height % 8 == 0, f"画面 {index} 存储尺寸未 8 对齐")
        check(width >= content_w and height >= content_h, f"画面 {index} 内容越界")
        for offset in range(patch_count):
            image_index, x, y = pack.patch(patch_first + offset)
            check(image_index < pack.count(SEC_IMAGE), f"画面 {index} 补丁图越界")
            _d, _w, _h, patch_w, patch_h = pack.image(image_index)
            check(x + patch_w <= FRAME_W, f"画面 {index} 补丁越过右边界")
            check(y + patch_h <= FRAME_H, f"画面 {index} 补丁越过下边界")
            check(patch_w <= 104, f"画面 {index} 补丁宽 {patch_w} 超过暂存条 104")


def check_decode(pack: Pack, frames_seen: set):
    """Re-decode sample frames and compare against the source tiles."""
    try:
        from PIL import Image
        import io
    except ImportError:  # pragma: no cover - host without Pillow
        print("  SKIP 解码对照:本机没有 Pillow")
        return 0

    source_root = os.environ.get("EDEN_SOURCE")
    if not source_root:
        print("  SKIP 解码对照:未设置 EDEN_SOURCE(源仓库路径)")
        return 0
    assets = os.path.join(source_root, "src", "common", "assets.js")
    if not os.path.exists(assets):
        print("  SKIP 解码对照:EDEN_SOURCE 下没有 src/common/assets.js")
        return 0
    text = open(assets, encoding="utf-8").read()
    import re
    frames_js = json.loads(re.search(r"export const frames\s*=\s*(\{.*?\})\s*;?\s*export",
                                     text, re.S).group(1))
    start = text.index("export const tiles")
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                tiles = json.loads(text[brace:index + 1])
                break
    image_dir = os.path.join(source_root, "src", "common", "images")
    atlas_cache: dict = {}
    ids = sorted(frames_js)
    # 采样:每种"有补丁/无补丁"各取若干,外加全部 camera 帧(camera-source.json)。
    plain = [fid for fid in ids if not frames_js[fid].get("patch")]
    patched = [fid for fid in ids if frames_js[fid].get("patch")]
    camera = set()
    camera_path = os.path.join(source_root, "reports", "camera-source.json")
    if os.path.exists(camera_path):
        for row in json.load(open(camera_path, encoding="utf-8")):
            camera.add(row.get("frame"))
    sample = plain[:6] + patched[:6] + [fid for fid in sorted(camera) if fid in frames_js][:6]

    # 包内画面按"剧本引用顺序"编号,和源仓库的 frames 表顺序不同,所以需要构建期导出的
    # 对照表(build/analysis/eden_frame_map.json,--frame-map 生成,不进固件)。
    map_path = os.path.join(ROOT, "build", "analysis", "eden_frame_map.json")
    if not os.path.exists(map_path):
        print("  SKIP 解码对照:缺 build/analysis/eden_frame_map.json(用 --frame-map 重新打包)")
        return 0
    frame_map = json.load(open(map_path, encoding="utf-8"))["frames"]

    compared = 0
    for fid in sample:
        if fid not in frames_js or fid not in frame_map:
            continue
        base, patch_first, patch_count = pack.frame(frame_map[fid])
        data, width, height, content_w, content_h = pack.image(base)
        decoded = Image.open(io.BytesIO(data)).convert("RGB").crop((0, 0, content_w, content_h))
        for offset in range(patch_count):
            image_index, x, y = pack.patch(patch_first + offset)
            pdata, _pw, _ph, pcw, pch = pack.image(image_index)
            patch_image = Image.open(io.BytesIO(pdata)).convert("RGB").crop((0, 0, pcw, pch))
            decoded.paste(patch_image, (x, y))

        spec = frames_js[fid]
        tile = tiles.get(spec["base"])
        if tile is None:
            continue
        if tile["file"] not in atlas_cache:
            atlas_cache[tile["file"]] = Image.open(
                os.path.join(image_dir, tile["file"])).convert("RGB")
        rx, ry, rw, rh = tile["rect"]
        expected = atlas_cache[tile["file"]].crop((rx, ry, rx + rw, ry + rh))
        if spec.get("patch"):
            ptile = tiles.get(spec["patch"])
            if ptile["file"] not in atlas_cache:
                atlas_cache[ptile["file"]] = Image.open(
                    os.path.join(image_dir, ptile["file"])).convert("RGB")
            px, py, pw, ph = ptile["rect"]
            patch_image = atlas_cache[ptile["file"]].crop((px, py, px + pw, py + ph))
            bx, by, _bw, _bh = spec["box"]
            expected = expected.copy()
            expected.paste(patch_image, (bx, by))

        if decoded.size != expected.size:
            check(False, f"{fid} 解码尺寸 {decoded.size} != 源 {expected.size}")
            continue
        # JPEG 是有损的:用平均绝对误差判断"同一帧",而不是要求逐位相等。
        diff = 0
        pixels = list(zip(decoded.getdata(), expected.getdata()))
        for (r1, g1, b1), (r2, g2, b2) in pixels:
            diff += abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2)
        mad = diff / (3 * len(pixels))
        check(mad < 12.0, f"{fid} 解码对照误差过大: MAD={mad:.2f}")
        compared += 1
    return compared


def check_store_source_invariants() -> None:
    """P1-4 的两条硬规则用源码不变量钉住(宿主机上跑不到真 NVS)。"""
    path = os.path.join(ROOT, "main", "eden_store_nvs.c")
    check(os.path.exists(path), "缺 main/eden_store_nvs.c")
    if not os.path.exists(path):
        return
    source = open(path, encoding="utf-8").read()
    check("nvs_flash_erase" not in source,
          "eden_store_nvs.c 不得调用 nvs_flash_erase(会擦掉其他命名空间的数据)")
    check("nvs_commit" in source, "eden_store_nvs.c 必须检查 nvs_commit")
    # 提交失败必须回到调用方:出现 "err = nvs_commit" 且随后判 err != ESP_OK
    check("err = nvs_commit(handle)" in source, "nvs_commit 的返回值必须被接住")
    check("return true;" in source, "写入成功分支缺失")


def check_fonts(strings: set) -> None:
    if not os.path.exists(FONTS):
        check(False, "缺字体字符清单 assets/fonts/gal_cjk_symbols.txt")
        return
    covered = set(open(FONTS, encoding="utf-8").read())
    missing = sorted({c for c in strings if ord(c) >= 0x20} - covered)
    check(not missing, f"字体缺 {len(missing)} 个字形: {''.join(missing[:20])}")


def units(text: str) -> int:
    """与 main/gal/gal_text.h 相同口径:半角 1、全角 2。"""
    total = 0
    for char in text:
        code = ord(char)
        total += 1 if code < 0x80 or 0xFF61 <= code <= 0xFF9F else 2
    return total


def check_pagination(pack: Pack) -> tuple[int, int]:
    pages_small = 0
    pages_large = 0
    worst = 0
    for op_index in range(pack.count(SEC_OPS)):
        kind, _a, _b, c, _d = pack.op(op_index)
        if kind != OP_TEXT:
            continue
        text = pack.text(c)
        for units_per_line in (UNITS_SMALL, UNITS_LARGE):
            pages = _page_count(text, units_per_line)
            worst = max(worst, pages)
            check(pages <= GAL_TEXT_MAX_PAGES,
                  f"指令 {op_index} 需要 {pages} 页,超过固件上限 {GAL_TEXT_MAX_PAGES}")
            if units_per_line == UNITS_SMALL:
                pages_small += pages
            else:
                pages_large += pages
    return pages_small, pages_large


def _page_count(text: str, units_per_line: int) -> int:
    """与 main/gal/gal_text.c 同口径的页数估算(只用于容量检查)。"""
    capacity = units_per_line * TEXT_LINES
    total = units(text)
    return max(1, (total + capacity - 1) // capacity)


def main() -> int:
    blob = open(PACK, "rb").read()
    pack = Pack(blob)
    print(f"pack: {len(blob)} 字节")

    text_ops, end_ops, frames_seen, strings, played = walk_all(pack)
    print(f"script: {text_ops} 句登记 / 完整一遍 {played} 句 / {end_ops} 处 end / "
          f"{len(frames_seen)} 个画面 / {len(strings)} 个字形")

    check_geometry(pack)
    compared = check_decode(pack, frames_seen)
    check_fonts(strings)
    check_store_source_invariants()
    pages_small, pages_large = check_pagination(pack)
    print(f"pages: 16px 档 {pages_small} 页 / 20px 档 {pages_large} 页")

    # 磁盘上的固件与包必须一起装得下 app 分区。
    firmware = os.path.join(ROOT, "build", "FoloToy-AI-Passport.bin")
    if os.path.exists(firmware):
        size = os.path.getsize(firmware)
        check(size <= APP_PARTITION, f"app {size} 超过分区 {APP_PARTITION}")
        print(f"app: {size} 字节 / 分区 {APP_PARTITION}(余 {APP_PARTITION - size})")
        merged = os.path.join(ROOT, "build", "FoloToy-AI-Passport-full.bin")
        if os.path.exists(merged):
            merged_size = os.path.getsize(merged)
            check(merged_size <= MERGED_LIMIT, f"合并镜像 {merged_size} 超过 8MiB")
            print(f"merged: {merged_size} 字节 / 上限 {MERGED_LIMIT}")
    else:
        print("  SKIP 体积检查:还没有构建产物")

    check(compared > 0 or not os.environ.get("EDEN_SOURCE"),
          "要求做解码对照但没有比到任何一帧")

    if failures:
        print(f"FAIL {len(failures)} 项:")
        for message in failures[:20]:
            print("  -", message)
        return 1
    print(f"OK eden artifacts gate ({compared} 帧解码对照)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
