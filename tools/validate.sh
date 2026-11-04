#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

GATE_PASS=0
GATE_FAIL=0
GATE_SKIP=0
gate_run() {
    if "$@"; then
        GATE_PASS=$((GATE_PASS + 1))
    else
        GATE_FAIL=$((GATE_FAIL + 1))
        return 1
    fi
}
gate_summary() {
    echo "Gate summary: PASS=${GATE_PASS} FAIL=${GATE_FAIL} SKIP=${GATE_SKIP}"
}
# Summary only: this trap never deletes logs, binaries, probes or archives.
trap 'rc=$?; if (( rc != 0 && GATE_FAIL == 0 )); then GATE_FAIL=1; fi; gate_summary; exit $rc' EXIT

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local local_skips=()
    local partial=0
    note_skip() { local_skips+=("$1"); partial=1; GATE_SKIP=$((GATE_SKIP + 1)); echo "SKIP: $1"; }
    local actionlint_bin
    local test_dir

    gate_run python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    gate_run "${actionlint_bin}" -color .github/workflows/*.yml

    mkdir -p "${repo_root}/build"
    test_dir="$(mktemp -d "${repo_root}/build/host-tests.XXXXXX")"
    echo "Host test artifacts: ${test_dir}"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    gate_run "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    gate_run "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    gate_run "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    gate_run "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    gate_run "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    gate_run "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    gate_run "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    gate_run "${test_dir}/test_bsp_audio_recovery"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" tests/demo_stubs/demo_missing_stubs.c \
            -o "${test_dir}/test_demo_${demo}_runtime"
        gate_run "${test_dir}/test_demo_${demo}_runtime"
    done
    gate_run env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    gate_run env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    # 本作:《魔法少女的魔女审判》阅读器宿主测试(纯逻辑 + 真实包 + 真实 TJpgDec)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_manosaba_text.c main/gal_text.c -o "${test_dir}/test_manosaba_text"
    gate_run "${test_dir}/test_manosaba_text"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_pack_bad.c main/gal_pack.c -o "${test_dir}/test_gal_pack_bad"
    gate_run "${test_dir}/test_gal_pack_bad" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_pack_security.c main/gal_pack.c -o "${test_dir}/test_gal_pack_security"
    gate_run "${test_dir}/test_gal_pack_security" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_book.c main/gal_pack.c main/gal_text.c -o "${test_dir}/test_gal_book"
    gate_run "${test_dir}/test_gal_book" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_damage.c         main/gal_pack.c main/gal_expr.c main/gal_vm.c -o "${test_dir}/test_gal_damage"
    gate_run "${test_dir}/test_gal_damage" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_sprites.c         main/gal_pack.c main/gal_expr.c main/gal_vm.c -o "${test_dir}/test_gal_sprites"
    gate_run "${test_dir}/test_gal_sprites" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_direct_book.c         main/gal_pack.c main/gal_expr.c main/gal_vm.c -o "${test_dir}/test_gal_direct_book"
    gate_run "${test_dir}/test_gal_direct_book" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_map_value.c         main/gal_pack.c main/gal_expr.c main/gal_vm.c -o "${test_dir}/test_gal_map_value"
    gate_run "${test_dir}/test_gal_map_value" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain         tests/test_gal_save_capacity.c main/gal_pack.c main/gal_expr.c main/gal_vm.c         main/gal_save.c main/gal_store.c main/gal_text.c         tests/esp_stubs/esp_err_stub.c tests/esp_stubs/esp_timer_stub.c tests/esp_stubs/nvs_stub.c         -o "${test_dir}/test_gal_save_capacity"
    gate_run bash -c '"$0" "$1" > "$2" 2>&1' "${test_dir}/test_gal_save_capacity" main/gal_data/manosaba.pack "${test_dir}/save-capacity-witness.log"
    gate_run python3 tests/test_nvs_capacity.py --witness-log "${test_dir}/save-capacity-witness.log" --partitions partitions.csv
    gate_run python3 tests/test_nvs_capacity_negative.py "${test_dir}/save-capacity-witness.log"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain tests/test_gal_store_status.c         main/gal_store.c tests/esp_stubs/esp_err_stub.c tests/esp_stubs/nvs_stub.c         -o "${test_dir}/test_gal_store_status"
    gate_run "${test_dir}/test_gal_store_status"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain tests/test_gal_save.c         main/gal_pack.c main/gal_expr.c main/gal_vm.c main/gal_save.c main/gal_store.c         main/gal_unlock.c main/gal_text.c         tests/esp_stubs/esp_err_stub.c tests/esp_stubs/esp_timer_stub.c tests/esp_stubs/nvs_stub.c         -o "${test_dir}/test_gal_save"
    gate_run "${test_dir}/test_gal_save" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_routes.c         main/gal_pack.c main/gal_expr.c main/gal_vm.c -o "${test_dir}/test_gal_routes"
    gate_run "${test_dir}/test_gal_routes" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/lvgl_stubs -Itests/esp_stubs -Imain         -Itests/vendor_tjpgd tests/test_gal_image_real.c main/gal_pack.c main/gal_image.c         tests/lvgl_stubs/lvgl_stub.c tests/esp_stubs/esp_err_stub.c tests/esp_stubs/esp_timer_stub.c         tests/esp_stubs/jpeg_real.c tests/vendor_tjpgd/tjpgd.c -o "${test_dir}/test_gal_image_real"
    gate_run "${test_dir}/test_gal_image_real" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/lvgl_stubs -Itests/esp_stubs -Itests/app_stubs -Imain         tests/test_gal_app.c main/gal_pack.c main/gal_expr.c main/gal_vm.c main/gal_save.c         main/gal_store.c main/gal_unlock.c main/gal_text.c main/gal_image.c main/gal_ui.c main/gal_app.c         tests/lvgl_stubs/lvgl_stub.c tests/esp_stubs/esp_err_stub.c tests/esp_stubs/esp_timer_stub.c         tests/esp_stubs/jpeg_stub.c tests/esp_stubs/nvs_stub.c tests/app_stubs/bsp_stub.c         -o "${test_dir}/test_gal_app"
    gate_run "${test_dir}/test_gal_app" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/lvgl_stubs -Itests/esp_stubs -Itests/app_stubs -Imain         tests/test_gal_app_pages.c main/gal_pack.c main/gal_expr.c main/gal_vm.c main/gal_save.c         main/gal_store.c main/gal_unlock.c main/gal_text.c main/gal_image.c main/gal_ui.c main/gal_app.c         tests/lvgl_stubs/lvgl_stub.c tests/esp_stubs/esp_err_stub.c tests/esp_stubs/esp_timer_stub.c         tests/esp_stubs/jpeg_stub.c tests/esp_stubs/nvs_stub.c tests/app_stubs/bsp_stub.c         -o "${test_dir}/test_gal_app_pages"
    gate_run "${test_dir}/test_gal_app_pages" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/lvgl_stubs -Itests/esp_stubs -Itests/app_stubs -Imain         tests/test_gal_checkpoint.c main/gal_pack.c main/gal_expr.c main/gal_vm.c main/gal_save.c         main/gal_store.c main/gal_unlock.c main/gal_text.c main/gal_image.c main/gal_ui.c main/gal_app.c         tests/lvgl_stubs/lvgl_stub.c tests/esp_stubs/esp_err_stub.c tests/esp_stubs/esp_timer_stub.c         tests/esp_stubs/jpeg_stub.c tests/esp_stubs/nvs_stub.c tests/app_stubs/bsp_stub.c         -o "${test_dir}/test_gal_checkpoint"
    gate_run "${test_dir}/test_gal_checkpoint" main/gal_data/manosaba.pack
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/lvgl_stubs -Itests/esp_stubs -Itests/app_stubs -Imain         tests/test_gal_open.c main/gal_pack.c main/gal_expr.c main/gal_vm.c main/gal_save.c         main/gal_store.c main/gal_unlock.c main/gal_text.c main/gal_image.c main/gal_ui.c main/gal_app.c         tests/lvgl_stubs/lvgl_stub.c tests/esp_stubs/esp_err_stub.c tests/esp_stubs/esp_timer_stub.c         tests/esp_stubs/jpeg_stub.c tests/esp_stubs/nvs_stub.c tests/app_stubs/bsp_stub.c         -o "${test_dir}/test_gal_open"
    gate_run "${test_dir}/test_gal_open" main/gal_data/manosaba.pack
    gate_run python3 tools/manosaba_font.py --check --pack main/gal_data/manosaba.pack --source-root main --out-dir assets/fonts
    gate_run python3 tests/test_font_negative.py
    gate_run python3 tests/test_gate_negative.py --cc "${CC:-cc}" --include main
    if [[ -n "${MANOSABA_SOURCE:-}" && -d "${MANOSABA_SOURCE}" ]]; then
        gate_run env PYTHONDONTWRITEBYTECODE=1 python3 tools/manosaba_audit.py             --source "${MANOSABA_SOURCE}" --pack main/gal_data/manosaba.pack             --report build/manosaba-audit.json
    else
        note_skip "manosaba source audit (MANOSABA_SOURCE 未设置)"
    fi
    gate_run env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    gate_run env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    gate_run env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    if python3 -c "import os,sys; sys.exit(0 if os.name != 'nt' else 1)"; then
        gate_run env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    else
        note_skip "install_passport_skills symlink tests (Windows)"
    fi
    if (( partial )); then
        echo "Host tests: PARTIAL (${local_skips[*]})"
    else
        echo "Host tests: PASS"
    fi
    echo "Host test artifacts kept: ${test_dir}"
}

run_firmware_checks() {
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    mkdir -p "${repo_root}/build"
    validation_build_dir="$(mktemp -d "${repo_root}/build/firmware-build.XXXXXX")"
    echo "Firmware build artifacts: ${validation_build_dir}"

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    GATE_PASS=$((GATE_PASS + 1))
    echo "Firmware build: PASS"
    echo "Firmware build artifacts kept: ${validation_build_dir}"
}

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
