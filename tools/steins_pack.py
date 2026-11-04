#!/usr/bin/env python3
"""把《命运石之门》REDMI Watch 5 场景重制版(rw5 快应用)源快照打成只读资源包。

源快照是只读输入:工具只读取 src/common 下的 graph.txt / story / images / chapters /
gallery.txt / worldline-data.js / title_bg.png / logo.png,不改动源。输出一份定长表
加数据块的小端二进制包;固件把整包嵌进 Flash,运行时按偏移读表、按偏移取 JPEG
字节流,不解析 JSON、不解压容器、不做文件系统访问。

用法:
  python tools/steins_pack.py --source <steins-rw5 目录> \
      --out assets/steins/steins_pack.bin --manifest assets/steins/steins_pack.json

设计要点(与 main/steins_pack.h 的注释一一对应):
  * 文本按 UTF-16LE 存储:源剧本用 PUA 私有区字形编码,UTF-8 要 3 字节/字形,
    UTF-16 只要 2 字节,运行时再转 UTF-8 交给 LVGL。字串全局去重,帧表只存下标。
  * 帧 = (正文, 说话人, 画面, 日期, 标题)。画面 = 层下标数组;层 = (图, 矩形,
    不透明度)。层去重后单独成表。
  * 图片按“画布上真正会显示的可见区”裁剪后重采样:正文页的画面区上方 180px,
    但底部 30px 被不透明对白带永久遮住,所以只在正文里出现的图可以只留上面
    150/180;鉴赏室里的 CG 保留整幅。裁剪是等比裁(按图像分数),绘制端用同一
    分数算目标矩形,因此构图、人物比例都不变,只是不再为看不见的像素付 Flash。
  * PNG 的 alpha 拆成半分辨率 1bpp 遮罩,颜色走 JPEG;透明区颜色先向不透明区扩散,
    避免 JPEG 在遮罩边界产生黑边。JPEG 质量按字节预算逐张搜索。
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import struct
import sys

try:
    from PIL import Image
    import numpy as np
except ImportError:  # pragma: no cover
    sys.exit("生成资源包需要 Pillow 与 numpy: python -m pip install pillow numpy")


# ---------------------------------------------------------------- 常量

PACK_MAGIC = b"STEINPK3"
PACK_VERSION = 3

# 画布:320x180 是源 432x243 艺术画布的等比缩放(320/432 == 180/243)。
CANVAS_W = 320
CANVAS_H = 180
SCALE = CANVAS_W / 432.0

# 正文页对白带从 y=150 起且不透明;正文里出现的图只需要留到 150。
SCENE_VISIBLE_H = 150

PACK_SCALE = 1.0

SEC = {
    "STRT": 1, "TXT": 2, "NODE": 3, "CHUNKREF": 4, "CHUNK": 5, "FRAME": 6,
    "VIEW": 7, "LAYER": 8, "ASSET": 9, "BLOB": 10, "CHOICE": 11, "CITEM": 12,
    "CHAPTER": 13, "GAL": 14, "GENTRY": 15, "WLINE": 16, "WCHANGE": 17,
    "BREAK": 18, "UI": 19, "META": 20,
}

ACTIONS = ["read", "ignore", "send", "noSend", "call", "noCall", "reply", "dmail"]
ACTION_CODE = {name: i for i, name in enumerate(ACTIONS)}
KINDS = ["mail", "dmail", "phone", "reply"]
KIND_CODE = {name: i for i, name in enumerate(KINDS)}

UI_TEXT = {
    "app.title": "命运石之门",
    "app.subtitle": "STEINS;GATE",
    "title.new": "开始游戏",
    "title.continue": "继续阅读",
    "title.load": "读取存档",
    "title.gallery": "鉴赏",
    "title.chapter": "章节选择",
    "title.config": "设置",
    "title.about": "关于",
    "menu.resume": "继续阅读",
    "menu.save": "保存",
    "menu.load": "读取",
    "menu.history": "历史",
    "menu.auto_on": "自动 开",
    "menu.auto_off": "自动 关",
    "menu.chapter": "跳到下一章",
    "menu.settings": "设置",
    "menu.gallery": "鉴赏",
    "menu.title": "返回标题",
    "save.title": "存档",
    "load.title": "读档",
    "save.empty": "空栏位",
    "save.broken": "存档无法读取",
    "save.overwrite": "覆盖这个存档？",
    "save.done": "已保存",
    "save.fail": "保存失败",
    "load.fail": "读取失败",
    "common.yes": "确定",
    "common.no": "取消",
    "common.back": "返回",
    "common.page": "页",
    "history.title": "历史记录",
    "settings.title": "设置",
    "settings.font": "字号",
    "settings.text_speed": "文字速度",
    "settings.auto_speed": "自动速度",
    "settings.opacity": "对话框浓度",
    "settings.title_menu": "返回标题",
    "chapter.title": "章节选择",
    "chapter.locked": "未到达",
    "gallery.title": "CG 鉴赏",
    "gallery.variant": "差分",
    "gallery.empty": "尚未解锁",
    "ending.title": "结局",
    "ending.reached": "到达结局",
    "ending.true": "真结局",
    "ending.collect": "已收集结局",
    "about.title": "关于",
    "about.body": (
        "本固件是《命运石之门》REDMI Watch 5 场景重制版源剧本的阅读器移植。\n"
        "剧本正文、立绘、背景、CG 与章节标题卡来自该源快照;"
        "界面按 320x240 横屏重新设计。\n"
        "OP 影片、标题机关动画与手表端手机邮件浏览器不在本批移植范围,"
        "选择与结局语义完整保留,详见仓库文档。"
    ),
    "image.fail": "图片读取失败",
    "image.retry": "按确定重试",
    "choice.key": "关键选项",
    "save.autosave": "自动存档",
    "worldline": "世界线",
}


# ---------------------------------------------------------------- 源读取

def load_json(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def load_js_const(path, name):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(name + r"\s*=\s*(\{.*?\});", text, re.S)
    if not m:
        raise SystemExit("cannot find %s in %s" % (name, path))
    return json.loads(m.group(1))


def load_js_string(path, name):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    m = re.search(name + r"\s*=\s*(\"[^\"]*\")", text)
    if not m:
        raise SystemExit("cannot find %s in %s" % (name, path))
    return json.loads(m.group(1))


class Source:
    def __init__(self, root):
        self.root = root
        self.common = os.path.join(root, "src", "common")
        self.graph = load_json(os.path.join(self.common, "graph.txt"))
        assets_js = open(os.path.join(self.common, "assets.js"), encoding="utf-8").read()
        m = re.search(r"const assets=(\{.*?\});", assets_js, re.S)
        if not m:
            raise SystemExit("assets.js: cannot find `const assets=`")
        self.assets = json.loads(m.group(1))
        self.gallery = load_json(os.path.join(self.common, "gallery.txt"))
        wl = os.path.join(self.common, "worldline-data.js")
        self.worldline = load_js_const(wl, "worldlineData")
        self.initial_worldline = load_js_string(wl, "INITIAL_WORLDLINE")
        self.true_worldline = load_js_string(wl, "TRUE_WORLDLINE")

    def image_path(self, asset_id):
        return os.path.join(self.common, "images", self.assets[asset_id])


def parse_rate(text):
    neg = text.startswith("-")
    if neg:
        text = text[1:]
    whole, _, frac = text.partition(".")
    frac = (frac + "000000")[:6]
    value = int(whole) * 1000000 + int(frac)
    return -value if neg else value


# ---------------------------------------------------------------- 字符串池

class Strings:
    def __init__(self):
        self.ids = {}
        self.order = []
        self.utf16 = bytearray()

    def add(self, text):
        key = text if text is not None else ""
        got = self.ids.get(key)
        if got is not None:
            return got
        idx = len(self.order)
        if idx > 0xFFFE:
            raise SystemExit("string pool exhausted (>65534 unique strings)")
        off = len(self.utf16)
        units = key.encode("utf-16-le")
        self.utf16 += units
        self.ids[key] = idx
        self.order.append((off, len(units) // 2))
        return idx


# ---------------------------------------------------------------- 几何

def round_half_up(value):
    return int(value + 0.5)


def layer_rect(layer):
    """源层 -> 画布上的完整显示矩形(未裁剪,与源 object-fit:fill 语义一致)。

    裁剪发生在解码写像素时:矩形可以有负坐标或超出画布,源视口同样会裁掉。
    打包时不能把矩形夹到画布内,否则等于改了 object-fit 的映射。
    """
    _iid, x, y, w, h, _opacity, _prio = layer
    dx = round_half_up(x * SCALE)
    dy = round_half_up(y * SCALE)
    dw = max(1, round_half_up((x + w) * SCALE) - dx)
    dh = max(1, round_half_up((y + h) * SCALE) - dy)
    return (dx, dy, dw, dh)


def visible_frac(rect, band_h):
    """返回该层在画布上的可见分数 (fx0, fy0, fx1, fy1);不可见返回 None。"""
    dx, dy, dw, dh = rect
    x0 = max(0, dx)
    y0 = max(0, dy)
    x1 = min(CANVAS_W, dx + dw)
    y1 = min(band_h, dy + dh)
    if x1 <= x0 or y1 <= y0:
        return None
    return ((x0 - dx) / float(dw), (y0 - dy) / float(dh),
            (x1 - dx) / float(dw), (y1 - dy) / float(dh))


# ---------------------------------------------------------------- 图像

def load_rgba(path):
    im = Image.open(path)
    if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
        return im.convert("RGBA")
    return im.convert("RGB")


def bleed_rgb(rgb, alpha, passes=3):
    """把透明区颜色向不透明区扩散,避免 JPEG 在遮罩边界产生黑边。"""
    out = rgb.copy()
    known = (alpha >= 96)
    if known.all():
        return out
    for _ in range(passes):
        acc = np.zeros(out.shape, np.float32)
        cnt = np.zeros(out.shape[:2], np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            shifted = np.roll(np.roll(out, dy, axis=0), dx, axis=1)
            k = np.roll(np.roll(known, dy, axis=0), dx, axis=1)
            acc += shifted * k[:, :, None]
            cnt += k
        fill = (~known) & (cnt > 0)
        if not fill.any():
            break
        out[fill] = (acc[fill] / cnt[fill][:, None]).astype(np.uint8)
        known = known | fill
    return out


def pack_mask(alpha):
    """alpha -> 半分辨率 1bpp 行打包(MSB 在前),解码端双线性上采样。"""
    h, w = alpha.shape
    mh = (h + 1) // 2
    mw = (w + 1) // 2
    small = np.array(Image.fromarray(alpha).resize((mw, mh), Image.BOX))
    bits = (small >= 112).astype(np.uint8)
    stride = (mw + 7) // 8
    out = np.zeros((mh, stride), np.uint8)
    for x in range(mw):
        out[:, x >> 3] |= bits[:, x] << (7 - (x & 7))
    return out.tobytes(), mw, mh


def encode_jpeg(rgb_image, budget, min_q, max_q):
    best = None
    lo, hi = min_q, max_q
    while lo <= hi:
        q = (lo + hi) // 2
        buf = io.BytesIO()
        rgb_image.save(buf, "JPEG", quality=q, optimize=True, subsampling=2)
        data = buf.getvalue()
        if len(data) <= budget:
            best = (q, data)
            lo = q + 1
        else:
            hi = q - 1
    if best is None:
        buf = io.BytesIO()
        rgb_image.save(buf, "JPEG", quality=min_q, optimize=True, subsampling=2)
        best = (min_q, buf.getvalue())
    return best


# ---------------------------------------------------------------- 资源计划

def plan_assets(src, views, gallery_assets):
    """对每张图求出:可见区(图像分数)、打包尺寸、是否带 alpha。"""
    uses = {}
    for view in views:
        for layer in view:
            rect = layer_rect(layer)
            frac = visible_frac(rect, SCENE_VISIBLE_H)
            if frac is None:
                continue
            uses.setdefault(layer[0], []).append((rect, frac))
    # 鉴赏室的图要整幅可见
    for aid in gallery_assets:
        uses.setdefault(aid, []).append(((0, 0, CANVAS_W, CANVAS_H), (0.0, 0.0, 1.0, 1.0)))

    plan = {}
    for iid, items in uses.items():
        path = src.image_path(iid)
        if not os.path.exists(path):
            raise SystemExit("missing image for %s: %s" % (iid, path))
        fx0 = min(f[0] for _r, f in items)
        fy0 = min(f[1] for _r, f in items)
        fx1 = max(f[2] for _r, f in items)
        fy1 = max(f[3] for _r, f in items)
        with Image.open(path) as im:
            ow, oh = im.size
            has_alpha = im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info)
        # 裁到像素网格,并留 1px 余量避免边界抖动
        cx = max(0, int(fx0 * ow) - 1)
        cy = max(0, int(fy0 * oh) - 1)
        cw = min(ow - cx, int(round(fx1 * ow)) - cx + 2)
        ch = min(oh - cy, int(round(fy1 * oh)) - cy + 2)
        cx = min(cx, ow - cw)
        cy = min(cy, oh - ch)
        # 需要的显示尺寸 = 各次使用中裁剪区的最大屏幕高度
        need_w = max(r[2] * (f[2] - f[0]) for r, f in items)
        need_h = max(r[3] * (f[3] - f[1]) for r, f in items)
        sc = min(1.0, need_w / float(cw), need_h / float(ch)) * PACK_SCALE
        pw = max(1, min(cw, round_half_up(cw * sc)))
        ph = max(1, min(ch, round_half_up(ch * sc)))
        plan[iid] = {"path": path, "orig": (ow, oh), "crop": (cx, cy, cw, ch),
                     "pack": (pw, ph), "alpha": has_alpha}
    return plan


# ---------------------------------------------------------------- 主流程

def build(args):
    src = Source(args.source)
    graph = src.graph
    if graph.get("format") != 2:
        raise SystemExit("unsupported graph format %r" % (graph.get("format"),))

    node_ids = list(graph["nodes"])
    node_index = {nid: i for i, nid in enumerate(node_ids)}

    # ---- chunk(按文件去重,节点只存引用序列)
    chunk_key_to_idx = {}
    chunk_files = []
    node_chunks = {}
    for nid in node_ids:
        refs = []
        for part in graph["nodes"][nid]["chunks"]:
            key = (part["file"], part["count"])
            idx = chunk_key_to_idx.get(key)
            if idx is None:
                blob = load_json(os.path.join(src.common, "story", part["file"]))
                frames = blob["frames"]
                views = blob["views"]
                if part["count"] != len(frames):
                    raise SystemExit("chunk %s: count %d != frames %d"
                                     % (part["file"], part["count"], len(frames)))
                idx = len(chunk_files)
                chunk_key_to_idx[key] = idx
                chunk_files.append((part["file"], frames, views))
            refs.append(idx)
        node_chunks[nid] = refs

    # ---- 画面/层去重
    view_ids = {}
    views = []
    layer_ids = {}
    layers = []
    chunk_view_idx = []
    for _file, _frames, vlist in chunk_files:
        local = []
        for v in vlist:
            key = json.dumps(v, separators=(",", ":"))
            vid = view_ids.get(key)
            if vid is None:
                idxs = []
                for layer in v:
                    lkey = json.dumps(layer, separators=(",", ":"))
                    lid = layer_ids.get(lkey)
                    if lid is None:
                        lid = len(layers)
                        layer_ids[lkey] = lid
                        layers.append(layer)
                    idxs.append(lid)
                vid = len(views)
                view_ids[key] = vid
                views.append(idxs)
            local.append(vid)
        chunk_view_idx.append(local)

    # ---- 字符串/帧
    pool = Strings()
    frame_rows = []
    chunk_first = []
    chunk_count = []
    for ci, (_file, frames, _v) in enumerate(chunk_files):
        chunk_first.append(len(frame_rows))
        chunk_count.append(len(frames))
        for row in frames:
            if len(row) < 5:
                raise SystemExit("frame with %d fields in chunk %d" % (len(row), ci))
            frame_rows.append((pool.add(row[1]), pool.add(row[0]),
                               chunk_view_idx[ci][row[2]],
                               pool.add(row[3] or ""), pool.add(row[4] or "")))

    # ---- 节点/选项
    nodes = []
    chunkref = []
    breaks = []
    choices = []
    citems = []
    for nid in node_ids:
        node = graph["nodes"][nid]
        total = sum(chunk_count[c] for c in node_chunks[nid])
        if total != node["count"]:
            raise SystemExit("node %s: count %d != chunks %d" % (nid, node["count"], total))
        chunk_first_idx = len(chunkref)
        chunkref.extend(node_chunks[nid])
        brk = [int(x[0]) for x in node.get("breaks") or []]
        break_first = len(breaks)
        breaks.extend(brk)
        choice_idx = 0xFFFF
        if node.get("choice"):
            ch = node["choice"]
            choice_idx = len(choices)
            items = []
            for item in ch["items"]:
                to = item.get("to")
                if to not in node_index:
                    raise SystemExit("choice %s -> unknown node %r" % (nid, to))
                items.append((pool.add(item.get("text") or ""),
                              pool.add(item.get("label") or ""),
                              node_index[to],
                              ACTION_CODE[item.get("action", "")]))
            citems_first = len(citems)
            citems.extend(items)
            choices.append((pool.add(ch.get("heading") or ""),
                            pool.add(ch.get("category") or ""),
                            citems_first, len(items),
                            KIND_CODE.get(ch.get("kind", ""), 0)))
        nodes.append((pool.add(nid), node["count"], chunk_first_idx, len(node_chunks[nid]),
                      break_first, len(brk), choice_idx,
                      pool.add(node["ending"]) if node.get("ending") else 0xFFFF))

    # ---- 章节 / 鉴赏 / 世界线
    chapters = [(pool.add(name), node_index[spec["node"]], int(spec["index"]))
                for name, spec in graph["chapterStarts"].items()]

    gal_groups = []
    gal_entries = []
    for entry in src.gallery:
        first = len(gal_entries)
        gal_entries.extend(entry["images"])
        gal_groups.append((pool.add(entry["label"]), first, len(entry["images"])))

    wline = []
    wchange = []
    for nid in node_ids:
        row = src.worldline.get(nid)
        first = len(wchange)
        if not row:
            wline.append((node_index[nid], first, 0, parse_rate(src.initial_worldline)))
            continue
        for cursor, rate in row[1]:
            wchange.append((int(cursor), parse_rate(rate)))
        wline.append((node_index[nid], first, len(row[1]), parse_rate(row[0])))

    # ---- 资源计划
    plan = plan_assets(src, [[layers[i] for i in v] for v in views], set(gal_entries))

    def add_extra(name, path, tw, th):
        if not os.path.exists(path):
            raise SystemExit("missing extra asset %s" % path)
        with Image.open(path) as im:
            ow, oh = im.size
            has_alpha = im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info)
        sc = min(1.0, tw / float(ow), th / float(oh)) * PACK_SCALE
        plan[name] = {"path": path, "orig": (ow, oh), "crop": (0, 0, ow, oh),
                      "pack": (max(1, round_half_up(ow * sc)), max(1, round_half_up(oh * sc))),
                      "alpha": has_alpha}

    add_extra("__title_bg", os.path.join(src.common, "title_bg.png"), CANVAS_W, CANVAS_H)
    add_extra("__logo", os.path.join(src.common, "logo.png"), 120, 120)
    card_ids = list(range(487, 499))
    for cid in card_ids:
        add_extra("__card%d" % cid, os.path.join(src.common, "chapters", "%d.png" % cid),
                  CANVAS_W, CANVAS_H)

    asset_names = sorted(plan)
    asset_index = {name: i for i, name in enumerate(asset_names)}

    # ---- 编码
    blob = bytearray()
    asset_entries = []
    stats = {"jpeg": 0, "mask": 0, "sprites": 0, "bgs": 0, "qsum": 0, "qmin": 999, "qmax": 0}
    for name in asset_names:
        info = plan[name]
        pw, ph = info["pack"]
        im = load_rgba(info["path"])
        if im.mode == "RGBA":
            alpha = np.array(im.split()[-1])
            rgb = bleed_rgb(np.array(im.convert("RGB")), alpha)
        else:
            alpha = None
            rgb = np.array(im)
        cx, cy, cw, ch = info["crop"]
        ow, oh = info["orig"]
        if (cx, cy, cw, ch) != (0, 0, ow, oh):
            rgb = rgb[cy:cy + ch, cx:cx + cw]
            if alpha is not None:
                alpha = alpha[cy:cy + ch, cx:cx + cw]
        if rgb.shape[1] != pw or rgb.shape[0] != ph:
            rgb = np.array(Image.fromarray(rgb).resize((pw, ph), Image.LANCZOS))
            if alpha is not None:
                alpha = np.array(Image.fromarray(alpha).resize((pw, ph), Image.LANCZOS))
        budget = max(args.min_bytes,
                     int(pw * ph * (args.bpp_sprite if alpha is not None else args.bpp_bg)))
        q, data = encode_jpeg(Image.fromarray(rgb), budget, args.min_q, args.max_q)
        stats["qsum"] += q
        stats["qmin"] = min(stats["qmin"], q)
        stats["qmax"] = max(stats["qmax"], q)
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob += data
        stats["jpeg"] += len(data)
        mask_off = mask_len = mw = mh = 0
        if alpha is not None:
            mdata, mw, mh = pack_mask(alpha)
            mask_off = len(blob)
            blob += mdata
            mask_len = len(mdata)
            stats["mask"] += len(mdata)
            stats["sprites"] += 1
        else:
            stats["bgs"] += 1
        asset_entries.append((off, len(data), mask_off, mask_len, pw, ph, mw, mh,
                              cx, cy, cw, ch, ow, oh, 1 if name.startswith("__") else 0))

    ui_pairs = [(pool.add(key), pool.add(UI_TEXT[key])) for key in sorted(UI_TEXT)]

    # ---- 序列化
    def pack_strt():
        out = bytearray()
        for off, length in pool.order:
            out += struct.pack("<IH", off, length)
        return bytes(out)

    out = bytearray()
    for n in nodes:
        out += struct.pack("<HIHBBBHH", n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7])
    node_bytes = bytes(out)

    out = bytearray()
    for i in range(len(chunk_count)):
        out += struct.pack("<IHH", chunk_first[i], chunk_count[i], 0)
    chunk_bytes = bytes(out)

    out = bytearray()
    for text, speaker, view, aux, title in frame_rows:
        out += struct.pack("<HHHHH", text, speaker, view, aux, title)
    frame_bytes = bytes(out)

    view_layer_first = []
    view_layer_count = []
    flat_layers = []
    for idxs in views:
        view_layer_first.append(len(flat_layers))
        view_layer_count.append(len(idxs))
        flat_layers.extend(idxs)
    out = bytearray()
    for i in range(len(views)):
        if view_layer_count[i] > 255:
            raise SystemExit("view with too many layers")
        out += struct.pack("<BBH", view_layer_count[i], 0, view_layer_first[i])
    view_bytes = bytes(out)

    out = bytearray()
    for lid in flat_layers:
        layer = layers[lid]
        rect = layer_rect(layer)
        if visible_frac(rect, SCENE_VISIBLE_H) is None:
            raise SystemExit("invisible layer reached packing")
        name = layer[0]
        if name not in asset_index:
            raise SystemExit("layer references unknown asset %r" % (name,))
        out += struct.pack("<HhhHHH", asset_index[name], rect[0], rect[1],
                           rect[2], rect[3], min(1000, int(layer[5])))
    layer_bytes = bytes(out)

    out = bytearray()
    for e in asset_entries:
        out += struct.pack("<IIIIHHHHHHHHHHHxx", *e)
    asset_bytes = bytes(out)

    out = bytearray()
    for heading, category, first, count, kind in choices:
        out += struct.pack("<HHHBBxxxx", heading, category, first, count, kind)
    choice_bytes = bytes(out)

    out = bytearray()
    for text, label, to, action in citems:
        out += struct.pack("<HHHBBx", text, label, to, action, 0)
    citem_bytes = bytes(out)

    out = bytearray()
    for name, node, cursor in chapters:
        out += struct.pack("<HHI", name, node, cursor)
    chapter_bytes = bytes(out)

    out = bytearray()
    for label, first, count in gal_groups:
        out += struct.pack("<HHHH", label, first, count, 0)
    gal_bytes = bytes(out)

    out = bytearray()
    for aid in gal_entries:
        out += struct.pack("<HH", asset_index[aid], 0)
    gentry_bytes = bytes(out)

    out = bytearray()
    for node, first, count, rate in wline:
        out += struct.pack("<HHBxI", node, first, count, rate)
    wline_bytes = bytes(out)

    out = bytearray()
    for cursor, rate in wchange:
        out += struct.pack("<Ii", cursor, rate)
    wchange_bytes = bytes(out)

    break_bytes = b"".join(struct.pack("<I", b) for b in breaks)

    out = bytearray()
    for key, value in ui_pairs:
        out += struct.pack("<HH", key, value)
    ui_bytes = bytes(out)

    chunkref_bytes = b"".join(struct.pack("<H", r) for r in chunkref)

    sections = [
        ("STRT", pack_strt(), len(pool.order)),
        ("TXT", bytes(pool.utf16), len(pool.utf16)),
        ("NODE", node_bytes, len(nodes)),
        ("CHUNKREF", chunkref_bytes, len(chunkref)),
        ("CHUNK", chunk_bytes, len(chunk_count)),
        ("FRAME", frame_bytes, len(frame_rows)),
        ("VIEW", view_bytes, len(views)),
        ("LAYER", layer_bytes, len(flat_layers)),
        ("ASSET", asset_bytes, len(asset_entries)),
        ("BLOB", bytes(blob), len(blob)),
        ("CHOICE", choice_bytes, len(choices)),
        ("CITEM", citem_bytes, len(citems)),
        ("CHAPTER", chapter_bytes, len(chapters)),
        ("GAL", gal_bytes, len(gal_groups)),
        ("GENTRY", gentry_bytes, len(gal_entries)),
        ("WLINE", wline_bytes, len(wline)),
        ("WCHANGE", wchange_bytes, len(wchange)),
        ("BREAK", break_bytes, len(breaks)),
        ("UI", ui_bytes, len(ui_pairs)),
    ]

    meta = {
        "initial": node_index[graph["initial"]],
        "nodes": len(nodes),
        "frames": len(frame_rows),
        "chunks": len(chunk_files),
        "views": len(views),
        "layers": len(flat_layers),
        "assets": len(asset_entries),
        "chapters": len(chapters),
        "gallery_groups": len(gal_groups),
        "gallery_variants": len(gal_entries),
        "endings": len({n[7] for n in nodes if n[7] != 0xFFFF}),
        "initial_worldline": src.initial_worldline,
        "true_worldline": src.true_worldline,
        "canvas": "320x180 art area (432x243 source canvas, ratio 0.7407407)",
        "scene_visible_h": SCENE_VISIBLE_H,
        "pack_scale": PACK_SCALE,
        "jpeg_quality_min": stats["qmin"],
        "jpeg_quality_max": stats["qmax"],
        "jpeg_quality_mean": round(stats["qsum"] / float(len(asset_entries)), 2),
        "asset_title_bg": asset_index["__title_bg"],
        "asset_logo": asset_index["__logo"],
    }
    for cid in card_ids:
        meta["asset_card_%d" % cid] = asset_index["__card%d" % cid]
    meta_buf = bytearray()
    for key in sorted(meta):
        meta_buf += ("%s=%s\n" % (key, meta[key])).encode("utf-8")
    # 章节标题卡:1..11;第 10 章按标题在 496/497 之间选
    meta_buf += ("card_table=" + ";".join(
        ("-1,496,497" if n == 10 else "%d,0,0" % (486 + n if n <= 9 else 498))
        for n in range(1, 12)) + "\n").encode("utf-8")
    sections.append(("META", bytes(meta_buf), len(meta_buf)))

    header_size = 32
    table_size = 16 * len(sections)
    offset = header_size + table_size
    layout = []
    for name, data, count in sections:
        pad = (-offset) % 4
        offset += pad
        layout.append((name, offset, count, len(data), pad))
        offset += len(data)
    total = offset

    out = bytearray()
    out += PACK_MAGIC
    out += struct.pack("<IIIIII", PACK_VERSION, total, len(sections), 0, 0, 0)
    for name, off, count, size, pad in layout:
        out += struct.pack("<IIII", SEC[name], off, count, size)
    data_by_name = {name: data for name, data, _c in sections}
    for name, off, count, size, pad in layout:
        out += b"\x00" * pad
        assert len(out) == off, (name, len(out), off)
        out += data_by_name[name]
    assert len(out) == total

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.manifest)) or ".", exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(out)

    report = {
        "pack": os.path.abspath(args.out),
        "pack_bytes": len(out),
        "pack_sha256": hashlib.sha256(out).hexdigest(),
        "sections": {name: {"count": count, "bytes": size}
                     for name, off, count, size, pad in layout},
        "counts": {k: v for k, v in meta.items() if not k.startswith("asset_")},
        "images": {
            "assets": len(asset_entries),
            "sprites": stats["sprites"],
            "backgrounds": stats["bgs"],
            "jpeg_bytes": stats["jpeg"],
            "mask_bytes": stats["mask"],
            "quality_min": stats["qmin"],
            "quality_max": stats["qmax"],
            "quality_mean": round(stats["qsum"] / float(len(asset_entries)), 2),
        },
        "ui_strings": len(ui_pairs),
        "worldline_changes": len(wchange),
    }
    text = json.dumps(report, ensure_ascii=False, indent=2)
    print(text)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    if args.manifest:
        with open(args.manifest, "w", encoding="utf-8") as fh:
            fh.write(text + "\n")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source", required=True, help="steins-rw5 源快照目录(只读)")
    ap.add_argument("--out", default="assets/steins/steins_pack.bin")
    ap.add_argument("--manifest", default="assets/steins/steins_pack.json")
    ap.add_argument("--report", default="")
    ap.add_argument("--bpp-bg", type=float, default=0.060)
    ap.add_argument("--bpp-sprite", type=float, default=0.080)
    ap.add_argument("--min-bytes", type=int, default=700)
    ap.add_argument("--min-q", type=int, default=50)
    ap.add_argument("--max-q", type=int, default=92)
    ap.add_argument("--pack-scale", type=float, default=1.0)
    args = ap.parse_args()
    global PACK_SCALE
    PACK_SCALE = args.pack_scale
    return build(args)


if __name__ == "__main__":
    sys.exit(main())
