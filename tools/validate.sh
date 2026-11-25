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
    # GalGame 框架:内容表形状与启动链（本作 1,537 段 / 四章 / 一个结局）。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/gal_app_stubs -Itests/gal_stubs -Imain -Imain/gal \
        tests/test_gal_app_flow.c main/gal/gal_app.c main/gal/gal_save.c \
        main/gal/gal_progress.c main/gal/gal_unlock.c main/gal/gal_settings.c \
        main/gal/gal_content.c main/gal/gal_text.c \
        tests/gal_app_stubs/gal_app_hal_stub.c tests/gal_stubs/nvs_stub.c \
        -o "$test_dir/test_gal_app_flow"
    "$test_dir/test_gal_app_flow"
    # GalGame 框架:三键语义 / 快进 / 自动 / 试读不计通关（受控时钟）。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/gal_app_stubs -Itests/gal_stubs -Imain -Imain/gal \
        tests/test_gal_keys.c main/gal/gal_app.c main/gal/gal_save.c \
        main/gal/gal_progress.c main/gal/gal_unlock.c main/gal/gal_settings.c \
        main/gal/gal_content.c main/gal/gal_text.c \
        tests/gal_app_stubs/gal_app_hal_stub.c tests/gal_stubs/nvs_stub.c \
        -o "$test_dir/test_gal_keys"
    "$test_dir/test_gal_keys"
    # GalGame 框架:快进在选项页停住 / 长按释放 / 分支标记（最小分支内容）。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/gal_app_stubs -Itests/gal_stubs -Imain -Imain/gal \
        tests/test_gal_choice_ff.c main/gal/gal_app.c main/gal/gal_save.c \
        main/gal/gal_progress.c main/gal/gal_unlock.c main/gal/gal_settings.c \
        main/gal/gal_text.c tests/gal_app_stubs/gal_content_choice.c \
        tests/gal_app_stubs/gal_app_hal_stub.c tests/gal_stubs/nvs_stub.c \
        -o "$test_dir/test_gal_choice_ff"
    "$test_dir/test_gal_choice_ff"
    # 本作内容与媒体包：生成文件结构、条带 CRC、场景合成采样、音频时长。
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_maitetsu_content.py
    PYTHONDONTWRITEBYTECODE=1 python3 tools/gal_font.py --check --out-dir assets/fonts
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
    # 依赖已下载：用设备同款 TJpgDec 解码整包，并与源构图采样对比。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/maitetsu_stubs -Imain/gal \
        -Imanaged_components/espressif__esp_jpeg/tjpgd \
        tests/test_maitetsu_pack.c main/gal/maitetsu_pack.c \
        main/gal/maitetsu_assets.c \
        managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c \
        -o "${validation_build_dir}/test_maitetsu_pack"
    "${validation_build_dir}/test_maitetsu_pack" \
        assets/maitetsu/maitetsu_media.bin tests/fixtures/maitetsu-scene-samples.txt

    # 真实 LVGL 解码器集成：编译完整 LVGL（host），驱动生产 maitetsu_media 的
    # info/open/get_area/close，交错解码不同宽高的背景/补丁/CG/缩略图并与源构图比对。
    lvgl_host="${validation_build_dir}/lvgl-host"
    mkdir -p "${lvgl_host}"
    find managed_components/lvgl__lvgl/src -name '*.c' \
        ! -path '*/drivers/opengles/*' > "${lvgl_host}/sources.txt"
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
    xargs -P "${jobs}" -I{} bash -c '
        src="$1"; dir="$2"; cc="$3"
        out="${dir}/$(printf "%s" "${src}" | cksum | cut -d" " -f1).o"
        "${cc}" -std=c11 -O1 -DLV_CONF_SKIP \
            -Imanaged_components/lvgl__lvgl -Imanaged_components/lvgl__lvgl/src \
            -c "${src}" -o "${out}" || exit 255
    ' _ {} "${lvgl_host}" "${CC:-cc}" < "${lvgl_host}/sources.txt" || return 1
    ar rcs "${lvgl_host}/liblvgl.a" "${lvgl_host}"/*.o
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DLV_CONF_SKIP \
        -Itests/maitetsu_stubs -Imain/gal \
        -Imanaged_components/lvgl__lvgl -Imanaged_components/lvgl__lvgl/src \
        -Imanaged_components/espressif__esp_jpeg/tjpgd \
        tests/test_maitetsu_decoder.c main/gal/maitetsu_media.c \
        main/gal/maitetsu_pack.c main/gal/maitetsu_assets.c \
        managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c \
        "${lvgl_host}/liblvgl.a" -lm -o "${lvgl_host}/test_maitetsu_decoder"
    "${lvgl_host}/test_maitetsu_decoder" \
        assets/maitetsu/maitetsu_media.bin tests/fixtures/maitetsu-scene-samples.txt

    # 真实 LVGL 错误层可见性：标题 / CG / 缩略图页面、隐藏父页、切页后的实际像素。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DLV_CONF_SKIP \
        -Itests/maitetsu_stubs -Itests/gal_stubs -Imain/gal \
        -Imanaged_components/lvgl__lvgl -Imanaged_components/lvgl__lvgl/src \
        -Imanaged_components/espressif__esp_jpeg/tjpgd \
        tests/test_gal_ui_error.c main/gal/gal_ui.c main/gal/gal_settings.c \
        main/gal/maitetsu_media.c main/gal/maitetsu_pack.c \
        main/gal/maitetsu_assets.c \
        managed_components/espressif__esp_jpeg/tjpgd/tjpgd.c \
        assets/fonts/gal_cjk_16.c assets/fonts/gal_cjk_20.c \
        tests/gal_stubs/nvs_stub.c "${lvgl_host}/liblvgl.a" -lm \
        -o "${lvgl_host}/test_gal_ui_error"
    "${lvgl_host}/test_gal_ui_error" assets/maitetsu/maitetsu_media.bin

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
