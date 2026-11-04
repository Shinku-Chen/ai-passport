#!/usr/bin/env bash
# 门禁负例：图片测试编译失败必须让 gate 非零退出。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."

STAMP="$(date -u +%Y%m%dT%H%M%SZ)-$$"
OUT_DIR="${OUT_DIR:-build/host-negative-${STAMP}}"
mkdir -p "$OUT_DIR"

zig_wrapper=""
if [[ -n "${ZIG_CC:-}" ]]; then
  zig_wrapper="$ZIG_CC"
elif command -v zig >/dev/null 2>&1; then
  zig_wrapper="$(command -v zig)"
elif python -c "import ziglang" >/dev/null 2>&1; then
  zig_wrapper="$OUT_DIR/zigcc.sh"
  [[ -x "$zig_wrapper" ]] || {
    echo '#!/usr/bin/env bash'
    echo 'exec python -m ziglang cc "$@"'
  } > "$zig_wrapper"
  chmod +x "$zig_wrapper"
fi
if [[ -n "$zig_wrapper" ]]; then
  CC_CMD=("$zig_wrapper" -target x86_64-windows-gnu)
else
  CC_CMD=("${CC:-cc}")
fi

LOG="$OUT_DIR/negative-compile.log"
if "${CC_CMD[@]}" -std=c11 -Itests/host/stubs -Imain -Imain/gal -Imain/parquet \
    -c tests/parquet/negative/image_gate_must_fail.c -o "$OUT_DIR/neg.o" \
    >"$LOG" 2>&1; then
  echo "negative gate: FAIL（故意缺头的图片测试竟然编译成功）" | tee -a "$LOG"
  exit 1
fi
echo "negative gate: PASS（编译失败已被正确判为非零退出）"
