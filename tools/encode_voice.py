#!/usr/bin/env python3
"""encode_voice.py —— 把 assets/project 里的语音素材预处理成固件可播放的 IMA-ADPCM 4bit 压缩 PCM。

流程（每段音频）:
  1. miniaudio 解码 mp3/ogg/wav -> 8 kHz 单声道 int16（解码时即完成重采样与声道合并）
  2. numpy 低通滤波（去残留高频, 语音带宽约 4 kHz）
  3. 去静音（剪除首尾过短 / 过低能量区）
  4. IMA-ADPCM 4bit 编码
  5. 写入 littlefs 镜像 + 生成 voice_index.h（固件端目录/文件索引）

输出在 assets/audio/ 下:
  voicefs.img             —— littlefs 分区镜像（可烧到 voicefs 数据分区）
  voice_index.json        —— 目录/文件 -> 分区路径 的中文映射（人可读 + 测试用）
  voice_index.h          —— 固件编译期索引(目录/文件 id/长度/偏移)

用法:
  python tools/encode_voice.py
"""
from __future__ import annotations

import json
import os
import sys
import wave
from pathlib import Path

import numpy as np

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = PROJECT_ROOT / "assets" / "project"
OUT_DIR = PROJECT_ROOT / "assets" / "audio"

SAMPLE_RATE = 16000         # 16 kHz 语音(与 demo/blufi 分支一致, 8000Hz 播放无声)
BIT_DEPTH = 16
CHANNELS = 1

# 文件名里可能出现的非 BMP / emoji 字符, 中文字体(Noto CJK子集)没有其字形,
# 会在 UI 显示缺字块。这里把常见 emoji 映射成对应中文, 让显示名可读。
EMOJI_REPLACE = {
    "\U0001F426": "鸟",      # 🐦 鸟
    "\U0001F3A7": "耳机",    # 🎧
    "\U0001F525": "火",      # 🔥
    "\U0001F44D": "赞",      # 👍
}


def clean_display_name(name: str) -> str:
    """把非 BMP(如 emoji)字符替换为对应中文, 保证中文字体子集能显示。"""
    out = []
    for c in name:
        if ord(c) > 0xFFFF:
            out.append(EMOJI_REPLACE.get(c, ""))   # 无映射的去 emoji
        else:
            out.append(c)
    return "".join(out)

# ---- IMA-ADPCM 步长表（标准 IMA） ----
IMA_STEPS = np.array([
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
], dtype=np.int32)

# 每个 code (4bit 高四位) 对应的增量系数
IMA_INDEX_TABLE = np.array([-1, -1, -1, -1, 2, 4, 6, 8,
                            -1, -1, -1, -1, 2, 4, 6, 8], dtype=np.int32)
IMA_STEP_INDEX_LIMIT = len(IMA_STEPS)


def ima_encode(samples: np.ndarray) -> tuple[bytes, int]:
    """把 int16 采样编码成标准 IMA-ADPCM 4bit。返回 (压缩字节, 初始 step_index)。

    量化与重建使用同一套对称公式, 保证编解码不失步:
      量化:  delta = min(abs(diff)*4 // step, 7)
      重建:  diff_q = ((code&7)*2 + 1) * step >> 3,  predictor += (diff>=0 ? diff_q : -diff_q)
      步长:  step_index += index_table[code]  (clamp 到 [0, 88])
    """
    samples = np.asarray(samples, dtype=np.int32)
    n = len(samples)
    n_out = (n + 1) // 2   # 两采样一字节, 低 4 位在前
    out = bytearray(n_out)

    predictor = 0
    step_index = 0

    for i in range(n):
        sample = int(samples[i])
        diff = sample - predictor
        step = int(IMA_STEPS[step_index])

        if step == 0:
            code = 0
            diff_q = 0
        else:
            if diff >= 0:
                delta = min(diff * 4 // step, 7)
                code = delta
            else:
                delta = min(-diff * 4 // step, 7)
                code = delta | 0x08
            # 标准重建增量, 与量化阶梯匹配
            diff_q = ((code & 0x07) * 2 + 1) * step >> 3
            if diff < 0:
                diff_q = -diff_q

        predictor = max(-32768, min(32767, predictor + diff_q))

        step_index += int(IMA_INDEX_TABLE[code])
        step_index = 0 if step_index < 0 else min(step_index, IMA_STEP_INDEX_LIMIT - 1)

        # 组包: 偶数采样放低 4bit, 奇数采样放高 4bit
        idx = i >> 1
        if (i & 1) == 0:
            out[idx] = code & 0x0F
        else:
            out[idx] |= (code & 0x0F) << 4

    return bytes(out), 0


def lowpass(samples: np.ndarray, cutoff_hz: float = 4000.0) -> np.ndarray:
    """轻量一阶低通, 抑制 4 kHz 以上残留高频。"""
    if cutoff_hz >= SAMPLE_RATE / 2:
        return samples
    alpha = np.exp(-2.0 * np.pi * cutoff_hz / SAMPLE_RATE)
    y = np.empty_like(samples, dtype=np.float32)
    acc = float(samples[0])
    for i, x in enumerate(samples.astype(np.float32)):
        acc = alpha * acc + (1.0 - alpha) * x
        y[i] = acc
    return y.astype(np.int16)


def trim_silence(samples: np.ndarray, threshold: int = 80, pad: int = 200) -> np.ndarray:
    """剪除首尾静音。threshold 为能量阈值, pad 为两侧保留的静音采样数。"""
    if len(samples) <= pad * 2:
        return samples
    amp = np.abs(samples)
    idx = np.where(amp > threshold)[0]
    if len(idx) == 0:
        return samples
    start = max(0, int(idx[0]) - pad)
    end = min(len(samples), int(idx[-1]) + pad)
    return samples[start:end]


def decode_with_miniaudio(path: Path) -> tuple[np.ndarray, float]:
    """用 miniaudio 解码任意受支持音频 -> int16 8kHz mono。返回 (采样, 时长s)。"""
    import miniaudio
    data = path.read_bytes()
    d = miniaudio.decode(
        data,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=CHANNELS,
        sample_rate=SAMPLE_RATE,
    )
    samples = np.frombuffer(d.samples, dtype=np.int16).copy()
    return samples, d.duration


def decode_with_wave(path: Path) -> tuple[np.ndarray, float]:
    """兜底: 纯 wave 解码(若 miniaudio 不处理 wav)。"""
    with wave.open(str(path), "rb") as w:
        nframes = w.getnframes()
        ch = w.getnchannels()
        rate = w.getframerate()
        sw = w.getsampwidth()
        raw = w.readframes(nframes)
    arr = np.frombuffer(raw, dtype=np.int16).copy()  # 假设 16bit
    if ch > 1:
        arr = arr.reshape(-1, ch).mean(axis=1).astype(np.int16)
    if rate != SAMPLE_RATE:
        # 简单线性重采样
        n_new = int(len(arr) * SAMPLE_RATE / rate)
        x = np.linspace(0, len(arr) - 1, n_new)
        arr = np.interp(x, np.arange(len(arr)), arr).astype(np.int16)
    return arr, nframes / rate


def load_audio(path: Path) -> tuple[np.ndarray, float]:
    ext = path.suffix.lower()
    try:
        if ext == ".wav":
            # 优先 miniaudio(更好的重采样), 失败再 wave
            try:
                return decode_with_miniaudio(path)
            except Exception:
                return decode_with_wave(path)
        return decode_with_miniaudio(path)
    except Exception as e:
        raise RuntimeError(f"无法解码 {path}: {type(e).__name__}: {e}") from e


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # --dirs <名称,名称> 只编码这些目录; 不传则处理全部。
    # 用途: 数据分区容量受限时先只放部分目录跑通链路, 再逐步扩充。
    dir_filter = None
    if len(sys.argv) > 2 and sys.argv[1] == "--dirs":
        dir_filter = {x.strip() for x in sys.argv[2].split(",")}
        print(f"[筛选] 只处理目录: {sorted(dir_filter)}")
    elif len(sys.argv) > 1:
        print(f"用法: {sys.argv[0]} [--dirs 目录名,目录名]", file=sys.stderr)

    # 清理旧的 ascii 目录, 避免残留上一次编码的文件
    for old in OUT_DIR.iterdir():
        if old.is_dir() and old.name.startswith("dir"):
            import shutil
            shutil.rmtree(old)

    dirs = []
    dir_id = 0
    for d in sorted(p for p in SRC_DIR.iterdir() if p.is_dir()):
        if dir_filter is not None and d.name not in dir_filter:
            continue
        files = sorted(
            p for p in d.iterdir()
            if p.suffix.lower() in (".mp3", ".ogg", ".wav")
        )
        if not files:
            continue
        dir_entry = {"dir": clean_display_name(d.name), "dir_id": dir_id, "files": []}
        # littlefs 对中文/特殊字符路径支持不佳, 这里用 ASCII 目录名 + 序号文件
        ascii_dir = f"dir{dir_id:02d}"
        dir_entry["dir_ascii"] = ascii_dir
        out_dir = OUT_DIR / ascii_dir
        out_dir.mkdir(parents=True, exist_ok=True)
        for i, f in enumerate(files):
            try:
                samples, dur = load_audio(f)
            except RuntimeError as e:
                print(f"  [skip] {f.name}: {e}", file=sys.stderr)
                continue
            samples = lowpass(samples)
            samples = trim_silence(samples)
            compressed, _ = ima_encode(samples)
            # 文件名用纯 ASCII 确定性命名(目录序号_文件序号), 避免 littlefs 特殊字符/中文;
            # 中文显示名放 JSON 索引, 不依赖文件系统对中文的支持
            ascii_name = f"clip{i:02d}.adpcm"
            rel_path = f"{ascii_dir}/{ascii_name}"
            (out_dir / ascii_name).write_bytes(compressed)
            dir_entry["files"].append({
                "name": clean_display_name(f.stem),  # 中文名(清洗emoji) 供 UI 显示
                "src": f.name,
                "path": rel_path,
                "samples": int(len(samples)),
                "duration": round(dur, 3),
                "compressed_bytes": len(compressed),
            })
        dirs.append(dir_entry)
        dir_id += 1

    # 汇总统计
    total_compressed = sum(e["compressed_bytes"] for d in dirs for e in d["files"])
    total_files = sum(len(d["files"]) for d in dirs)
    print(f"编码完成: {total_files} 段, 压缩后共 {total_compressed} bytes"
          f" ({total_compressed/1024:.1f} KB)")

    # 写出索引
    index_path = OUT_DIR / "voice_index.json"
    index_path.write_text(json.dumps(dirs, ensure_ascii=False, indent=2),
                          encoding="utf-8")
    print(f"索引: {index_path}")

    # 生成固件端 C 索引: 编译期表, 免掉运行时解析 JSON
    # 放到 main/ 让 voice_app.c 能以 #include "voice_index.h" 找到
    gen_c_index(dirs, PROJECT_ROOT / "main" / "voice_index.h")

    # 生成 SPIFFS 数据分区镜像 voicefs.img (固件用它存压缩语音)。
    # 用 ESP-IDF 的 spiffsgen.py; 找不到该工具时明确提示, 不静默跳过。
    try:
        _build_spiffs_image(dirs)
    except (FileNotFoundError, OSError) as e:
        print(f"提示: 未找到可用的 ESP-IDF spiffsgen.py, 跳过 voicefs.img 生成。"
              f"({e}) 若要烧录数据分区, 请在有 ESP-IDF 的环境下重跑本工具。")
    except RuntimeError as e:
        print(f"提示: voicefs.img 生成失败: {e}")
    return 0


def _build_spiffs_image(dirs: list) -> None:
    """调用 ESP-IDF spiffsgen.py 把 dir* 目录打包成 voicefs.img(3MB)。"""
    import shutil
    import subprocess
    import tempfile

    spiffsgen = None
    # 用 glob 找 esp-idf-* 下的 components/spiffs/spiffsgen.py
    import glob as _glob
    for pat in [r"C:\Espressif\frameworks\esp-idf-*\components\spiffs\spiffsgen.py",
                r"D:\Espressif\frameworks\esp-idf-*\components\spiffs\spiffsgen.py"]:
        hits = _glob.glob(pat)
        if hits:
            spiffsgen = hits[0]
            break
    if not spiffsgen:
        spiffsgen = shutil.which("spiffsgen.py")
    if not spiffsgen:
        raise FileNotFoundError("spiffsgen.py 未找到")

    # 需要一个只含 dir* 的临时目录作为 SPIFFS 根
    with tempfile.TemporaryDirectory() as tmp:
        src_root = Path(tmp)
        for d in dirs:
            src_dir = OUT_DIR / d["dir_ascii"]
            if src_dir.is_dir():
                shutil.copytree(src_dir, src_root / d["dir_ascii"])
        img_path = OUT_DIR / "voicefs.img"
        size = 3 * 1024 * 1024  # 与 partitions.csv voicefs 分区一致 (0x300000)
        cmd = [sys.executable, spiffsgen, str(size), str(src_root), str(img_path)]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if res.returncode != 0:
            raise RuntimeError(f"spiffsgen 失败: {res.stderr or res.stdout}")
        print(f"SPIFFS 镜像: {img_path} ({img_path.stat().st_size} bytes)")


def _c_escape(s: str) -> str:
    """把字符串转成 C 字符串字面量, 规避 `\\xHH` 粘连。

    规则: 每个字节要么是"安全可读" ASCII(直接输出), 要么转成 `\\xHH`。
    所谓安全可读 = 非反斜杠/引号, 且不是十六进制字符(0-9a-fA-F)。
    这样 `\\xHH` 之后接的必然是 `\\x`(下一字节)或安全非hex字符, 绝不会被 C 的
    贪心 hex 转义吞并(这正是旧实现 "hex escape sequence out of range" 的根因)。
    """
    out = []
    hexchars = set("0123456789abcdefABCDEF")
    for ch in s:
        c = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif c < 0x80 and ch not in hexchars:
            out.append(ch)          # 安全可读 ASCII(非hex)
        else:
            # 含汉字/全角/数字字母等 hex 字符, 一律转 UTF-8 字节, 逐字节 \\xHH
            for b in (ch.encode("utf-8") if c >= 0x80 else ch.encode("ascii")):
                out.append(f"\\x{b:02x}")
    return "".join(out)


def gen_c_index(dirs: list, path: Path) -> None:
    """写 voice_index.h —— 目录/文件/中文名/路径/字节数/采样数 的编译期表。"""
    lines = [
        "// auto-generated by tools/encode_voice.py — 音效钥匙扣素材索引",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "// 目录名(中文, UTF-8 嵌入, 供 UI 显示)",
        "static const char *const VOICE_DIR_NAMES[] = {",
    ]
    for d in dirs:
        lines.append(f'    "{_c_escape(d["dir"])}",')
    lines += ["};", ""]
    lines.append(f"#define VOICE_DIR_COUNT {len(dirs)}")
    lines.append("")
    # 类型只定义一次, 供下面各目录与最终 VOICE_DIRS 引用
    lines.append("typedef struct { const char *path; const char *name; "
                 "uint32_t adpcm_bytes; uint32_t samples; } voice_file_t;")
    lines.append("")
    lines.append("// 每个目录: 文件表(路径/中文名/字节/采样)。类型只定义一次(见上)。")
    for d in dirs:
        d_id = d["dir_id"]
        lines.append(f"// --- dir{d_id:02d}: {d['dir']} ---")
        lines.append(f"static const voice_file_t VOICE_DIR_{d_id:02d}_FILES[] = {{")
        for e in d["files"]:
            lines.append(f'    {{ "{_c_escape(e["path"])}", "{_c_escape(e["name"])}", '
                         f'{e["compressed_bytes"]}u, {e["samples"]}u }},')
        lines.append("};")
        lines.append(f"static const voice_file_t *const VOICE_DIR_{d_id:02d} = "
                     f"VOICE_DIR_{d_id:02d}_FILES;")
        lines.append(f"#define VOICE_DIR_{d_id:02d}_COUNT "
                     f"(sizeof(VOICE_DIR_{d_id:02d}_FILES)/sizeof(VOICE_DIR_{d_id:02d}_FILES[0]))")
        lines.append("")

    # 目录表: 每个目录的文件指针与计数
    lines.append("typedef struct { const voice_file_t *files; uint32_t count; "
                 "const char *name; } voice_dir_t;")
    lines.append("static const voice_dir_t VOICE_DIRS[] = {")
    for d in dirs:
        d_id = d["dir_id"]
        lines.append(f'    {{ VOICE_DIR_{d_id:02d}, VOICE_DIR_{d_id:02d}_COUNT, '
                     f'"{_c_escape(d["dir"])}" }},')
    lines.append("};")
    lines.append(f"#define VOICE_DIR_TOTAL "
                 f"(sizeof(VOICE_DIRS)/sizeof(VOICE_DIRS[0]))")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"C 索引: {path}")


if __name__ == "__main__":
    sys.exit(main())
