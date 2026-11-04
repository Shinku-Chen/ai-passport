#!/usr/bin/env bash
# 《PARQUET》host 回归构建（tracked）。
# 默认输出到每次唯一的 build/host-<UTC>-$$；显式 OUT_DIR 时尊重调用方选择。
# 编译器：ZIG_CC → zig → python -m ziglang cc → CC → cc；不依赖被忽略的个人 wrapper。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."

STAMP="$(date -u +%Y%m%dT%H%M%SZ)-$$"
OUT_DIR="${OUT_DIR:-build/host-${STAMP}}"
FIXTURES_DIR="${FIXTURES_DIR:-tests/parquet/fixtures}"
mkdir -p "$OUT_DIR"

zig_wrapper=""
if [[ -n "${ZIG_CC:-}" ]]; then
  zig_wrapper="$ZIG_CC"
elif command -v zig >/dev/null 2>&1; then
  zig_wrapper="$(command -v zig)"
elif python -c "import ziglang" >/dev/null 2>&1; then
  zig_wrapper="$OUT_DIR/zigcc.sh"
  {
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

FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -DPQ_PACK_USE_HOST_BLOBS -DPQ_GFX_TEST_HOOKS)
INC=(-Itests/host/stubs -Imain -Imain/gal -Imain/parquet -Imain/parquet/tjpgd)
SRC=(main/parquet/parquet_pack.c main/parquet/parquet_inflate.c main/parquet/parquet_game.c main/parquet/parquet_gfx.c
     main/parquet/tjpgd/tjpgd.c)
COMMON=(tests/host/stub_platform.c tests/host/tinfl_shim.c)
PACKS=(main/parquet_data/parquet_img.bin main/parquet_data/parquet_scn.bin)

echo "== parquet host build ($(basename "$zig_wrapper" 2>/dev/null || echo "${CC:-cc}")) =="
echo "out: $(cd -- "$OUT_DIR" && pwd)"
"${CC_CMD[@]}" "${FLAGS[@]}" "${INC[@]}" "${SRC[@]}" "${COMMON[@]}" \
  tests/host/test_parquet.c -o "$OUT_DIR/test_parquet.exe"
"$OUT_DIR/test_parquet.exe" "${PACKS[@]}" "${FIXTURES_DIR}/images.txt"

# 坏包边界负例（生产 pq_pack_init / pq_text_get 原样链接，只改内存副本）
"${CC_CMD[@]}" "${FLAGS[@]}" "${INC[@]}" main/parquet/parquet_pack.c \
  main/parquet/parquet_inflate.c "${COMMON[@]}" tests/host/test_pack_negative.c \
  -o "$OUT_DIR/test_pack_negative.exe"
"$OUT_DIR/test_pack_negative.exe" "${PACKS[@]}"

"${CC_CMD[@]}" "${FLAGS[@]}" -DPQ_UI_TEST_HOOKS "${INC[@]}" "${SRC[@]}" \
  main/gal/gal_app.c "${COMMON[@]}" tests/host/stub_lvgl.c tests/host/stub_bsp.c \
  tests/host/test_ui.c -o "$OUT_DIR/test_ui.exe"
"$OUT_DIR/test_ui.exe" "${PACKS[@]}"

echo "parquet_host_build: PASS | out=$(cd -- "$OUT_DIR" && pwd)"
