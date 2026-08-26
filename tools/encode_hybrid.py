#!/usr/bin/env python3
"""encode_hybrid.py —— 混合码率编码, 保留全部素材段, 让总量装进 voicefs 分区。

策略: 默认全部用高质量码率(HI_BITRATE, 12k); 若总数据 + SPIFFS 元数据
超过分区容量, 则按"音频时长降序"把最长的段降到低码率(LO_BITRATE, 8k),
直到总量 <= 分区。这样只牺牲时长占比大的少数段的音质, 其余保持高音质。

复用 encode_opus 的 ffmpeg 编码 / Ogg 剥离 / 索引生成 / SPIFFS 打包。

用法:
  python tools/encode_hybrid.py              # 全量, 12k 主 / 8k 降级
"""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

import encode_opus as EO
import encode_voice as EV

PARTITION_SIZE = EO.PARTITION_SIZE      # voicefs 分区大小(字节)
HI_BITRATE = 16                          # 大部分段(高音质, 实测全量 5.87MB 超分区)
LO_BITRATE = 12                          # 超预算时降级的最长段

# SPIFFS 元数据开销估算: 每文件 ~1100 B(对象头+页索引+目录项), 按文件数计。
# 由 16k 全量(859文件, 数据5.87MB, 镜像6.81MB)推算: (6.81-5.87)MB/859 ≈ 1080 B/文件,
# 取偏高值 1100 留安全余量, 避免打包仍超分区。
OVERHEAD_PER_FILE = 1100


def main() -> int:
    EO.OUT_DIR.mkdir(parents=True, exist_ok=True)
    ffmpeg = EO.find_ffmpeg()
    print(f"混合编码: 主 {HI_BITRATE}k / 降级 {LO_BITRATE}k, 分区 {PARTITION_SIZE/1048576:.2f}MB")

    # 清掉旧 dir*
    for old in EO.OUT_DIR.iterdir():
        if old.is_dir() and old.name.startswith("dir"):
            shutil.rmtree(old)

    # --- 第一遍: 全 HI_BITRATE 编码, 记录每段 (dir, ascii, src, dur, bytes) ---
    dirs = []
    dir_id = 0
    all_files = []   # 扁平: {"dir_entry", "ascii_dir", "src", "out", "dur", "bytes", "bitrate"}
    total = 0
    for d in sorted(p for p in EO.SRC_DIR.iterdir() if p.is_dir()):
        files = sorted(p for p in d.iterdir() if p.suffix.lower() in (".mp3", ".ogg", ".wav"))
        if not files:
            continue
        ascii_dir = f"dir{dir_id:02d}"
        out_dir = EO.OUT_DIR / ascii_dir
        out_dir.mkdir(parents=True, exist_ok=True)
        dirent = {"dir": EV.clean_display_name(d.name), "dir_id": dir_id,
                  "dir_ascii": ascii_dir, "files": []}
        for i, f in enumerate(files):
            ascii_name = f"clip{i:02d}.opus"
            out = out_dir / ascii_name
            try:
                nb = EO.encode_opus(ffmpeg, f, out, HI_BITRATE)
            except RuntimeError as e:
                print(f"  [skip] {f.name}: {e}", file=sys.stderr)
                continue
            dur = EO._probe_duration(ffmpeg, f)
            rel = f"{ascii_dir}/{ascii_name}"
            rec = {"name": EV.clean_display_name(f.stem), "src": f.name, "path": rel,
                   "samples": int(dur * EO.SAMPLE_RATE), "duration": round(dur, 3),
                   "compressed_bytes": nb, "_dur": dur, "_out": out, "_bitrate": HI_BITRATE,
                   "_srcpath": str(f)}
            dirent["files"].append(rec)
            all_files.append(rec)
            total += nb
        dirs.append(dirent)
        dir_id += 1

    n_files = len(all_files)
    overhead = n_files * OVERHEAD_PER_FILE
    budget = PARTITION_SIZE - overhead
    print(f"第一遍 12k: {n_files} 段, 数据 {total/1048576:.2f}MB + 元数据 {overhead/1048576:.2f}MB "
          f"= {(total+overhead)/1048576:.2f}MB / 分区 {PARTITION_SIZE/1048576:.2f}MB")
    if total <= budget:
        print("预算内, 全部保持 12k")
    else:
        # 超预算: 按时长降序, 把最长的段降 LO_BITRATE, 直到 total <= budget
        need = total - budget
        print(f"超预算 {need/1048576:.2f}MB, 按时长降序降级长段到 {LO_BITRATE}k ...")
        to_downgrade = sorted(all_files, key=lambda r: r["_dur"], reverse=True)
        downgraded = 0
        for rec in to_downgrade:
            if total <= budget:
                break
            old = rec["compressed_bytes"]
            nb = EO.encode_opus(ffmpeg, Path(rec["_srcpath"]), rec["_out"], LO_BITRATE)
            rec["compressed_bytes"] = nb
            rec["_bitrate"] = LO_BITRATE
            total = total - old + nb
            downgraded += 1
            if downgraded % 5 == 0:
                print(f"  已降 {downgraded} 段, 数据 {total/1048576:.2f}MB")
        print(f"降级完成: {downgraded} 段转 {LO_BITRATE}k, 数据 {total/1048576:.2f}MB"
              f" (预算 {budget/1048576:.2f}MB)")

    # 汇总(去掉内部字段)
    tc = sum(e["compressed_bytes"] for d in dirs for e in d["files"])
    tf = sum(len(d["files"]) for d in dirs)
    print(f"编码完成: {tf} 段, 共 {tc/1048576:.2f}MB"
          f" (占分区 {100*tc/PARTITION_SIZE:.1f}%)")

    # 索引: 剔除内部 '_' 下划线字段, 只保留公开字段供 JSON/C 索引使用
    def clean(rec):
        return {k: v for k, v in rec.items() if not k.startswith("_")}
    public_dirs = [{"dir": d["dir"], "dir_id": d["dir_id"],
                    "dir_ascii": d["dir_ascii"],
                    "files": [clean(e) for e in d["files"]]} for d in dirs]
    (EO.OUT_DIR / "voice_index.json").write_text(
        json.dumps(public_dirs, ensure_ascii=False, indent=2), encoding="utf-8")
    EV.gen_c_index(public_dirs, Path("main/voice_index.h"))
    print(f"索引: {EO.OUT_DIR/'voice_index.json'}")

    # 打包镜像
    try:
        EO._build_spiffs_image(dirs)
    except (FileNotFoundError, OSError) as e:
        print(f"提示: spiffsgen 未找到, 跳过镜像: {e}")
    except RuntimeError as e:
        print(f"提示: 镜像生成失败: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
