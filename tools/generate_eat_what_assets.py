#!/usr/bin/env python3
# tools/generate_eat_what_assets.py —— 把两份 GIF 素材转成 LVGL I8 索引色二进制。
#
# 输出(默认写入 main/ 组件目录,供 CMake EMBED_FILES 嵌入 flash):
#   eat_what_g1.bin   引导 GIF(今天午餐要吃什么呢)- RES 分辨率帧序列
#   eat_what_g2.bin   食物 GIF(今天吃什么选择器)- 同上
#   eat_what_assets.h 帧数/帧间隔/分辨率/符号声明
#
# 格式(每帧,LVGL LV_COLOR_FORMAT_I8 布局,与 v9.5 bin decoder 一致):
#   [调色板] 256 色 * 4 字节/色 = 1024 字节,颜色按 lv_color32_t 内存序 B,G,R,A
#   [像素]   RES*RES 字节,每像素 1 字节索引
# 帧与帧连续拼接;每帧独立调色板(GIF 各帧色彩分布差异大,共享会明显失色)。
#
# 用法: python3 tools/generate_eat_what_assets.py [RES]
#   RES 默认 160。选值权衡:
#     - C3 无 PSRAM,LVGL 解码 I8 图像时若 RAM_LOAD=1 会整帧转 ARGB8888(约 RES*RES*4 B)。
#       160x160 整帧约 100KB,在 demo 打开时(彼时 Wi-Fi/BLE 已停)的堆可承受。
#     - flash 上 160px 两份合计约 1.04MB,在 3MB app 分区内余量充足。
#     需要更清晰可传 192(flash +0.5MB,整帧 147KB 风险升高);更省内存可传 144/120。
#
# 依赖: Pillow。素材路径是开发机上的 Cindy 媒体缓存,仅供生成用,不落库。
import sys, os, struct

RES = int(sys.argv[1]) if len(sys.argv) > 1 else 160
COLORS = 256
PALETTE_BYTES = COLORS * 4                 # 1024: lv_color32_t (B,G,R,A)
FRAME_BYTES = PALETTE_BYTES + RES * RES

G1 = "/Users/chenzhuyu/Library/Application Support/Cindy/cindy-media/blobs/9a/9ab5a5e2d8ce40e54a20a0e7476fed07b0b25bf4612066679bab14f32ee40228.gif"
G2 = "/Users/chenzhuyu/Library/Application Support/Cindy/cindy-media/blobs/41/41dd4b529d29ccccbb0c07153bdf14726e4ea9fe5c6ced6a2de49dcbb49e8030.gif"

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "main")
OUT_DIR = os.path.abspath(OUT_DIR)

from PIL import Image


def frames_to_bin(gif_path, out_name):
    """读取 GIF 每一帧,resize 到 RES,量化 256 色,写成 LVGL I8 帧序列文件。"""
    img = Image.open(gif_path)
    n = img.n_frames
    with open(os.path.join(OUT_DIR, out_name), "wb") as f:
        for i in range(n):
            img.seek(i)
            rgb = img.convert("RGB").resize((RES, RES), Image.LANCZOS)
            q = rgb.quantize(colors=COLORS, method=Image.MEDIANCUT)
            pal = q.getpalette()                     # [R,G,B, R,G,B, ...]
            num = q.getpalette() and (len(pal) // 3)
            idx = q.tobytes()                        # RES*RES 字节索引
            if len(idx) != RES * RES:
                raise RuntimeError("索引长度异常")
            for c in range(COLORS):
                if c < num:
                    r, g, b = pal[3 * c], pal[3 * c + 1], pal[3 * c + 2]
                else:
                    r = g = b = 0
                # lv_color32_t 内存序为 B,G,R,A(alpha 置 FF 不透明)
                f.write(struct.pack("BBBB", b, g, r, 0xFF))
            f.write(idx)
    return n


def main():
    n1 = frames_to_bin(G1, "eat_what_g1.bin")
    n2 = frames_to_bin(G2, "eat_what_g2.bin")

    g1_size = n1 * FRAME_BYTES
    g2_size = n2 * FRAME_BYTES
    total = g1_size + g2_size

    hdr = f"""// main/eat_what_assets.h —— 由 tools/generate_eat_what_assets.py 生成,勿手改。
// 两份素材的 LVGL I8 索引帧数据,经 main/CMakeLists.txt 的 EMBED_FILES 嵌入 flash。
// 布局:每帧 = 256色调色板(1024B) + 像素索引(RES*RES 字节),连续拼接。
#pragma once

#define EAT_WHAT_RES           {RES}
#define EAT_WHAT_FRAME_BYTES   {FRAME_BYTES}   // 每帧字节 = 调色板 + 像素
#define EAT_WHAT_PALETTE_BYTES {PALETTE_BYTES}

#define EAT_WHAT_G1_FRAMES     {n1}            // 引导: 今天午餐要吃什么呢?
#define EAT_WHAT_G1_DELAY_MS   100             // 原速 10 fps
#define EAT_WHAT_G2_FRAMES     {n2}            // 食物: 今天吃什么选择器
#define EAT_WHAT_G2_DELAY_MS   50              // 原速 20 fps

// ESP-IDF EMBED_FILES 生成的符号(指向 flash 中的 .rodata)。
extern const uint8_t _binary_eat_what_g1_bin_start[];
extern const uint8_t _binary_eat_what_g1_bin_end[];
extern const uint8_t _binary_eat_what_g2_bin_start[];
extern const uint8_t _binary_eat_what_g2_bin_end[];
"""
    with open(os.path.join(OUT_DIR, "eat_what_assets.h"), "w") as f:
        f.write(hdr)

    mb = lambda n: n / 1024 / 1024
    print(f"RES={RES}  每帧={FRAME_BYTES}B")
    print(f"G1(引导) {n1}帧 -> eat_what_g1.bin {mb(g1_size):.2f} MB")
    print(f"G2(食物) {n2}帧 -> eat_what_g2.bin {mb(g2_size):.2f} MB")
    print(f"合计      {n1+n2}帧 -> {mb(total):.2f} MB")
    print(f"已写 {OUT_DIR}/eat_what_g1.bin, eat_what_g2.bin, eat_what_assets.h")


if __name__ == "__main__":
    main()
