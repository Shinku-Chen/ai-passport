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
    # Cafe Stella: 三键短/长按、NVS 坏 blob、源 engine.js 轨迹、横屏应用状态机、图像参考合成。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/cafe         tests/test_cafe_input.c main/cafe/cafe_input.c -o "${test_dir}/test_cafe_input"
    "${test_dir}/test_cafe_input"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/gal_stubs -Itests/cafe_stubs -Imain/cafe         tests/test_cafe_store.c main/cafe/cafe_store.c main/cafe/cafe_compat.c         tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c         -o "${test_dir}/test_cafe_store"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_stubs -Imain/cafe         tests/test_cafe_inflate.c main/cafe/cafe_compat.c tests/cafe_stubs/tinfl_shim.c         -o "${test_dir}/test_cafe_inflate"
    "${test_dir}/test_cafe_inflate" assets/cafe/cafe.pak
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-format-truncation -Wno-unused-function         -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal         tests/test_cafe_recall.c main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c         main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c main/gal/gal_text.c         tests/cafe_app_stubs/cafe_ui_audit_stub.c tests/cafe_app_stubs/cafe_image_stub.c         tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c         -o "${test_dir}/test_cafe_recall"
    "${test_dir}/test_cafe_recall" assets/cafe/cafe.pak
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-format-truncation -Wno-unused-function         -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal         tests/test_cafe_ff.c main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c         main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c main/gal/gal_text.c         tests/cafe_app_stubs/cafe_ui_audit_stub.c tests/cafe_app_stubs/cafe_image_stub.c         tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c         -o "${test_dir}/test_cafe_ff"
    "${test_dir}/test_cafe_ff" assets/cafe/cafe.pak
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-format-truncation -Wno-unused-function         -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal         tests/test_cafe_layout.c main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c         main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c main/gal/gal_text.c         tests/cafe_app_stubs/cafe_ui_stub.c tests/cafe_app_stubs/cafe_image_stub.c         tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c         -o "${test_dir}/test_cafe_layout"
    "${test_dir}/test_cafe_layout" assets/cafe/cafe.pak
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Wno-format-truncation -Wno-unused-function         -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal         tests/test_cafe_ff_cache.c main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c         main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c main/cafe/cafe_image.c         main/cafe/tjpgd.c main/gal/gal_text.c tests/cafe_app_stubs/cafe_ui_stub.c         tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c         -o "${test_dir}/test_cafe_ff_cache"
    "${test_dir}/test_cafe_ff_cache" assets/cafe/cafe.pak
    "${test_dir}/test_cafe_store"
    if [[ -f assets/cafe/media/title-bgm-4k.ima && -f assets/cafe/media/opening-bgm-8k.ima ]]; then
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/cafe tests/test_cafe_audio.c main/cafe/cafe_audio_codec.c -o "${test_dir}/test_cafe_audio"
        "${test_dir}/test_cafe_audio" assets/cafe/media/title-bgm-4k.ima assets/cafe/media/opening-bgm-8k.ima
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Itests/cafe_audio_player_stubs -Imain/cafe -Imain/gal tests/test_cafe_audio_player.c main/cafe/cafe_audio_codec.c -o "${test_dir}/test_cafe_audio_player"
        "${test_dir}/test_cafe_audio_player" assets/cafe/media/title-bgm-4k.ima assets/cafe/media/opening-bgm-8k.ima
    fi
    if [[ -f assets/cafe/cafe.pak && -f tests/cafe_trace.txt ]]; then
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_stubs -Imain/cafe             tests/test_cafe_trace.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c             main/cafe/cafe_compat.c tests/cafe_stubs/tinfl_shim.c             -o "${test_dir}/test_cafe_trace"
        "${test_dir}/test_cafe_trace" assets/cafe/cafe.pak tests/cafe_trace.txt
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_app_stubs -Itests/cafe_stubs             -Itests/gal_stubs -Imain/cafe -Imain/gal tests/test_cafe_app.c             main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c             main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c             main/gal/gal_text.c tests/cafe_app_stubs/cafe_ui_stub.c             tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c             tests/gal_stubs/nvs_stub.c -o "${test_dir}/test_cafe_app"
        "${test_dir}/test_cafe_app" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_app_stubs -Itests/cafe_stubs             -Itests/gal_stubs -Imain/cafe -Imain/gal tests/test_cafe_input_timing.c             main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c             main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c             main/gal/gal_text.c tests/cafe_app_stubs/cafe_ui_stub.c             tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c             tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c             -o "${test_dir}/test_cafe_input_timing"
        "${test_dir}/test_cafe_input_timing" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_app_stubs -Itests/cafe_stubs             -Itests/gal_stubs -Imain/cafe -Imain/gal tests/test_cafe_input_overload.c             main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c             main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c             main/gal/gal_text.c tests/cafe_app_stubs/cafe_ui_stub.c             tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c             tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c             -o "${test_dir}/test_cafe_input_overload"
        "${test_dir}/test_cafe_input_overload" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation             -Wno-format-truncation -Wno-unused-function             -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal             tests/test_cafe_nav_pages.c main/cafe/cafe_app.c main/cafe/cafe_pack.c             main/cafe/cafe_vm.c main/cafe/cafe_store.c main/cafe/cafe_input.c             main/cafe/cafe_compat.c main/gal/gal_text.c             tests/cafe_app_stubs/cafe_ui_audit_stub.c             tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c             tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c             -o "${test_dir}/test_cafe_nav_pages"
        "${test_dir}/test_cafe_nav_pages" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_stubs -Imain/cafe \
            tests/test_cafe_ending.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c \
            main/cafe/cafe_compat.c tests/cafe_stubs/tinfl_shim.c \
            -o "${test_dir}/test_cafe_ending"
        "${test_dir}/test_cafe_ending" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation \
            -Wno-format-truncation -Wno-unused-function \
            -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal \
            tests/test_cafe_menu_flow.c main/cafe/cafe_app.c main/cafe/cafe_pack.c \
            main/cafe/cafe_vm.c main/cafe/cafe_store.c main/cafe/cafe_input.c \
            main/cafe/cafe_compat.c main/gal/gal_text.c \
            tests/cafe_app_stubs/cafe_ui_audit_stub.c \
            tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c \
            tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c \
            -o "${test_dir}/test_cafe_menu_flow"
        "${test_dir}/test_cafe_menu_flow" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation \
            -Wno-format-truncation -Wno-unused-function \
            -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal \
            tests/test_cafe_ending_app.c main/cafe/cafe_app.c main/cafe/cafe_pack.c \
            main/cafe/cafe_vm.c main/cafe/cafe_store.c main/cafe/cafe_input.c \
            main/cafe/cafe_compat.c main/gal/gal_text.c \
            tests/cafe_app_stubs/cafe_ui_audit_stub.c \
            tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c \
            tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c \
            -o "${test_dir}/test_cafe_ending_app"
        "${test_dir}/test_cafe_ending_app" assets/cafe/cafe.pak tests/cafe_ending_cases.txt
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal tests/test_cafe_failure.c main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c main/cafe/cafe_image.c main/cafe/tjpgd.c main/gal/gal_text.c tests/cafe_app_stubs/cafe_ui_stub.c tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c -o "${test_dir}/test_cafe_failure"
        "${test_dir}/test_cafe_failure" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal tests/test_cafe_op.c main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c main/gal/gal_text.c tests/cafe_app_stubs/cafe_ui_stub.c tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c -o "${test_dir}/test_cafe_op"
        "${test_dir}/test_cafe_op" assets/cafe/cafe.pak
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal tests/test_cafe_op_gallery.c main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c main/gal/gal_text.c tests/cafe_app_stubs/cafe_ui_stub.c tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c -Wl,--wrap=cafe_audio_failed -o "${test_dir}/test_cafe_op_gallery"
    "${test_dir}/test_cafe_op_gallery" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Itests/cafe_app_stubs -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal tests/test_cafe_audio_ctx.c main/cafe/cafe_app.c main/cafe/cafe_pack.c main/cafe/cafe_vm.c main/cafe/cafe_store.c main/cafe/cafe_input.c main/cafe/cafe_compat.c main/gal/gal_text.c tests/cafe_app_stubs/cafe_ui_stub.c tests/cafe_app_stubs/cafe_image_stub.c tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c -Wl,--wrap=cafe_audio_failed -o "${test_dir}/test_cafe_audio_ctx"
        "${test_dir}/test_cafe_audio_ctx" assets/cafe/cafe.pak
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_stubs -Imain/cafe             tests/test_cafe_media.c main/cafe/cafe_pack.c main/cafe/cafe_image.c             main/cafe/cafe_compat.c main/cafe/tjpgd.c tests/cafe_stubs/tinfl_shim.c             -o "${test_dir}/test_cafe_media"
        "${test_dir}/test_cafe_media" assets/cafe/cafe.pak
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_stubs -Imain/cafe         tests/test_cafe_jpeg_dedup.c main/cafe/cafe_pack.c main/cafe/cafe_image.c         main/cafe/cafe_compat.c main/cafe/tjpgd.c tests/cafe_stubs/tinfl_shim.c         -o "${test_dir}/test_cafe_jpeg_dedup"
    "${test_dir}/test_cafe_jpeg_dedup" assets/cafe/cafe.pak
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -Itests/cafe_stubs -Imain/cafe         tests/test_cafe_jpeg_guard.c main/cafe/cafe_pack.c main/cafe/cafe_compat.c         main/cafe/cafe_image.c main/cafe/tjpgd.c tests/cafe_stubs/tinfl_shim.c         -o "${test_dir}/test_cafe_jpeg_guard"
    "${test_dir}/test_cafe_jpeg_guard" assets/cafe/cafe.pak
        if [[ -x tests/ui_smoke/run.sh && -d managed_components/lvgl__lvgl ]]; then
            bash tests/ui_smoke/run.sh
        fi
        if [[ -f tests/cafe_image_ref.bin ]]; then
            "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/cafe_stubs -Imain/cafe                 tests/test_cafe_image.c main/cafe/cafe_pack.c main/cafe/cafe_image.c                 main/cafe/cafe_compat.c main/cafe/tjpgd.c tests/cafe_stubs/tinfl_shim.c                 -o "${test_dir}/test_cafe_image"
            "${test_dir}/test_cafe_image" assets/cafe/cafe.pak tests/cafe_image_ref.bin
        fi
    fi
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_cafe_lifecycle_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tools/gal_font.py --check --out-dir assets/fonts --source-root assets/cafe
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
        -o FoloToy-AI-Passport-full.bin
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
