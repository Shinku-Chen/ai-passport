#!/usr/bin/env python3
"""encode_opus.py —— 把 assets/project 里的语音素材编码成 Opus 8kbps，供固件 Opus 解码器播放。

当前活跃的语音链路(encode_voice.py 是遗留 IMA-ADPCM 版本, 仅被本脚本复用
其显示名清洗与 C 索引生成):
  1. ffmpeg(libopus) 把 mp3/ogg/wav -> Opus 8kbps @16kHz 单声道
  2. 响度归一化后剥离 Ogg 容器为裸包流 -> assets/audio/dirNN/clipMM.opus
     (每个包 = 2字节小端长度 + Opus 帧, 与固件解码格式一致)
  3. 生成 voice_index.json + voice_index.h(固件编译期索引)
  4. 用 spiffsgen.py 打包成 assets/audio/voicefs.img (分区大小 0x5F0000)

仅从已提交片段重新打包数据分区(不重编码、无 ffmpeg)请用 tools/pack_voicefs.py,
构建/CI 也用后者。

用法:
  python tools/encode_opus.py [--dirs 目录名,目录名] [--bitrate 8]
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# 复用 encode_voice.py 的显示名清洗与 C 索引生成，避免两套逻辑漂移。
import encode_voice as EV

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = PROJECT_ROOT / "assets" / "project"
OUT_DIR = PROJECT_ROOT / "assets" / "audio"

# --- Opus 编码参数(与固件解码器对齐) ---
SAMPLE_RATE = 16000      # 与现有链路一致的采样率(Opus 体积由码率决定, 非采样率)
CHANNELS = 1             # 单声道
BITRATE_KBPS = 8         # 8 kbps; Opus 感知编码, 体积随码率线性变化

# 分区大小: 更新后的 voicefs 分区 0x5F0000 = 6225920 bytes
PARTITION_SIZE = 0x5F0000

# 常用 ffmpeg 位置(带 libopus 编码器)；找不到则报错提示。
FFMPEG_CANDIDATES = [
    r"C:\Program Files\ZWSOFT\ZWCAD\ffmpeg.exe",
    "ffmpeg",
]


def find_ffmpeg() -> str:
    for f in FFMPEG_CANDIDATES:
        p = Path(f)
        if p.is_file() or (Path(f).name == "ffmpeg" and shutil.which("ffmpeg")):
            return f
    raise FileNotFoundError(
        "未找到带 libopus 的 ffmpeg。请确认 ffmpeg.exe 可用或调整 FFMPEG_CANDIDATES。")


def _strip_ogg_to_raw_packets(ogg_path: Path, out_path: Path) -> int:
    """把 Ogg Opus 文件剥离成裸 Opus 包流(固件端解码用)。

    格式: 每个 Opus 包 = 2 字节小端长度 + 包数据。跳过 OpusHead / OpusTags
    头页(由 'OpusHead'/'OpusTags' payload 前缀识别), 只写入音频包。
    返回写出的总字节数。
    """
    import struct
    data = ogg_path.read_bytes()
    off = 0
    out = bytearray()
    n = len(data)
    while off + 27 <= n and data[off:off + 4] == b"OggS":
        nsegs = data[off + 26]
        seg_end = off + 27 + nsegs
        if seg_end > n:
            break
        lacing = list(data[off + 27:seg_end])
        body_start = seg_end
        body_size = sum(lacing)
        body = data[body_start:body_start + body_size]
        # 还原每页里的包(>=255 的连续 segment 构成一个包, 到非255结束)
        pkts = []
        cur = bytearray()
        for seg in lacing:
            cur.extend(body[:seg])
            body = body[seg:]
            if seg < 255:
                pkts.append(bytes(cur))
                cur = bytearray()
        if cur:
            pkts.append(bytes(cur))
        # 头页: 第一个包以 OpusHead/OpusTags 开头 -> 跳过
        if pkts and (pkts[0].startswith(b"OpusHead") or pkts[0].startswith(b"OpusTags")):
            off = body_start + body_size
            continue
        # 音频包: 写入 2 字节长度 + 包数据
        for p in pkts:
            if not p:
                continue
            out += struct.pack("<H", len(p))
            out += p
        off = body_start + body_size
    out_path.write_bytes(bytes(out))
    return len(out)


# 响度归一化参数: 把每段感知响度(unweighted RMS)统一到 TARGET_MEAN_DB,
# 峰值不超过 TARGET_PEAK_DB(防削顶)。这比"只拉峰值"更能让所有素材听起来一样响。
# 实测素材 mean_volume 从 -6.6dB(我要验牌)到 -29.4dB(吉伊卡哇), 差 23dB; 统一到此目标后响度一致。
TARGET_MEAN_DB = -14.0   # 目标平均响度(感知 RMS); 偏小声的素材被提升, 过响的被适度压到统一
TARGET_PEAK_DB = -1.0    # 峰值上限(防削顶)


def _measure_volume(ffmpeg: str, src: Path) -> tuple[float, float]:
    """用 ffmpeg volumedetect 测源素材的 mean_volume(RMS) 与 max_volume(峰值)。失败返回 (-999,-999)。"""
    import re
    try:
        r = subprocess.run([ffmpeg, "-i", str(src), "-af", "volumedetect", "-f", "null", "-"],
                           capture_output=True, text=True)
        mean = maxvol = None
        for line in r.stderr.splitlines():
            m = re.search(r"mean_volume:\s*(-?[\d.]+)\s*dB", line)
            if m: mean = float(m.group(1))
            m = re.search(r"max_volume:\s*(-?[\d.]+)\s*dB", line)
            if m: maxvol = float(m.group(1))
        if mean is not None and maxvol is not None:
            return mean, maxvol
    except Exception:
        pass
    return -999.0, -999.0


def encode_opus(ffmpeg: str, src: Path, out: Path, bitrate_kbps: int) -> int:
    """把 src 编码为裸 Opus 包流到 out, 返回字节数。

    编码前做响度归一化(统一感知响度): 取 min(TARGET_MEAN-mean, TARGET_PEAK-max) 为增益,
    把每段的平均响度统一到 TARGET_MEAN_DB, 同时峰值不超过 TARGET_PEAK_DB(防削顶)。
    偏小声的素材(如吉伊卡哇 -29dB)被大幅提升, 过响的适度压到统一, 使所有素材听感一致。
    """
    import tempfile
    mean, maxdb = _measure_volume(ffmpeg, src)
    # 只对可测量的素材计算增益; 测量失败(-999)则视为达标不加增益
    if mean > -900 and maxdb > -900:
        gain_db = min(TARGET_MEAN_DB - mean, TARGET_PEAK_DB - maxdb)
    else:
        gain_db = 0.0
    with tempfile.TemporaryDirectory() as tmp:
        ogg_path = Path(tmp) / "tmp.opus"
        cmd = [ffmpeg, "-y", "-i", str(src)]
        if abs(gain_db) > 0.5:
            cmd += ["-af", f"volume={gain_db:.1f}dB"]   # 增益(负=压小声, 正=提升)
        cmd += ["-c:a", "libopus", "-b:a", f"{bitrate_kbps}k",
                "-ar", str(SAMPLE_RATE), "-ac", str(CHANNELS),
                "-application", "voip", str(ogg_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0 or not ogg_path.exists():
            raise RuntimeError(f"opus 编码失败 {src}: {res.stderr[-500:]}")
        total = _strip_ogg_to_raw_packets(ogg_path, out)
    return total


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    dir_filter = None
    bitrate = BITRATE_KBPS
    # 允许 --dirs 筛选 / --bitrate 覆盖, 二者可独立或组合使用
    if "--dirs" in sys.argv:
        i = sys.argv.index("--dirs")
        if i + 1 < len(sys.argv):
            dir_filter = {x.strip() for x in sys.argv[i + 1].split(",")}
            print(f"[筛选] 只处理目录: {sorted(dir_filter)}")
    if "--bitrate" in sys.argv:
        i = sys.argv.index("--bitrate")
        if i + 1 < len(sys.argv):
            bitrate = int(sys.argv[i + 1])
    if "--dirs" not in sys.argv and "--bitrate" not in sys.argv and len(sys.argv) > 1:
        print(f"用法: {sys.argv[0]} [--dirs 目录名,目录名] [--bitrate N]", file=sys.stderr)
        return 1

    ffmpeg = find_ffmpeg()
    print(f"使用 ffmpeg: {ffmpeg} (Opus {bitrate}kbps @{SAMPLE_RATE}Hz mono)")

    # 清理旧的 ascii 目录(避免残留上一次编码结果)
    for old in OUT_DIR.iterdir():
        if old.is_dir() and old.name.startswith("dir"):
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
        dir_entry = {"dir": EV.clean_display_name(d.name), "dir_id": dir_id, "files": []}
        ascii_dir = f"dir{dir_id:02d}"
        dir_entry["dir_ascii"] = ascii_dir
        out_dir = OUT_DIR / ascii_dir
        out_dir.mkdir(parents=True, exist_ok=True)
        for i, f in enumerate(files):
            ascii_name = f"clip{i:02d}.opus"
            try:
                nbytes = encode_opus(ffmpeg, f, out_dir / ascii_name, bitrate)
            except RuntimeError as e:
                print(f"  [skip] {f.name}: {e}", file=sys.stderr)
                continue
            rel_path = f"{ascii_dir}/{ascii_name}"
            # samples/duration：ffmpeg 解码后耗时不计入镜像体积，仅作索引参考。
            # 这里取 Opus 文件字节数与估算时长（时长用 ffprobe 或粗略估算）。
            dur = _probe_duration(ffmpeg, f)
            dir_entry["files"].append({
                "name": EV.clean_display_name(f.stem),
                "src": f.name,
                "path": rel_path,
                "samples": int(dur * SAMPLE_RATE),
                "duration": round(dur, 3),
                "compressed_bytes": nbytes,
            })
        dirs.append(dir_entry)
        dir_id += 1

    total_compressed = sum(e["compressed_bytes"] for d in dirs for e in d["files"])
    total_files = sum(len(d["files"]) for d in dirs)
    print(f"编码完成: {total_files} 段, Opus 压缩后共 {total_compressed} bytes"
          f" ({total_compressed/1024:.1f} KB)")
    print(f"分区大小: {PARTITION_SIZE/1048576:.2f} MB;"
          f" 占用 {100*total_compressed/PARTITION_SIZE:.1f}%")

    # 索引
    index_path = OUT_DIR / "voice_index.json"
    index_path.write_text(json.dumps(dirs, ensure_ascii=False, indent=2),
                          encoding="utf-8")
    print(f"索引: {index_path}")

    # C 索引(编译期表)。字段 opus_bytes 存 Opus 文件字节数。
    EV.gen_c_index(dirs, PROJECT_ROOT / "main" / "voice_index.h")

    # SPIFFS 镜像(按扩充后的分区大小)
    try:
        _build_spiffs_image(dirs)
    except (FileNotFoundError, OSError) as e:
        print(f"提示: 未找到可用的 ESP-IDF spiffsgen.py, 跳过 voicefs.img 生成。"
              f"({e})")
    except RuntimeError as e:
        print(f"提示: voicefs.img 生成失败: {e}")
    return 0


def _probe_duration(ffmpeg: str, path: Path) -> float:
    """粗略解析 ffmpeg -i 的 Duration 行。失败返回 0。"""
    try:
        r = subprocess.run([ffmpeg, "-i", str(path)], capture_output=True, text=True)
        for line in r.stderr.splitlines():
            if "Duration:" in line:
                t = line.split("Duration:")[1].split(",")[0].strip()
                h, m, s = t.split(":")
                return int(h) * 3600 + int(m) * 60 + float(s)
    except Exception:
        pass
    return 0.0


def _build_spiffs_image(dirs: list) -> None:
    """调用 ESP-IDF spiffsgen.py 把 dir* 目录打包成 voicefs.img(PARTITION_SIZE)。"""
    import glob as _glob
    spiffsgen = None
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

    with tempfile.TemporaryDirectory() as tmp:
        src_root = Path(tmp)
        for d in dirs:
            src_dir = OUT_DIR / d["dir_ascii"]
            if src_dir.is_dir():
                shutil.copytree(src_dir, src_root / d["dir_ascii"])
        img_path = OUT_DIR / "voicefs.img"
        cmd = [sys.executable, spiffsgen, str(PARTITION_SIZE), str(src_root), str(img_path)]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if res.returncode != 0:
            raise RuntimeError(f"spiffsgen 失败: {res.stderr or res.stdout}")
        print(f"SPIFFS 镜像: {img_path} ({img_path.stat().st_size} bytes)")


if __name__ == "__main__":
    sys.exit(main())
