#!/usr/bin/env bash
# 本作 host 回归构建脚本（tracked）。产物写到 $OUT_DIR（默认 build/host），不依赖 build/ 里的旧脚本。
#
# 编译器优先级：ZIG_CC → PATH 上的 zig → python -m ziglang cc → CC → cc
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."

OUT_DIR="${OUT_DIR:-build/host}"
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

FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -DPAPER_PACK_USE_HOST_BLOBS)
INC=(-Itests/host/stubs -Imain -Imain/gal -Imain/paper -Imain/paper/tjpgd)
SRC=(main/paper/paper_pack.c main/paper/paper_game.c main/paper/paper_gfx.c
     main/paper/tjpgd/tjpgd.c)
COMMON=(tests/host/stub_platform.c tests/host/tinfl_shim.c)

echo "== host build ($(basename "$zig_wrapper" 2>/dev/null || echo "$CC")) =="
"${CC_CMD[@]}" "${FLAGS[@]}" "${INC[@]}" "${SRC[@]}" "${COMMON[@]}" tests/host/test_paper.c \
  -o "$OUT_DIR/test_paper.exe"
"$OUT_DIR/test_paper.exe" main/paper_data/paper_img.bin main/paper_data/paper_img_full.bin \
  main/paper_data/paper_scn.bin

"${CC_CMD[@]}" "${FLAGS[@]}" -DPAPER_UI_TEST_HOOKS "${INC[@]}" "${SRC[@]}" main/gal/gal_app.c \
  "${COMMON[@]}" tests/host/stub_lvgl.c tests/host/stub_bsp.c tests/host/test_ui.c \
  -o "$OUT_DIR/test_ui.exe"
"$OUT_DIR/test_ui.exe" main/paper_data/paper_img.bin main/paper_data/paper_scn.bin
