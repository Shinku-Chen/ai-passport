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
    local local_skips=()

    note_skip() { local_skips+=("$1"); echo "SKIP: $1"; }

    python3 tools/check_repo.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    mkdir -p "${repo_root}/build/host-tests"
    test_dir="$(mktemp -d "${repo_root}/build/host-tests/run.XXXXXX")"
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
    # 本作:分页排版 / 资源包结构 / 剧情虚拟机语义(全部纯逻辑,宿主机运行)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_text.c main/gal_text.c -o "${test_dir}/test_gal_text"
    "${test_dir}/test_gal_text"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_model.c main/gal_model.c main/gal_pack.c -o "${test_dir}/test_gal_model"
    "${test_dir}/test_gal_model"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_pack.c main/gal_pack.c -o "${test_dir}/test_gal_pack"
    "${test_dir}/test_gal_pack" main/gal_data/gal_pack.bin
    # 存档/设置的持久化:内存 NVS + 故障注入(commit 失败必须判为保存失败)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/esp_stubs -Imain tests/test_gal_store.c \
        main/gal_store.c main/gal_model.c main/gal_pack.c tests/esp_stubs/nvs_stub.c \
        tests/esp_stubs/esp_err_stub.c \
        -o "${test_dir}/test_gal_store"
    "${test_dir}/test_gal_store"
    # 真实包的全路线遍历:UI 同一入口(gal_choice_pick)、四结局、53 个选项。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_routes.c main/gal_model.c main/gal_pack.c -o "${test_dir}/test_gal_routes"
    "${test_dir}/test_gal_routes" main/gal_data/gal_pack.bin
    # 画面合成(JPEG/LVGL 薄桩):场景两半、遮罩几何、失败返回。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/lvgl_stubs -Itests/esp_stubs -Imain         tests/test_gal_image.c main/gal_image.c main/gal_pack.c main/gal_model.c         tests/lvgl_stubs/lvgl_stub.c tests/lvgl_stubs/fonts_stub.c         tests/esp_stubs/jpeg_stub.c tests/esp_stubs/esp_err_stub.c         -o "${test_dir}/test_gal_image"
    "${test_dir}/test_gal_image"
    # 坏包必须被拒绝。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_gal_badpack.c main/gal_pack.c -o "${test_dir}/test_gal_badpack"
    "${test_dir}/test_gal_badpack" main/gal_data/gal_pack.bin
    # 真实 app + ui 的按键/页面回归(选择点菜单、存档、toast、历史、快进)。
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/lvgl_stubs -Itests/esp_stubs -Itests/app_stubs -Imain         tests/test_gal_app.c main/gal_app.c main/gal_ui.c main/gal_image.c main/gal_store.c         main/gal_model.c main/gal_pack.c main/gal_text.c         tests/lvgl_stubs/lvgl_stub.c tests/lvgl_stubs/fonts_stub.c         tests/esp_stubs/jpeg_stub.c tests/esp_stubs/nvs_stub.c         tests/esp_stubs/esp_err_stub.c tests/app_stubs/bsp_stub.c         -o "${test_dir}/test_gal_app"
    "${test_dir}/test_gal_app" main/gal_data/gal_pack.bin
    # 源 chunk 对账(独立解析 29 个 chunk,不 import 转换器)。需要外部转换源目录。
    if [[ -n "${GALGOD_SOURCE:-}" && -d "${GALGOD_SOURCE}" ]]; then
        PYTHONDONTWRITEBYTECODE=1 python3 tools/galgod_audit.py \
            --script "${GALGOD_SOURCE}" --pack main/gal_data/gal_pack.bin
    else
        note_skip "source audit (GALGOD_SOURCE 未设置)"
    fi
    PYTHONDONTWRITEBYTECODE=1 python3 tools/galgod_font.py --check --pack main/gal_data/gal_pack.bin --out-dir assets/fonts
    # baseline demo 的运行时测试。Windows 的 COFF 链接器不做 --gc-sections 裁剪,
    # 因此额外链一份空实现(tests/demo_stubs/demo_missing_stubs.c)。
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" tests/demo_stubs/demo_missing_stubs.c \
            -Wl,--gc-sections \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    # 技能安装器的测试需要创建目录符号链接;Windows 非管理员/未开开发者模式时
    # 会被系统策略拒绝(WinError 1314)。这里先探测,再决定跑还是明确跳过。
    if PYTHONDONTWRITEBYTECODE=1 python3 - "${test_dir}" <<'PYPROBE' >/dev/null 2>&1
import os, sys, tempfile
root = tempfile.mkdtemp(dir=sys.argv[1])
target = os.path.join(root, "t")
os.mkdir(target)
os.symlink(target, os.path.join(root, "l"))
PYPROBE
    then
        PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    else
        note_skip "skill-installer (Windows 符号链接策略)"
    fi
    # 临时目录保留在 build/host-tests 下(不 rm -rf),失败时可复查。
    if [[ ${#local_skips[@]} -eq 0 ]]; then
        echo "Host tests: PASS"
    else
        echo "Host tests: PARTIAL (skipped: ${local_skips[*]})"
    fi
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    # 每次用一个新的、保留下来的构建目录(不 rm -rf,便于复查失败现场)。
    mkdir -p "${repo_root}/build/firmware-gate"
    validation_build_dir="$(mktemp -d "${repo_root}/build/firmware-gate/run.XXXXXX")"

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
