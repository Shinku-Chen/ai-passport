#!/usr/bin/env bash
# Prefer the 32-bit (target-like pointer size) smoke; fall back to 64-bit when
# the ziglang toolchain is unavailable. Both run the real image/app/UI paths.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
if python3 -m ziglang version >/dev/null 2>&1; then
    NINE_REALGL_OUT="${root}/build/nine/realgl_gate" \
    NINE_LV_CONF_DIR="${root}/tests/nine_realgl/cfg48" \
    CC="${root}/tests/nine_realgl/zigcc32.sh" \
        bash "${root}/tests/nine_realgl/run.sh"
else
    echo "WARN: ziglang unavailable; using the 64-bit real-LVGL proxy (72 KB pool)"
    CC="${CC:-cc}" bash "${root}/tests/nine_realgl/run.sh"
fi
