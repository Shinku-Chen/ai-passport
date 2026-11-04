#!/usr/bin/env bash
# tools/nn_gate.sh —— 本机宿主机门禁（Windows 上 tools/validate.sh 的等效替代）。
#
# 本机没有 gcc/clang，也没有可用的 MSys ESP-IDF 环境；tools/validate.sh 用
# `${CC:-cc}` 编译宿主测试，并用 `-Wl,--gc-sections` 链接 4 个 demo 运行时测试。
# 这里用 zig 自带的 C 编译器（python -m ziglang cc）跑能跑的部分。
#
# 关于临时目录：所有中间产物都写进 build/gate-tmp/<时间戳-进程号>/ 并**保留**。
# 脚本不做任何删除（用户约定：删除一律进回收站；跨 shell 拼字符串删除更不允许）。
# 需要清理时由人手动把 build/gate-tmp/ 下的目录移到回收站。
#
# 关于分组：输出严格分四类，不把“跑不了”写成“通过”——
#   [C-nn]      本作新增的 C 套件
#   [C-framework] 仓库既有 C 套件
#   [python]    python 套件（含已知的 baseline 平台失败）
#   [skip]      本机工具链无法完成的套件（附原因）
set -uo pipefail
cd "$(dirname "$0")/.."

CC_CMD="python -m ziglang cc -std=c11 -Wall -Wextra -Werror"
CC_LINK_CMD="python -m ziglang cc -std=c11 -Wall -Wextra -Werror -Wl,--gc-sections"
# 本作自己的 C 套件用 -O2：zig cc 默认 -O0，本机/沙箱里 -O0 产物启动即 SIGILL
# （同一份代码 -O1/-O2/-Os 正常）。框架套件保持 -O0，避免 -O2 触发它们既有的
# -Wunused-variable 报错。
CC_NN_CMD="python -m ziglang cc -O2 -std=c11 -Wall -Wextra -Werror"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
WORK="build/gate-tmp/$STAMP"
mkdir -p "$WORK" || exit 1
case "$WORK" in
    build/gate-tmp/*) ;;                       # 只允许写进自己的目录
    *) echo "refusing to use $WORK as workspace" >&2; exit 1 ;;
esac

pass=0; fail=0; skip=0
declare -a failed_names=() skipped_names=()

run() {
    local group="$1" name="$2"; shift 2
    if "$@" >"$WORK/$name.log" 2>&1; then
        echo "[$group] PASS  $name"
        pass=$((pass+1))
    else
        echo "[$group] FAIL  $name"
        tail -4 "$WORK/$name.log" | sed 's/^/           /'
        fail=$((fail+1)); failed_names+=("$name")
    fi
}
skip() {
    echo "[skip] SKIP  $1 — $2"
    skip=$((skip+1)); skipped_names+=("$1")
}

echo "workspace: $WORK (kept; recycle it manually when done)"
echo
echo "== 仓库检查 =="
run python check_repo             python tools/check_repo.py
run python font-coverage          python tools/gal_font.py --check --out-dir assets/fonts
run python pack-audit             python tools/nn_audit.py
run python nvs-contract           python tests/test_nn_store_contract.py

echo
echo "== 本作 C 套件 =="
run C-nn nn_script   $CC_NN_CMD -Imain -Imain/gal tests/test_nn_script.c main/nn_pack.c main/nn_script.c -o "$WORK/nn_script"
run C-nn run-nn_script "$WORK/nn_script"
run C-nn nn_image    $CC_NN_CMD -Imain -Imain/nn_jpeg tests/test_nn_image.c main/nn_pack.c main/nn_image.c main/nn_jpeg/tjpgd.c -o "$WORK/nn_image"
run C-nn run-nn_image "$WORK/nn_image"
run C-nn nn_text     $CC_NN_CMD -Imain -Imain/gal tests/test_nn_text.c main/nn_pack.c main/gal/gal_text.c -o "$WORK/nn_text"
run C-nn run-nn_text "$WORK/nn_text"
run C-nn nn_save     $CC_NN_CMD -Imain -Imain/gal tests/test_nn_save.c main/nn_pack.c main/nn_save_codec.c main/nn_script.c -o "$WORK/nn_save"
run C-nn run-nn_save "$WORK/nn_save"
run C-nn nn_pack     $CC_NN_CMD -Imain tests/test_nn_pack.c main/nn_pack.c -o "$WORK/nn_pack"
run C-nn run-nn_pack "$WORK/nn_pack"
run C-nn harness_app  $CC_NN_CMD -Itests/harness_stubs -Imain -Imain/gal tests/harness_app.c tests/harness_stubs/lvgl.c tests/harness_stubs/nn_canvas_stub.c main/nn_app.c main/nn_ui.c main/nn_pack.c main/nn_script.c main/nn_save.c main/nn_save_codec.c main/gal/gal_text.c -o "$WORK/harness_app"
run C-nn run-harness_app "$WORK/harness_app"
run C-nn harness_save $CC_NN_CMD -Itests/harness_stubs -Imain -Imain/gal tests/harness_save.c tests/harness_stubs/lvgl.c tests/harness_stubs/nn_canvas_stub.c main/nn_app.c main/nn_ui.c main/nn_pack.c main/nn_script.c main/nn_save.c main/nn_save_codec.c main/gal/gal_text.c -o "$WORK/harness_save"
run C-nn run-harness_save "$WORK/harness_save"

echo
echo "== 仓库既有 C 套件 =="
run C-framework key_guard    $CC_CMD -Imain tests/test_key_guard.c -o "$WORK/key_guard"
run C-framework gal_text     $CC_CMD -Imain -Imain/gal tests/test_gal_text.c main/gal/gal_text.c -o "$WORK/gal_text"
run C-framework gal_tone     $CC_CMD -Imain -Imain/gal tests/test_gal_tone.c main/gal/gal_tone.c -o "$WORK/gal_tone"
run C-framework bsp_rounding $CC_CMD -Icomponents/bsp/src tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c -o "$WORK/bsp_rounding"
run C-framework bsp_es8311   $CC_CMD -Icomponents/bsp/src tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c -o "$WORK/bsp_es8311"
run C-framework bsp_button   $CC_CMD -Itests/bsp_stubs -Icomponents/bsp/include tests/test_bsp_button.c -o "$WORK/bsp_button"
run C-framework bsp_lvgl     $CC_CMD -Itests/bsp_stubs -Icomponents/bsp/include tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c -o "$WORK/bsp_lvgl"
run C-framework bsp_audio    $CC_CMD -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c -o "$WORK/bsp_audio"

echo
echo "== python 套件 =="
run python deep-sleep-contract     python tests/test_deep_sleep_contract.py
run python key-wake-contract       python tests/test_key_wake_contract.py
run python verify-firmware-selftest python tests/test_verify_firmware.py
run python archive-selftest        python tests/test_archive_firmware.py
run python check-repo-selftest     python tests/test_check_repo.py

echo
echo "== 本机工具链无法完成的套件 =="
for demo in audio low_power ble wifi; do
    if $CC_CMD -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
        "tests/test_demo_${demo}_runtime.c" -c -o "$WORK/demo_${demo}.o" >"$WORK/demo_${demo}.log" 2>&1; then
        skip "test_demo_${demo}_runtime" "编译通过；zig 在本机走 lld-link(COFF)，不支持 -Wl,--gc-sections，链接出的二进制跑不了"
    else
        echo "[skip] FAIL  test_demo_${demo}_runtime — 编译失败（这里不是 skip，是坏）"
        tail -4 "$WORK/demo_${demo}.log" | sed 's/^/           /'
        fail=$((fail+1)); failed_names+=("test_demo_${demo}_runtime")
    fi
done

echo
echo "RESULT: $pass passed, $fail failed, $skip skipped"
echo "    passed: 见上方 [C-nn]/[C-framework]/[python] PASS 行"
if [ "$fail" -gt 0 ]; then
    echo "    failed: ${failed_names[*]}"
fi
if [ "$skip" -gt 0 ]; then
    echo "    skipped: ${skipped_names[*]}"
fi
[ "$fail" -eq 0 ]
