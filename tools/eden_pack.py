#!/usr/bin/env python3
"""Build the eden* resource pack for the AI Passport galgame reader.

The pack is one little-endian binary mapped straight out of flash; the firmware
never parses JSON and never keeps the whole script in RAM.

Input is the source tree of https://github.com/xuanyide01/eden-rw5 (committed
assets + story + reports). Everything is derived from that tree, so no absolute
path from the authoring machine ends up in the repository:

    python tools/eden_pack.py --source <eden-rw5 checkout> \
        --out main/eden_data/eden_pack.bin

Format (all integers little-endian):

  header   "EDENPK01" (8) | version u16 | header_size u16 | section_count u16
           | reserved u16 | total_size u32                          (20 B)
  section  { type u32, offset u32, count u32, size u32 }            (16 B x N)

  SEC_TEXT    UTF-8 strings, NUL terminated, unique, referenced by index
  SEC_TXIDX   u32[N+1]  byte offsets into SEC_TEXT
  SEC_OPS     12 B records: { kind u8, a u8, b u16, c u32, d u32 }
  SEC_SCENE   u32[scenes+1] scene -> op range (scenes are 1-based, index 0 unused)
  SEC_IMAGE   16 B records: { data_off u32, data_len u32, w u16, h u16,
                              cw u16, ch u16 }
              w/h are the stored (8-aligned) dimensions handed to the JPEG
              decoder; cw/ch are the visible content dimensions.
  SEC_FRAME   6 B records: { base u16, patch_first u16, patch_count u8,
                              reserved u8 }
  SEC_PATCH   6 B records: { image u16, x u16, y u16 }, blitted over the base
  SEC_CHAP    8 B records: { scene u16, name u16, op u16, reserved u16 }
  SEC_GAL     6 B records: { frame u16, group u16, label u16 }  (CG gallery)
  SEC_BLOB    payload area (JPEG bytes)

Op kinds (see main/eden_script.h):
  0 TEXT  b=speaker index, c=text index, d=frame index   (a 保留,必须为 0)
  1 SET   b=variable index, c=value
  2 IF    a=compare (0 ==, 1 <=), b=variable index, c=value, d=target op
  3 GOTO  d=target op
  4 JUMP  c=scene id
  5 END

Why JPEG for scene frames: eden's art is photographic; the framework's palette
BLOCK format costs ~1.8x more bytes on the same content (measured on this
source), and the board already ships esp_jpeg (ROM TJpgDec). Patches are JPEG
too, decoded into a narrow scratch strip and blitted, so no second full frame
buffer is ever allocated.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import struct
import sys

from PIL import Image

MAGIC = b"EDENPK01"
# 画面缓冲的内容尺寸(与 main/eden_image.h 的 EDEN_FRAME_W/H 一致)。
CANVAS_W, CANVAS_H = 264, 165
VERSION = 1

SEC_TEXT, SEC_TXIDX, SEC_OPS, SEC_SCENE, SEC_IMAGE, SEC_FRAME, SEC_CHAP, SEC_GAL, SEC_BLOB = range(9)
SEC_PATCH = SEC_BLOB + 1          # appended after the documented sections
SECTION_COUNT = 10

OP_TEXT, OP_SET, OP_IF, OP_GOTO, OP_JUMP, OP_END = range(6)
CMP_EQ, CMP_LE = 0, 1

NO_PATCH = 0xFFFF


def align8(value: int) -> int:
    """Stored images are padded to a multiple of 8 in both axes: TJpgDec emits
    whole MCUs, so the decoder buffer must cover the padded size."""
    return (value + 7) // 8 * 8


def emit_logo(source: str, out_path: str) -> None:
    """Write the title logo as an LVGL RGB565A8 image (colour plane + alpha plane).

    LVGL reads that format natively, so the title screen needs no decoder: the
    array is the draw buffer. 165x66 costs 32,670 B of flash, which is cheaper
    than carrying a PNG/palette decoder for a single image.
    """
    image = Image.open(source).convert("RGBA")
    width, height = image.size
    pixels = list(image.getdata())
    color = bytearray()
    alpha = bytearray()
    for red, green, blue, alpha_value in pixels:
        value = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        color.extend(struct.pack("<H", value))
        alpha.append(alpha_value)
    data = bytes(color) + bytes(alpha)
    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(f"// 由 tools/eden_pack.py 从 {os.path.basename(source)} 生成;不要手改。\n")
        handle.write("// LVGL RGB565A8:色平面 RGB565 小端 + A8 平面,直接作为绘制数据。\n")
        handle.write("#include <stdint.h>\n\n")
        handle.write(f"#define EDEN_LOGO_W {width}\n#define EDEN_LOGO_H {height}\n\n")
        handle.write(f"static const uint8_t eden_logo_data[{len(data)}] = {{\n")
        for index in range(0, len(data), 20):
            chunk = ",".join(f"0x{value:02x}" for value in data[index:index + 20])
            handle.write(f"    {chunk},\n")
        handle.write("};\n")
    log(f"logo: {width}x{height} -> {len(data)} B {out_path}")


def log(msg: str) -> None:
    print(msg, flush=True)


# ---------------------------------------------------------------- source loading
def parse_js_exports(path: str) -> dict:
    """Parse `export const NAME = <json>;` statements out of a .js module."""
    text = open(path, encoding="utf-8").read()
    parts = list(re.finditer(r"export const (\w+)\s*=\s*", text))
    out = {}
    for index, match in enumerate(parts):
        start = match.end()
        end = parts[index + 1].start() if index + 1 < len(parts) else len(text)
        body = text[start:end].rstrip().rstrip(";")
        try:
            out[match.group(1)] = json.loads(body)
        except json.JSONDecodeError:
            continue
    return out


def parse_tiles(path: str) -> dict:
    """`tiles` is the last export of assets.js and is followed by JS helpers."""
    text = open(path, encoding="utf-8").read()
    start = text.index("export const tiles")
    brace = text.index("{", start)
    depth = 0
    end = None
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                end = index + 1
                break
    if end is None:
        raise SystemExit("assets.js: tiles 导出不完整")
    return json.loads(text[brace:end])


class Source:
    def __init__(self, root: str):
        self.root = os.path.abspath(root)
        common = os.path.join(self.root, "src", "common")
        exports = parse_js_exports(os.path.join(common, "assets.js"))
        self.frames = exports["frames"]
        self.tiles = parse_tiles(os.path.join(common, "assets.js"))
        catalog = parse_js_exports(os.path.join(common, "catalog.js"))
        self.catalog = catalog[catalog and "catalog" or "catalog"] if False else json.loads(
            re.search(r"=\s*(\{.*\})\s*;?\s*$",
                      open(os.path.join(common, "catalog.js"), encoding="utf-8").read(),
                      re.S).group(1))
        self.image_dir = os.path.join(common, "images")
        self.story_dir = os.path.join(common, "story")
        # The pack's 295 gallery groups include the pre-dedupe duplicate label
        # `CG · ev_add02_up04` (same frame as `CG · ev_add02_up02`).  The
        # post-dedupe `gallery-source.json` is a review artifact and would
        # silently drop that label; a missing report must fail, never become
        # an empty gallery.
        gallery_path = os.path.join(self.root, "reports",
                                    "gallery-source-before-dedupe.json")
        if not os.path.exists(gallery_path):
            raise SystemExit(
                "missing tracked source report: " + gallery_path +
                " (the packer needs reports/gallery-source-before-dedupe.json)")
        self.gallery = json.load(open(gallery_path, encoding="utf-8"))
        self._atlas_cache = {}

    # -- images ------------------------------------------------------------
    @staticmethod
    def fit_to_canvas(image: Image.Image) -> Image.Image:
        """把非 264x165 的画面等比缩进画幅并黑底居中。

        eden 里有 50 个画面不是 264x165(128x256 立绘、264x198/113 等);
        它们**全部只出现在 CG 鉴赏里**(剧本引用 0 次)。固件的画面缓冲是 264x168,
        装不下 256 高,所以在打包阶段一次性 fit 好,运行时不缩放、不裁切。
        """
        if image.size == (CANVAS_W, CANVAS_H):
            return image
        scale = min(CANVAS_W / image.width, CANVAS_H / image.height)
        scaled = image.resize((max(1, round(image.width * scale)),
                               max(1, round(image.height * scale))), Image.LANCZOS)
        canvas = Image.new("RGB", (CANVAS_W, CANVAS_H))
        canvas.paste(scaled, ((CANVAS_W - scaled.width) // 2,
                              (CANVAS_H - scaled.height) // 2))
        return canvas

    def tile_image(self, name: str) -> Image.Image:
        tile = self.tiles[name]
        atlas = tile["file"]
        if atlas not in self._atlas_cache:
            self._atlas_cache[atlas] = Image.open(
                os.path.join(self.image_dir, atlas)).convert("RGB")
        x, y, w, h = tile["rect"]
        return self._atlas_cache[atlas].crop((x, y, x + w, y + h))

    # -- script ------------------------------------------------------------
    def scenes(self) -> list:
        """每个场景的全部 chunk,按数字序拼接。

        源移植版把长场景拆成 <scene>/0.txt、1.txt …(每块最多 512 条指令);
        引擎在拼接后的整个场景上跑,if/goto 的 to 也是**场景内绝对下标**
        (已核对:双块场景里 1.txt 的 to 全部 > 512,指向第二块内部)。
        只读 0.txt 会静默丢掉 1,030 句对白。
        """
        ids = sorted((int(name) for name in os.listdir(self.story_dir)
                      if os.path.isdir(os.path.join(self.story_dir, name))))
        out = []
        for scene_id in ids:
            directory = os.path.join(self.story_dir, str(scene_id))
            chunks = sorted((name for name in os.listdir(directory)
                             if name.endswith(".txt")),
                            key=lambda name: int(name.split(".", 1)[0]))
            if not chunks:
                raise SystemExit(f"场景 {scene_id} 没有任何 chunk")
            expected = [f"{index}.txt" for index in range(len(chunks))]
            if chunks != expected:
                raise SystemExit(f"场景 {scene_id} 的 chunk 编号不连续: {chunks}")
            ops = []
            for name in chunks:
                ops.extend(json.load(open(os.path.join(directory, name), encoding="utf-8")))
            out.append((scene_id, ops))
        return out

    def catalog_lengths(self) -> dict:
        """catalog.js 里每个场景声明的指令数,用来校验 chunk 拼接是否完整。"""
        return {int(key): int(value["length"])
                for key, value in self.catalog["scenes"].items()}

    def chapter_order(self) -> list:
        """章节顺序(A001..D001):跨场景自然推进按它走,不假设 scene+1。"""
        chapters = self.catalog.get("chapters") or []
        order = [int(chapter["id"]) for chapter in chapters]
        return order if order else sorted(self.catalog_lengths())


# ---------------------------------------------------------------- pack writing
class Section:
    def __init__(self, kind: int):
        self.kind = kind
        self.data = bytearray()
        self.count = 0


def encode_jpeg(image: Image.Image, quality: int) -> bytes:
    pad_w, pad_h = align8(image.width), align8(image.height)
    if (pad_w, pad_h) != image.size:
        padded = Image.new("RGB", (pad_w, pad_h))
        padded.paste(image, (0, 0))
        image = padded
    buf = io.BytesIO()
    image.save(buf, "JPEG", quality=quality, optimize=True, subsampling=2)
    return buf.getvalue()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True, help="eden-rw5 源码树(含 src/common 与 reports)")
    ap.add_argument("--out", required=True, help="输出的资源包路径")
    ap.add_argument("--report", help="覆盖率/体积报告(JSON)输出路径")
    ap.add_argument("--jpeg-quality", type=int, default=25)
    ap.add_argument("--scratch-width", type=int, default=104,
                    help="补丁最大宽度;超过的补丁按列切成多块,决定解码暂存条宽度")
    ap.add_argument("--frame-map", help="画面 id -> 包内下标的对照表(仅构建期测试用,不进固件)")
    ap.add_argument("--logo", help="标题页徽标的源 PNG(RGBA)")
    ap.add_argument("--logo-out", help="标题页徽标的 C 数组输出路径(与 --logo 配对)")
    args = ap.parse_args()

    src = Source(args.source)
    scenes = src.scenes()

    # 完整性自检:拼接后的指令数必须与 catalog.js 声明的 length 一致。
    # 这条断言就是"只读 0.txt 丢掉 1.txt"那个 bug 的守门员(7 个场景共 28 个 chunk)。
    declared = src.catalog_lengths()
    for scene_id, ops in scenes:
        if scene_id in declared and declared[scene_id] != len(ops):
            raise SystemExit(f"场景 {scene_id} 拼接后 {len(ops)} 条指令,catalog 声明 "
                             f"{declared[scene_id]};chunk 缺失或被截断")
    chunk_total = sum(len(ops) for _sid, ops in scenes)
    log(f"scenes: {len(scenes)} 个,指令 {chunk_total} 条,catalog 声明 "
        f"{sum(declared.values())} 条")

    # --- collect what the script actually uses --------------------------
    # Every frame the script shows, plus every frame the CG gallery lists:
    # the gallery must stay complete even when a CG never appears in the
    # common route.
    used_frames = []
    seen = set()
    for _scene_id, ops in scenes:
        for op in ops:
            if op.get("op") != "text":
                continue
            fid = op["background"]
            if fid not in seen:
                seen.add(fid)
                used_frames.append(fid)
    gallery_frames = [item["image"] for item in src.gallery
                      if item.get("image") in src.frames]
    for fid in gallery_frames:
        if fid not in seen:
            seen.add(fid)
            used_frames.append(fid)
    used_frames.sort()

    # --- text pool ------------------------------------------------------
    text_index = {}
    text_list = []

    def intern(value: str) -> int:
        if value not in text_index:
            text_index[value] = len(text_list)
            text_list.append(value)
        return text_index[value]

    # 说话人名与正文共用同一个字符串池:读端统一用 eden_pack_text(index) 取,
    # 分成两套下标会让名牌显示成随机某句台词。
    speakers = {}
    variables = {}

    def intern_into(table: dict, value: str) -> int:
        if value not in table:
            table[value] = len(table)
        return table[value]

    # --- images: one JPEG per source tile, wide patches split into columns ---
    images = []           # list of {payload, content}
    image_index = {}      # deterministic key -> image index
    frames = []           # {base, patch_first, patch_count}
    patches = []          # {image, x, y}
    frame_index = {}

    def add_image(key, image: Image.Image) -> int:
        if key in image_index:
            return image_index[key]
        payload = encode_jpeg(image, args.jpeg_quality)
        index = len(images)
        images.append({"payload": payload, "content": image.size})
        image_index[key] = index
        return index

    fitted = 0
    for fid in used_frames:
        frame = src.frames[fid]
        base_image = src.tile_image(frame["base"])
        if base_image.size != (CANVAS_W, CANVAS_H):
            # 缩小到画幅里:补丁坐标是按原始尺寸记的,有补丁的画面不允许走到这里。
            if frame.get("patch"):
                raise SystemExit(
                    f"{fid} 尺寸 {base_image.size} 不是 {CANVAS_W}x{CANVAS_H} 却带补丁,"
                    "补丁坐标无法直接换算,需要人工确认")
            base_image = Source.fit_to_canvas(base_image)
            fitted += 1
        base_idx = add_image(("tile", frame["base"], "fit" if fitted else None), base_image)
        patch_first = len(patches)
        patch_count = 0
        if frame.get("patch"):
            patch_img = src.tile_image(frame["patch"])
            bx, by, bw, bh = frame["box"]
            # Split wide patches into column chunks so the decoder scratch strip
            # stays narrow (and therefore small in RAM).
            if bw <= args.scratch_width:
                chunks = [(0, bw)]
            else:
                chunks = [(cx, min(args.scratch_width, bw - cx))
                          for cx in range(0, bw, args.scratch_width)]
            for offset, chunk_w in chunks:
                chunk = patch_img.crop((offset, 0, offset + chunk_w, bh))
                index = add_image(("tile", frame["patch"], offset, chunk_w), chunk)
                patches.append({"image": index, "x": bx + offset, "y": by})
                patch_count += 1
        frame_index[fid] = len(frames)
        frames.append({"base": base_idx, "patch_first": patch_first,
                       "patch_count": patch_count})

    # --- script op encoding --------------------------------------------
    ops_records = []
    scene_ranges = []
    for scene_id, ops in scenes:
        start = len(ops_records)
        # 源剧本里的 if.to / goto.to 是"场景内相对下标"(engine.js 用它当 cursor),
        # 而本包的指令是全局连排的,所以这里统一换算成绝对下标。
        for op in ops:
            kind = op.get("op")
            if kind == "text":
                op_id = OP_TEXT
                # a 只有 8 位,放不下 7,560 条的字符串池下标;说话人名放 b(u16)。
                a = 0
                b = intern(op.get("speaker", ""))
                speakers.setdefault(op.get("speaker", ""), b)
                c = intern(op["text"])
                d = frame_index[op["background"]]
            elif kind == "set":
                op_id = OP_SET
                a = 0
                b = intern_into(variables, op["key"])
                c = int(op["value"]) & 0xFFFFFFFF
                d = 0
            elif kind == "if":
                compare = {"==": CMP_EQ, "<=": CMP_LE}.get(op.get("compare"))
                if compare is None:
                    raise SystemExit(f"未知比较符 {op.get('compare')} (scene {scene_id})")
                op_id = OP_IF
                a = compare
                b = intern_into(variables, op["key"])
                c = int(op["value"]) & 0xFFFFFFFF
                d = start + int(op["to"])
            elif kind == "goto":
                op_id = OP_GOTO
                a = b = 0
                c = 0
                d = start + int(op["to"])
            elif kind == "jumpScene":
                op_id = OP_JUMP
                a = b = 0
                c = int(op["scene"])
                d = 0
            elif kind == "end":
                op_id = OP_END
                a = b = c = d = 0
            else:
                raise SystemExit(f"未支持的剧本指令 {kind} (scene {scene_id})")
            ops_records.append(struct.pack("<BBHII", op_id, a & 0xFF, b & 0xFFFF,
                                           c & 0xFFFFFFFF, d & 0xFFFFFFFF))
        scene_ranges.append((scene_id, start, len(ops_records) - start))

    # --- chapters / gallery --------------------------------------------
    chapters = []
    for chapter in src.catalog["chapters"]:
        scene_id = chapter["id"]
        name_idx = intern(chapter["name"])
        op_index = len(ops_records)
        for sid, start, _count in scene_ranges:
            if sid == scene_id:
                op_index = start
                break
        chapters.append({"scene": scene_id, "name": name_idx, "op": op_index})

    gallery = []
    seen_groups = set()
    for item in src.gallery:
        frame_id = item.get("image")
        if frame_id not in frame_index:
            continue
        group = item.get("group", "")
        key = (group, item.get("label", ""))
        if key in seen_groups:
            continue
        seen_groups.add(key)
        gallery.append({"frame": frame_index[frame_id],
                        "group": len(seen_groups) - 1,
                        "label": intern(item.get("label", ""))})

    # --- assemble sections ---------------------------------------------
    sections = [Section(kind) for kind in range(SECTION_COUNT)]
    text = sections[SEC_TEXT]
    text.data.extend(b"")
    offsets = [0]
    for value in text_list:
        text.data.extend(value.encode("utf-8"))
        text.data.append(0)
        offsets.append(len(text.data))
    text.count = len(text_list)
    txidx = sections[SEC_TXIDX]
    for offset in offsets:
        txidx.data.extend(struct.pack("<I", offset))
    txidx.count = len(offsets)

    ops_sec = sections[SEC_OPS]
    for record in ops_records:
        ops_sec.data.extend(record)
    ops_sec.count = len(ops_records)

    scene_sec = sections[SEC_SCENE]
    # Index 0 is a placeholder so that scene ids map straight to record offsets
    # (scenes are 1-based in the source catalog).
    scene_sec.data.extend(struct.pack("<II", 0, 0))
    for section_index in range(1, max(sid for sid, _s, _c in scene_ranges) + 1):
        match = [r for r in scene_ranges if r[0] == section_index]
        if not match:
            raise SystemExit(f"剧本场景 {section_index} 缺失")
        scene_sec.data.extend(struct.pack("<II", match[0][1], match[0][1] + match[0][2]))
    scene_sec.count = len(scene_sec.data) // 8

    image_sec = sections[SEC_IMAGE]
    blob = sections[SEC_BLOB]
    for image in images:
        offset = len(blob.data)
        blob.data.extend(image["payload"])
        pad_w = align8(image["content"][0])
        pad_h = align8(image["content"][1])
        image_sec.data.extend(struct.pack("<IIHHHH", offset, len(image["payload"]),
                                          pad_w, pad_h,
                                          image["content"][0], image["content"][1]))
    image_sec.count = len(images)

    frame_sec = sections[SEC_FRAME]
    for frame in frames:
        frame_sec.data.extend(struct.pack("<HHBB", frame["base"], frame["patch_first"],
                                          frame["patch_count"], 0))
    frame_sec.count = len(frames)

    patch_sec = sections[SEC_PATCH]
    for patch in patches:
        patch_sec.data.extend(struct.pack("<HHH", patch["image"], patch["x"], patch["y"]))
    patch_sec.count = len(patches)

    chap_sec = sections[SEC_CHAP]
    for chapter in chapters:
        chap_sec.data.extend(struct.pack("<HHHH", chapter["scene"], chapter["name"],
                                         chapter["op"], 0))
    chap_sec.count = len(chapters)

    gal_sec = sections[SEC_GAL]
    for item in gallery:
        gal_sec.data.extend(struct.pack("<HHH", item["frame"], item["group"], item["label"]))
    gal_sec.count = len(gallery)

    # --- header + section table ----------------------------------------
    header_size = 20 + SECTION_COUNT * 16
    payload_offset = header_size
    layout = []
    for section in sections:
        aligned = payload_offset + (-payload_offset) % 4
        layout.append((section.kind, aligned, section.count, len(section.data)))
        payload_offset = aligned + len(section.data)
    total = payload_offset + (-payload_offset) % 4

    blob_out = bytearray(total)
    struct.pack_into("<8sHHHHI", blob_out, 0, MAGIC, VERSION, header_size,
                     SECTION_COUNT, 0, total)
    for index, (kind, offset, count, size) in enumerate(layout):
        struct.pack_into("<IIII", blob_out, 20 + index * 16, kind, offset, count, size)
    for section, (_kind, offset, _count, _size) in zip(sections, layout):
        blob_out[offset:offset + len(section.data)] = section.data

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as handle:
        handle.write(blob_out)

    blob_bytes = len(sections[SEC_BLOB].data)
    image_bytes = sum(len(image["payload"]) for image in images)
    report = {
        "source": "eden-rw5",
        "pack": os.path.basename(args.out),
        "total_bytes": total,
        "sections": {name: size for name, size in zip(
            ["text", "txidx", "ops", "scene", "image", "frame", "chap", "gal", "blob",
             "patch"],
            [len(section.data) for section in sections])},
        "scenes": len(scene_ranges),
        "ops": len(ops_records),
        "text_strings": len(text_list),
        "text_bytes": len(sections[SEC_TEXT].data),
        "speakers": len(speakers),
        "variables": len(variables),
        "frames_used": len(used_frames),
        "frame_entries": len(frames),
        "patch_entries": len(patches),
        "images": len(images),
        "jpeg_bytes": image_bytes,
        "chapters": len(chapters),
        "gallery_entries": len(gallery),
        "gallery_groups": len(seen_groups),
        "jpeg_quality": args.jpeg_quality,
        "scratch_width": args.scratch_width,
    }
    if args.report:
        os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
        with open(args.report, "w", encoding="utf-8") as handle:
            json.dump(report, handle, ensure_ascii=False, indent=1)
    if args.frame_map:
        os.makedirs(os.path.dirname(os.path.abspath(args.frame_map)), exist_ok=True)
        with open(args.frame_map, "w", encoding="utf-8") as handle:
            json.dump({"frames": {fid: frame_index[fid] for fid in used_frames}},
                      handle, ensure_ascii=False, separators=(",", ":"))
    if args.logo:
        if not args.logo_out:
            raise SystemExit("--logo 需要同时给 --logo-out")
        emit_logo(args.logo, args.logo_out)
    if fitted:
        log(f"fitted frames: {fitted} 个非 {CANVAS_W}x{CANVAS_H} 画面已等比缩入画幅"
            f"(只出现在 CG 鉴赏里)")
    log(json.dumps(report, ensure_ascii=False, indent=1))
    if blob_bytes != image_bytes:
        raise SystemExit("内部错误: blob 段与图片字节数不一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
