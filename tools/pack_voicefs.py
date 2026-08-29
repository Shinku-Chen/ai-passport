#!/usr/bin/env python3
"""pack_voicefs.py —— 把已提交的 assets/audio/dir* 打包成 voicefs.img 数据分区镜像。

与 encode_opus.py(重编码, 需源素材 + ffmpeg + numpy)不同, 本工具是**纯打包**:
从仓库里已提交的 assets/audio/dirNN/*.opus(固件可播放的原始裸包流)直接打包,
供 CI / validate.sh 在合并固件时生成完整的数据分区。零额外依赖(仅 stdlib +
ESP-IDF 的 spiffsgen.py), 可在无源素材/无 ffmpeg 的环境复现。

分区大小从 partitions.csv 的 voicefs 行读取(offset, size), 与固件布局保持一致。

用法:
  python tools/pack_voicefs.py
输出:
  assets/audio/voicefs.img
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
AUDIO_DIR = PROJECT_ROOT / "assets" / "audio"
PARTITIONS_CSV = PROJECT_ROOT / "partitions.csv"


def read_partition_size() -> int:
    """从 partitions.csv 读出 voicefs 分区大小(字节)。找不到则用 0x5F0000 兜底。"""
    if PARTITIONS_CSV.is_file():
        for line in PARTITIONS_CSV.read_text(encoding="utf-8").splitlines():
            parts = [p.strip() for p in line.split(",")]
            if len(parts) >= 5 and parts[0] == "voicefs":
                return int(parts[4], 16)  # voicefs, data, spiffs, <offset>, <size>
    return 0x5F0000


def find_spiffsgen() -> str:
    """定位 ESP-IDF 的 spiffsgen.py: 优先 IDF_PATH, 其次是常见安装路径, 最后用 PATH。"""
    idf = os.environ.get("IDF_PATH")
    if idf:
        cand = Path(idf) / "components" / "spiffs" / "spiffsgen.py"
        if cand.is_file():
            return str(cand)
    for pat in (
        r"C:\Espressif\frameworks\esp-idf-*\components\spiffs\spiffsgen.py",
        r"D:\Espressif\frameworks\esp-idf-*\components\spiffs\spiffsgen.py",
        "/opt/esp/idf/components/spiffs/spiffsgen.py",
        "/root/esp/idf/components/spiffs/spiffsgen.py",
    ):
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    on_path = shutil.which("spiffsgen.py")
    if on_path:
        return on_path
    raise SystemExit("未找到 spiffsgen.py —— 请设置 IDF_PATH 或把 ESP-IDF 加入 PATH")


def main() -> int:
    dirs = sorted(p for p in AUDIO_DIR.iterdir() if p.is_dir() and p.name.startswith("dir"))
    if not dirs:
        print("ERROR: assets/audio 下没有 dir* 目录可打包", file=sys.stderr)
        return 1

    size = read_partition_size()
    img_path = AUDIO_DIR / "voicefs.img"
    with tempfile.TemporaryDirectory() as tmp:
        src_root = Path(tmp)
        for d in dirs:
            shutil.copytree(d, src_root / d.name)
        spiffsgen = find_spiffsgen()
        cmd = [sys.executable, spiffsgen, str(size), str(src_root), str(img_path)]
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if res.returncode != 0:
            print(f"ERROR: spiffsgen 失败: {res.stderr or res.stdout}", file=sys.stderr)
            return 1
    print(f"voicefs.img: {img_path} ({img_path.stat().st_size} bytes, 分区 0x{size:x}, "
          f"{len(dirs)} 个 dir)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
