#!/usr/bin/env python3
"""Generate production Cafe Stella media assets (title 4 kHz, OP 8 kHz IMA; OP native 224x126 3fps).

This is a one-time generator using the fixed source assets. It writes only
under assets/cafe/media; the story pack is unchanged until the packer is run.
The OP audio file is kept at its full 93.00 s duration; source guard semantics
are metadata, not encoded by truncating the file.
"""
from __future__ import annotations

import json
import re
import struct
import subprocess
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = Path("C:/tmp/ai-passport-six-20261123-79f0/sources/cafestella-rw5")
FFMPEG = Path("C:/Program Files/ZWSOFT/ZWCAD/ffmpeg.exe")
OUT = ROOT / "assets/cafe/media"
OP_DIR = OUT / "op"
OUT.mkdir(parents=True, exist_ok=True)
OP_DIR.mkdir(parents=True, exist_ok=True)


def run(args):
    return subprocess.run(args, capture_output=True, check=False)


def wav_info(path: Path):
    b = path.read_bytes()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE"
    pos = 12
    fmt = data = None
    while pos + 8 <= len(b):
        cid = b[pos:pos + 4]
        size = struct.unpack_from("<I", b, pos + 4)[0]
        payload = b[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = payload
        elif cid == b"data":
            data = payload
        pos += 8 + size + (size & 1)
    assert fmt and data and len(fmt) >= 16
    tag, channels, rate, byte_rate, block_align, bits = struct.unpack_from("<HHIIHH", fmt)
    return {"format": tag, "channels": channels, "sample_rate": rate,
            "byte_rate": byte_rate, "block_align": block_align, "bits": bits,
            "data": data}


def encode_audio(name: str, rate: int) -> dict:
    src = SRC / "src/common" / f"{name}.opus"
    pcm = run([str(FFMPEG), "-hide_banner", "-loglevel", "error", "-i", str(src),
               "-ac", "1", "-ar", str(rate), "-f", "s16le", "-"])
    assert pcm.returncode == 0, pcm.stderr
    samples = len(pcm.stdout) // 2
    wav = OUT / f"{name}-{rate // 1000}k-ima.wav"
    p = run([str(FFMPEG), "-y", "-hide_banner", "-loglevel", "error", "-i", str(src),
             "-ac", "1", "-ar", str(rate), "-c:a", "adpcm_ima_wav", str(wav)])
    assert p.returncode == 0, p.stderr
    info = wav_info(wav)
    assert info["format"] == 0x11 and info["channels"] == 1 and info["sample_rate"] == rate
    assert info["bits"] == 4 and info["block_align"] > 4
    (OUT / f"{name}-{rate // 1000}k.ima").write_bytes(info["data"])
    return {
        "source": str(src.relative_to(SRC)).replace("\\", "/"),
        "source_bytes": src.stat().st_size,
        "ima": str((OUT / f"{name}-{rate // 1000}k.ima").relative_to(ROOT)).replace("\\", "/"),
        "ima_bytes": len(info["data"]),
        "sample_rate": info["sample_rate"], "channels": 1, "bits": 4,
        "block_align": info["block_align"], "samples": samples,
        "duration_s": samples / float(rate),
    }


title = encode_audio("title-bgm", 4000)
opening = encode_audio("opening-bgm", 8000)

frames = sorted((SRC / "src/common/opening").glob("*/*.jpg"))
assert len(frames) == 477
op_count = (95460 * 3 + 999) // 1000
op_files = []
op_bytes = 0
for k in range(op_count):
    src_i = min(len(frames) - 1, int((k * 1000.0 / 3) * 5 / 1000.0))
    im = Image.open(frames[src_i]).convert("RGB")
    # Keep the source-native 224x126 stage. The old 128x72 q10 path threw away
    # half the source detail before the runtime 320x180 display upscale.
    if im.size != (224, 126):
        im = im.resize((224, 126), Image.Resampling.LANCZOS)
    path = OP_DIR / f"op-{k:03d}.jpg"
    im.save(path, "JPEG", quality=15, optimize=True, subsampling=2)
    op_files.append(str(path.relative_to(ROOT)).replace("\\", "/"))
    op_bytes += path.stat().st_size

media = {
    "schema": 1,
    "source": {"repo": "https://github.com/xuanyide01/cafestella-rw5",
               "commit": "90ddb9e020970907b61ad4181f25c65267e6e98b"},
    "audio": {"title": title, "opening": opening},
    "op": {
        "frames": op_count, "fps": 3, "duration_ms": 95460,
        "audio_track": "opening", "audio_guard_ms": 92400,
        "audio_content_ms": 93000, "source_frames": 477, "source_fps": 5,
        "frame_map": "floor(k*5/3), last held to 95.460 s",
        "frame_files": op_files, "jpeg_bytes": op_bytes,
        "width": 224, "height": 126, "quality": 15,
    },
}
(OUT / "media.json").write_text(json.dumps(media, ensure_ascii=False, indent=2) + "\n",
                                encoding="utf-8")
print(json.dumps({"title": title, "opening": opening, "op_frames": op_count,
                  "op_bytes": op_bytes}, ensure_ascii=False, indent=2))
