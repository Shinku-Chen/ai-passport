#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local dead_strip_flag

    # Apple ld does not accept --gc-sections; -dead_strip is the equivalent.
    dead_strip_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        dead_strip_flag="-Wl,-dead_strip"
    fi

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/battery_stubs -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_battery.c -o "${test_dir}/test_bsp_battery"
    "${test_dir}/test_bsp_battery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    # GalGame 框架:排版分页纯逻辑 + 中文字体覆盖自检。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_text.c main/gal/gal_text.c \
        -o "${test_dir}/test_gal_text"
    "${test_dir}/test_gal_text"
    # GalGame 框架:方波合成(音乐鉴赏的占位音色)纯逻辑。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_tone.c main/gal/gal_tone.c \
        -o "${test_dir}/test_gal_tone"
    "${test_dir}/test_gal_tone"
    # GalGame 框架:周目 flag 与永久解锁分离、旧 save/progress 兼容。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/gal_stubs -Imain/gal \
        tests/test_gal_state.c main/gal/gal_save.c main/gal/gal_progress.c \
        main/gal/gal_unlock.c tests/gal_stubs/nvs_stub.c \
        -o "$test_dir/test_gal_state"
    "$test_dir/test_gal_state"
    # GalGame 框架:生产 app 的 NewGame/choice/save/load/continue/restart 状态链。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/gal_app_stubs -Itests/gal_stubs -Imain/gal \
        tests/test_gal_app_flow.c main/gal/gal_app.c main/gal/gal_save.c \
        main/gal/gal_progress.c main/gal/gal_unlock.c main/gal/gal_settings.c \
        main/gal/gal_content.c main/gal/gal_text.c \
        tests/gal_app_stubs/gal_app_hal_stub.c tests/gal_stubs/nvs_stub.c \
        -o "$test_dir/test_gal_app_flow"
    "$test_dir/test_gal_app_flow"

    # 9-nine- Episode 4: source-JS vs C VM differential (requires the pinned
    # read-only source checkout; the generated trace is not committed).
    if [[ -n "${NINE_SOURCE:-}" ]] && command -v node >/dev/null 2>&1; then
        node tools/nine_trace.mjs --source "${NINE_SOURCE}" --pack assets/nine/yukiiro \
            --out "$test_dir/nine_trace.bin"
        "${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror \
            -Itests/nine -Imain/nine \
            tests/nine/test_nine_vm_diff.c main/nine/nine_pack.c main/nine/nine_vm.c \
            tests/nine/tinfl_shim.c -o "$test_dir/test_nine_vm_diff"
        "$test_dir/test_nine_vm_diff" assets/nine/yukiiro/nine_script.bin \
            "$test_dir/nine_trace.bin"
        python3 tools/nine_audit.py --source "${NINE_SOURCE}" --episode yukiiro \
            --pack assets/nine/yukiiro --report "$test_dir/nine_audit.json"
    else
        echo "9-nine source differential: SKIP (set NINE_SOURCE to the pinned 9-nine-rw5 checkout)"
    fi

    # 9-nine- Episode 4: production reader state machine on the real pack.
    "${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror \
        -DNINE_READER -DNINE_APP_HOST -DNINE_IMAGE_HOST -DNINE_MEDIA_HOST -DNINE_STORE_HOST \
        -Itests/nine/stubs -Itests/nine -Itests/gal_stubs -Icomponents/bsp/include \
        -Imain -Imain/gal -Imain/nine -Imain/nine/tjpgd \
        tests/nine/test_nine_app.c main/nine/nine_app.c main/nine/nine_pack.c \
        main/nine/nine_vm.c main/nine/nine_store.c main/nine/nine_image.c \
        main/nine/nine_media.c main/nine/tjpgd/tjpgd.c main/gal/gal_text.c \
        main/gal/gal_settings.c tests/nine/nine_ui_stub.c tests/nine/nine_assets_host.c \
        tests/nine/nvs_stub_large.c tests/nine/tinfl_shim.c \
        tests/nine/stubs/esp_heap_caps.c tests/nine/nine_hal_stub.c \
        -o "$test_dir/test_nine_app"
    "$test_dir/test_nine_app"

    # 9-nine- Episode 4: strict choice -> menu -> return regression.
    "${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror         -DNINE_READER -DNINE_APP_HOST -DNINE_IMAGE_HOST -DNINE_MEDIA_HOST -DNINE_STORE_HOST         -Itests/nine/stubs -Itests/nine -Itests/gal_stubs -Icomponents/bsp/include         -Imain -Imain/gal -Imain/nine -Imain/nine/tjpgd         tests/nine/test_nine_menu.c main/nine/nine_app.c main/nine/nine_pack.c         main/nine/nine_vm.c main/nine/nine_store.c main/nine/nine_image.c         main/nine/nine_media.c main/nine/tjpgd/tjpgd.c main/gal/gal_text.c         main/gal/gal_settings.c tests/nine/nine_ui_stub.c tests/nine/nine_assets_host.c         tests/nine/nvs_stub_large.c tests/nine/tinfl_shim.c         tests/nine/stubs/esp_heap_caps.c tests/nine/nine_hal_stub.c         -o "$test_dir/test_nine_menu"
    "$test_dir/test_nine_menu"

    # 9-nine- Episode 4: every pack asset decodes; source-art reference composite.
    "${CC:-cc}" -std=c11 -O1 -Wall -Wextra -Werror -DNINE_IMAGE_HOST \
        -Itests/nine -Imain/nine -Imain/nine/tjpgd \
        tests/nine/test_nine_image.c main/nine/nine_image.c main/nine/nine_pack.c \
        main/nine/tjpgd/tjpgd.c tests/nine/tinfl_shim.c tests/nine/nine_assets_host.c \
        -o "$test_dir/test_nine_image"
    "$test_dir/test_nine_image"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/nine_font_check.py \
        --out assets/nine/yukiiro --source-root main
    if [[ -n "${NINE_LVGL_HOST:-}" ]]; then
        python3 tests/nine/lvgl_host/run.py
    else
        echo "9-nine real LVGL production path: SKIP (set NINE_LVGL_HOST=1)"
    fi
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" "${dead_strip_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_battery_profile.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

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
    echo "Firmware build: PASS"
)

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
