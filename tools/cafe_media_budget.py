#!/usr/bin/env python3
"""Measure the real audio/OP media budget candidates (isolated, no firmware change)."""
from __future__ import annotations

import json
import os
import re
import struct
import subprocess
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = Path("C:/tmp/ai-passport-six-20261123-79f0/sources/cafestella-rw5")
OUT = ROOT / "reports/cafestella/media"
FFMPEG = Path(os.environ.get("FFMPEG", r"C:/Program Files/ZWSOFT/ZWCAD/ffmpeg.exe"))
OUT.mkdir(parents=True, exist_ok=True)

def run(args):
    return subprocess.run(args, capture_output=True, text=True)

def probe(path):
    p = run([str(FFMPEG), "-hide_banner", "-i", str(path)])
    dur = re.search(r"Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)", p.stderr)
    sec = int(dur.group(1)) * 3600 + int(dur.group(2)) * 60 + float(dur.group(3)) if dur else None
    audio = re.search(r"Audio:\s*(\w+),\s*(\d+) Hz,\s*(\w+)", p.stderr)
    return sec, (audio.groups() if audio else None), p.stderr

audio = {}
for name in ("title-bgm", "opening-bgm"):
    path = SRC / "src/common" / f"{name}.opus"
    sec, stream, _ = probe(path)
    pcm = subprocess.run([str(FFMPEG), "-hide_banner", "-loglevel", "error", "-i", str(path),
                          "-ac", "1", "-ar", "8000", "-f", "s16le", "-"],
                         capture_output=True)
    samples = len(pcm.stdout) // 2
    wav = OUT / f"{name}-8k-ima.wav"
    subprocess.run([str(FFMPEG), "-y", "-hide_banner", "-loglevel", "error", "-i", str(path),
                    "-ac", "1", "-ar", "8000", "-c:a", "adpcm_ima_wav", str(wav)],
                   check=True)
    audio[name] = {
        "source": str(path.relative_to(SRC)).replace("\\", "/"),
        "source_bytes": path.stat().st_size,
        "duration_s": round(sec, 3) if sec else None,
        "stream": stream,
        "pcm_8k_mono_samples": samples,
        "ima_wav_bytes": wav.stat().st_size,
        "ima_payload_bytes": wav.stat().st_size - 44,
    }

frames = sorted((SRC / "src/common/opening").glob("*/*.jpg"))
opening = {"frames": len(frames), "source_dir": "src/common/opening",
           "source_bytes": sum(f.stat().st_size for f in frames),
           "fps": 5, "duration_ms": 95460, "source_size": [224, 126], "source_quality": 30}
for size, q in [((160, 90), 20), ((160, 90), 15), ((160, 90), 10), ((128, 72), 10)]:
    total = 0
    for f in frames:
        im = Image.open(f).convert("RGB")
        if im.size != size:
            im = im.resize(size, Image.Resampling.LANCZOS)
        import io
        b = io.BytesIO()
        im.save(b, "JPEG", quality=q, optimize=True, subsampling=2)
        total += len(b.getvalue())
    opening[f"{size[0]}x{size[1]}_q{q}_bytes"] = total

report = {"audio": audio, "opening": opening,
          "fit_note": "Compare against the final app-partition free space; see firmware manifest."}
(OUT / "budget.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps(report, ensure_ascii=False, indent=2))
