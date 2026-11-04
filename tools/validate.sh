#!/usr/bin/env bash
# 仓库门禁：逐 case 记录 PASS/FAIL/SKIP，每个 case 独立日志；不再中途 rm 输出或
# 无条件打印 PASS。输出目录每次唯一（build/validate-*），保留全部日志/二进制/负例。
#
# 用法：tools/validate.sh [--all|--static|--firmware]
# 退出码：本作 C 编译/运行或其它真实失败 → 1；仅有已识别的 baseline 链接差异 SKIP
#         → 0 且汇总为 PARTIAL；全部 PASS → 0。
set -uo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tests/validate_lib.sh
source "${repo_root}/tools/tests/validate_lib.sh"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)-$$"
HOST_DIR="${HOST_OUT_DIR:-${repo_root}/build/validate-host-${STAMP}}"
CASE_LOG_DIR="${HOST_DIR}/cases"
FW_DIR="${FW_OUT_DIR:-${repo_root}/build/validate-firmware-${STAMP}}"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

case_actionlint() {
    local bin="${ACTIONLINT_BIN:-}"
    if [[ -z "$bin" ]]; then
        bin="$(command -v actionlint || true)"
    fi
    if [[ -z "$bin" || ! -x "$bin" ]]; then
        bin="$(./tools/install-actionlint.sh)"
    fi
    "$bin" -color .github/workflows/*.yml
}

case_ui_pixel_math() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${HOST_DIR}/test_ui_pixel_math" &&
        "${HOST_DIR}/test_ui_pixel_math"
}

case_demo_navigation() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${HOST_DIR}/test_demo_navigation" &&
        "${HOST_DIR}/test_demo_navigation"
}

case_key_guard() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${HOST_DIR}/test_key_guard" &&
        "${HOST_DIR}/test_key_guard"
}

case_bsp_display_rounding() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${HOST_DIR}/test_bsp_display_rounding" &&
        "${HOST_DIR}/test_bsp_display_rounding"
}

case_bsp_es8311_sleep_check() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${HOST_DIR}/test_bsp_es8311_sleep_check" &&
        "${HOST_DIR}/test_bsp_es8311_sleep_check"
}

case_bsp_button() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${HOST_DIR}/test_bsp_button" &&
        "${HOST_DIR}/test_bsp_button"
}

case_bsp_lvgl_init() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${HOST_DIR}/test_bsp_lvgl_init" &&
        "${HOST_DIR}/test_bsp_lvgl_init"
}

case_bsp_audio_recovery() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${HOST_DIR}/test_bsp_audio_recovery" &&
        "${HOST_DIR}/test_bsp_audio_recovery"
}

case_gal_text() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_text.c main/gal/gal_text.c \
        -o "${HOST_DIR}/test_gal_text" &&
        "${HOST_DIR}/test_gal_text"
}

case_gal_tone() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_tone.c main/gal/gal_tone.c \
        -o "${HOST_DIR}/test_gal_tone" &&
        "${HOST_DIR}/test_gal_tone"
}

case_parquet_gate() {
    OUT_DIR="${HOST_DIR}/parquet-host" \
        FIXTURES_DIR="${HOST_DIR}/parquet-fixtures" \
        PARQUET_GATE_LOG="${CASE_LOG_DIR}/parquet-gate-inner.log" \
        bash tools/parquet_gate.sh
}

case_demo_runtime() {
    local demo="$1"
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror \
        -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
        "tests/test_demo_${demo}_runtime.c" -Wl,--gc-sections \
        -o "${HOST_DIR}/test_demo_${demo}_runtime" &&
        "${HOST_DIR}/test_demo_${demo}_runtime"
}

run_static_checks() {
    local demo
    mkdir -p "${CASE_LOG_DIR}"
    printf 'host output: %s\n' "$(_abs_path "$HOST_DIR")"
    pick_host_cc

    record_case repo-check python3 tools/check_repo.py
    record_case actionlint case_actionlint
    record_case ui-pixel-math case_ui_pixel_math
    record_case demo-navigation case_demo_navigation
    record_case key-guard case_key_guard
    record_case bsp-display-rounding case_bsp_display_rounding
    record_case bsp-es8311-sleep-check case_bsp_es8311_sleep_check
    record_case bsp-button case_bsp_button
    record_case bsp-lvgl-init case_bsp_lvgl_init
    record_case bsp-audio-recovery case_bsp_audio_recovery
    record_case gal-text case_gal_text
    record_case gal-tone case_gal_tone

    # 本作 gate：拒绝在缺少已入库资源/源快照时静默通过，明确 SKIP 并计 PARTIAL。
    if [[ -f main/parquet_data/parquet_img.bin &&
          -f main/parquet_data/parquet_scn.bin &&
          -d assets/parquet-source &&
          -x tools/parquet_gate.sh ]]; then
        record_case parquet-gate case_parquet_gate
    else
        skip_case parquet-gate "missing parquet packs or tracked source snapshot"
    fi

    record_case font-check python3 tools/gal_font.py --check --out-dir assets/fonts --sizes 16,20

    for demo in audio low_power ble wifi; do
        record_case_any "demo-${demo}-runtime" case_demo_runtime "$demo"
    done

    record_case deep-sleep-contract python3 tests/test_deep_sleep_contract.py
    record_case key-wake-contract python3 tests/test_key_wake_contract.py
    record_case check-repo-selftest python3 tests/test_check_repo.py
    record_case verify-firmware-selftest python3 tests/test_verify_firmware.py
    record_case archive-firmware-selftest python3 tests/test_archive_firmware.py
    record_case install-skills-selftest python3 tests/test_install_passport_skills.py

    case_summary "Host tests"
}

run_firmware_checks() {
    local log_dir="${FW_DIR}/logs"
    mkdir -p "$log_dir"
    printf 'firmware output: %s\n' "$(_abs_path "$FW_DIR")"

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "Firmware build: SKIP (idf.py is not available; activate ESP-IDF 5.5.3)"
        return 0
    fi

    if ! SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${FW_DIR}" -D "SDKCONFIG=${FW_DIR}/sdkconfig" build \
        >"${log_dir}/build.log" 2>&1; then
        echo "Firmware build: FAIL (build) | $(_abs_path "${log_dir}/build.log")"
        grep -m1 -E 'error:|ERROR' "${log_dir}/build.log" || true
        return 1
    fi
    if ! idf.py -B "${FW_DIR}" merge-bin \
        -o "${FW_DIR}/FoloToy-AI-Passport-full.bin" >"${log_dir}/merge.log" 2>&1; then
        echo "Firmware build: FAIL (merge-bin) | $(_abs_path "${log_dir}/merge.log")"
        return 1
    fi
    if ! python3 tools/verify_firmware.py "${FW_DIR}" >"${log_dir}/verify.log" 2>&1; then
        echo "Firmware build: FAIL (verify) | $(_abs_path "${log_dir}/verify.log")"
        grep -m1 -E 'ERROR|PASS|FAIL' "${log_dir}/verify.log" || true
        return 1
    fi
    if ! PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${FW_DIR}" --archive-root "${repo_root}/build/firmware" \
        >"${log_dir}/archive.log" 2>&1; then
        echo "Firmware build: FAIL (archive) | $(_abs_path "${log_dir}/archive.log")"
        return 1
    fi
    # 稳定的便捷副本只在不存在时写，避免覆盖既有通过证据。
    if [[ ! -e "${repo_root}/build/FoloToy-AI-Passport-full.bin" ]]; then
        install -m 0644 "${FW_DIR}/FoloToy-AI-Passport-full.bin" \
            "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    fi
    echo "Firmware build: PASS | $(_abs_path "${FW_DIR}") | $(_abs_path "${log_dir}")"
    return 0
}

cd "$repo_root"
case "$mode" in
    --static)
        run_static_checks
        exit $?
        ;;
    --firmware)
        run_firmware_checks
        exit $?
        ;;
    --all)
        static_rc=0
        firmware_rc=0
        run_static_checks || static_rc=1
        run_firmware_checks || firmware_rc=1
        if [[ "$static_rc" -ne 0 || "$firmware_rc" -ne 0 ]]; then
            exit 1
        fi
        ;;
    *)
        usage
        exit 2
        ;;
esac
