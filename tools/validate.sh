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
    cc -std=c11 -Wall -Wextra -Werror \
        -Itests/gal_stubs -Imain/gal \
        tests/test_gal_state.c main/gal/gal_save.c main/gal/gal_progress.c \
        main/gal/gal_unlock.c tests/gal_stubs/nvs_stub.c \
        -o "$test_dir/test_gal_state"
    "$test_dir/test_gal_state"
    # GalGame 框架:生产 app 的 NewGame/choice/save/load/continue/restart 状态链。
    cc -std=c11 -Wall -Wextra -Werror \
        -Itests/gal_app_stubs -Itests/gal_stubs -Imain/gal \
        tests/test_gal_app_flow.c main/gal/gal_app.c main/gal/gal_save.c \
        main/gal/gal_progress.c main/gal/gal_unlock.c main/gal/gal_settings.c \
        main/gal/gal_content.c main/gal/gal_text.c \
        tests/gal_app_stubs/gal_app_hal_stub.c tests/gal_stubs/nvs_stub.c \
        -o "$test_dir/test_gal_app_flow"
    "$test_dir/test_gal_app_flow"
    # 9-nine- ships its own 16/20px subsets (tests/test_nine_assets.py checks
    # full story/UI coverage); the skeleton's placeholder fonts are not built.
    if [[ ! -f "${repo_root}/assets/nine/kokoiro/nine_symbols.txt" ]]; then
        PYTHONDONTWRITEBYTECODE=1 python3 tools/gal_font.py --check --out-dir assets/fonts
    fi

    # 9-nine- reader: production pack/VM/image/audio/store/app tests.
    local nine_pack_dir="${repo_root}/assets/nine/kokoiro"
    if [[ ! -f "${nine_pack_dir}/nine_script.bin" || ! -f "${nine_pack_dir}/nine_images.bin" ]]; then
        echo "ERROR: missing 9-nine- pack; run tools/nine_pack.py --episode kokoiro" >&2
        return 1
    fi
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_nine_assets.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/nine_host/make_filter_pack.py >/dev/null
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/nine -Itests/nine_host         tests/test_nine_engine.c main/nine/nine_pack.c main/nine/nine_vm.c         tests/nine_host/tinfl_shim.c -o "${test_dir}/test_nine_engine"
    "${test_dir}/test_nine_engine" "${nine_pack_dir}/nine_script.bin"         "tests/nine_host/filter-fallback.bin"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/nine -Itests/nine_host         tests/test_nine_store.c main/nine/nine_pack.c main/nine/nine_vm.c         main/nine/nine_store.c tests/nine_host/nvs_stub.c tests/nine_host/tinfl_shim.c         -o "${test_dir}/test_nine_store"
    "${test_dir}/test_nine_store" "${nine_pack_dir}/nine_script.bin"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DNINE_IMAGE_HOST -Imain/nine -Itests/nine_host         tests/test_nine_image.c main/nine/nine_pack.c main/nine/nine_vm.c         main/nine/nine_image.c main/nine/tjpgd/tjpgd.c tests/nine_host/tinfl_shim.c         -o "${test_dir}/test_nine_image"
    "${test_dir}/test_nine_image" "${nine_pack_dir}/nine_script.bin"         "${nine_pack_dir}/nine_images.bin"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DNINE_IMAGE_HOST -DNINE_MEDIA_HOST         -Imain/nine -Itests/nine_host         tests/test_nine_media.c main/nine/nine_pack.c main/nine/nine_vm.c         main/nine/nine_image.c main/nine/nine_media.c main/nine/tjpgd/tjpgd.c         tests/nine_host/tinfl_shim.c -o "${test_dir}/test_nine_media"
    "${test_dir}/test_nine_media" "${nine_pack_dir}/nine_script.bin"         "${nine_pack_dir}/nine_images.bin"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DNINE_IMAGE_HOST -DNINE_MEDIA_HOST         -Imain -Imain/gal -Imain/nine -Itests/nine_host         tests/test_nine_app.c tests/nine_host/gal_ui_stub.c tests/nine_host/nvs_stub.c         tests/nine_host/tinfl_shim.c main/nine/nine_app.c main/nine/nine_pack.c         main/nine/nine_vm.c main/nine/nine_store.c main/nine/nine_image.c         main/nine/nine_media.c main/nine/tjpgd/tjpgd.c main/gal/gal_text.c         -o "${test_dir}/test_nine_app"
    "${test_dir}/test_nine_app" "${nine_pack_dir}/nine_script.bin"         "${nine_pack_dir}/nine_images.bin"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DNINE_IMAGE_HOST -DNINE_MEDIA_HOST         -Imain -Imain/gal -Imain/nine -Itests/nine_host         tests/test_nine_ff.c tests/nine_host/gal_ui_stub.c tests/nine_host/nvs_stub.c         tests/nine_host/tinfl_shim.c main/nine/nine_app.c main/nine/nine_pack.c         main/nine/nine_vm.c main/nine/nine_store.c main/nine/nine_image.c         main/nine/nine_media.c main/nine/tjpgd/tjpgd.c main/gal/gal_text.c         -o "${test_dir}/test_nine_ff"
    "${test_dir}/test_nine_ff" "${nine_pack_dir}/nine_script.bin"         "${nine_pack_dir}/nine_images.bin"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DNINE_IMAGE_HOST -DNINE_MEDIA_HOST         -Imain -Imain/gal -Imain/nine -Itests/nine_host         tests/test_nine_menu.c tests/nine_host/gal_ui_stub.c tests/nine_host/nvs_stub.c         tests/nine_host/tinfl_shim.c main/nine/nine_app.c main/nine/nine_pack.c         main/nine/nine_vm.c main/nine/nine_store.c main/nine/nine_image.c         main/nine/nine_media.c main/nine/tjpgd/tjpgd.c main/gal/gal_text.c         -o "${test_dir}/test_nine_menu"
    "${test_dir}/test_nine_menu" "${nine_pack_dir}/nine_script.bin"         "${nine_pack_dir}/nine_images.bin"
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

    validation_build_dir="${VALIDATION_BUILD_DIR:-$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)}"
    mkdir -p "${validation_build_dir}"
    echo "Firmware evidence: ${validation_build_dir}"
    # Git Bash passes /d/... paths; the Windows idf.py needs drive-letter form.
    local idf_build_dir="${validation_build_dir}"
    if command -v cygpath >/dev/null 2>&1; then idf_build_dir="$(cygpath -m "${validation_build_dir}")"; fi
    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" idf.py -B "${idf_build_dir}" -D "SDKCONFIG=${idf_build_dir}/sdkconfig" build
    idf.py -B "${idf_build_dir}" merge-bin -o "${idf_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${idf_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create "${idf_build_dir}" --archive-root "${repo_root}/build/firmware"
    mkdir -p "${repo_root}/build"
    install -m 0644 "${idf_build_dir}/FoloToy-AI-Passport-full.bin" "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    # Production-path UI/image startup with real LVGL objects/fonts and pack.
    bash "${repo_root}/tests/nine_realgl/run_gate.sh"
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
