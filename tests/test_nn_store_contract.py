#!/usr/bin/env python3
"""Contract test: the save layer must never wipe the shared NVS partition.

Why this exists
---------------
`nvs_flash_erase()` erases the whole NVS partition, not just one namespace. On
this device NVS is shared with other applications and older games, so calling it
from a reader that merely failed to open its own namespace would silently delete
somebody else's saves. NVS cannot be simulated in a host test, so the invariant is
checked at the source level instead:

  * `main/nn_save.c` must not contain `nvs_flash_erase` (nor `nvs_flash_deinit`
    followed by an erase);
  * the degraded path must be explicit: a state enum, a reason string and an
    error log, with the player still able to read;
  * save records must go through the codec (magic/version/length checked), not be
    memcpy'd straight out of NVS.

Exit code 0 = contract holds, 1 = violation (with the offending line).
"""

from __future__ import annotations

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FORBIDDEN = [
    "nvs_flash_erase",
    "nvs_flash_deinit",
]

REQUIRED = {
    "main/nn_save.c": [
        "nn_store_state_t",          # degraded-state enum
        "NN_STORE_BROKEN",           # the "partition needs a human" case
        "nn_store_reason",           # human-readable reason for logs/UI
        "nn_save_encode",            # records are encoded, not memcpy'd
        "nn_save_decode",            # and decoded with validation
        "ESP_LOGE",                  # the failure is logged, not swallowed
    ],
    "main/nn_save_codec.c": [
        "NN_SAVE_RECORD_MAGIC",
        "NN_SAVE_RECORD_VERSION",
        "nn_save_record_fits_pack",
    ],
    "main/nn_save.h": [
        "nn_store_available",
        "nn_store_reason",
    ],
}


def strip_comments(text: str) -> str:
    """去掉行注释与块注释，避免把“说明为什么不能擦”的注释当成违规调用。"""
    out = []
    i = 0
    n = len(text)
    dq = chr(34)
    sq = chr(39)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif text.startswith("//", i):
            j = text.find(chr(10), i)
            i = n if j < 0 else j
            out.append(" ")
        elif text[i] == dq or text[i] == sq:
            quote = text[i]
            out.append(text[i])
            i += 1
            while i < n:
                if text[i] == chr(92):
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)

def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def main() -> int:
    for path in ("main/nn_save.c", "main/nn_save.h", "main/nn_save_codec.c", "main/nn_save_codec.h"):
        full = os.path.join(ROOT, path)
        if not os.path.exists(full):
            fail(f"{path} is missing")
        text = strip_comments(open(full, encoding="utf-8").read())
        for needle in FORBIDDEN:
            if needle in text:
                line = next(i for i, ln in enumerate(text.splitlines(), 1) if needle in ln)
                fail(f"{path}:{line} uses {needle} — the shared NVS partition must never be erased")

    for path, needles in REQUIRED.items():
        text = open(os.path.join(ROOT, path), encoding="utf-8").read()
        for needle in needles:
            if needle not in text:
                fail(f"{path} no longer provides {needle}")

    # 存档模块不能直接把结构体写进 NVS
    save_c = strip_comments(open(os.path.join(ROOT, "main/nn_save.c"), encoding="utf-8").read())
    if "sizeof(nn_save_t)" in save_c.replace(" ", "") and "nn_save_encode" not in save_c:
        fail("main/nn_save.c writes nn_save_t directly instead of using the codec")

    print("nvs contract: PASS (no global erase, degraded path present, records encoded)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
