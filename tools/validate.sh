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
    PYTHONDONTWRITEBYTECODE=1 python3 tools/gal_font.py --check --out-dir assets/fonts
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" -Wl,--gc-sections \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    # 《男娘恋爱物语》横屏移植：资源包解析 / 剧本全路线 / JPEG 直写画布 / 横屏分页。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal tests/test_nn_script.c main/nn_pack.c main/nn_script.c -o "${test_dir}/test_nn_script"
    "${test_dir}/test_nn_script"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal -Imain/nn_jpeg tests/test_nn_image.c main/nn_pack.c main/nn_image.c main/nn_jpeg/tjpgd.c -o "${test_dir}/test_nn_image"
    "${test_dir}/test_nn_image"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal tests/test_nn_text.c main/nn_pack.c main/gal/gal_text.c -o "${test_dir}/test_nn_text"
    "${test_dir}/test_nn_text"
    # 页面/按键状态机回归（真实 nn_app + nn_ui + nn_pack + nn_save，LVGL/NVS 用薄桩）。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/harness_stubs -Imain -Imain/gal tests/harness_app.c tests/harness_stubs/lvgl.c main/nn_app.c main/nn_ui.c main/nn_pack.c main/nn_script.c main/nn_save.c main/nn_save_codec.c main/gal/gal_text.c -o "${test_dir}/harness_app"
    "${test_dir}/harness_app"
    # NVS 失败注入（真实 nn_save.c，可注入 set_blob/commit/init/get 失败）。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/harness_stubs -Imain -Imain/gal tests/harness_save.c tests/harness_stubs/lvgl.c main/nn_app.c main/nn_ui.c main/nn_pack.c main/nn_script.c main/nn_save.c main/nn_save_codec.c main/gal/gal_text.c -o "${test_dir}/harness_save"
    "${test_dir}/harness_save"
    # 资源包 fail-closed 校验（真实包 + 11 类坏包）。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_nn_pack.c main/nn_pack.c -o "${test_dir}/test_nn_pack"
    "${test_dir}/test_nn_pack"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/nn_audit.py
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
