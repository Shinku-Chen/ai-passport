#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
host_cc=()
if [[ -n "${CC:-}" ]]; then
    read -r -a host_cc <<< "${CC}"
elif python3 -c 'import ziglang' >/dev/null 2>&1; then
    host_cc=(python3 -m ziglang cc)
elif python -c 'import ziglang' >/dev/null 2>&1; then
    host_cc=(python -m ziglang cc)
else
    host_cc=(cc)
fi

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local dead_strip_flag
    local c_fail=0
    local c_suites=20
    local source_status="SKIP"

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

    test_dir="$(mktemp -d "${TMPDIR:-/tmp}/ai-passport-host-tests.XXXXXX")"
    host_log="${test_dir}/host.log"
    exec > >(tee "${host_log}") 2>&1
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"
    # GalGame 框架:排版分页纯逻辑 + 中文字体覆盖自检。
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_text.c main/gal/gal_text.c \
        -o "${test_dir}/test_gal_text"
    "${test_dir}/test_gal_text"
    # GalGame 框架:方波合成(音乐鉴赏的占位音色)纯逻辑。
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_tone.c main/gal/gal_tone.c \
        -o "${test_dir}/test_gal_tone"
    "${test_dir}/test_gal_tone"
    # 《星之终途》阅读器：真实资源包全量引用/分页/存档审计 + 两条故事线遍历。
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain         tests/test_stella_pack.c main/stella_pack.c main/stella_model.c         -o "${test_dir}/test_stella_pack"
    "${test_dir}/test_stella_pack"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain         tests/test_stella_story.c main/stella_pack.c main/stella_model.c         -o "${test_dir}/test_stella_story"
    "${test_dir}/test_stella_story"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Itests/stella_host_stubs -Imain         tests/test_stella_app.c main/stella_app.c main/stella_pack.c main/stella_model.c         tests/stella_host_stubs/stella_host_stubs.c         -o "${test_dir}/test_stella_app"
    "${test_dir}/test_stella_app"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Itests/stella_host_stubs -Imain         tests/test_stella_app_regressions.c main/stella_app.c main/stella_pack.c         main/stella_model.c tests/stella_host_stubs/stella_host_stubs.c         -o "${test_dir}/test_stella_app_regressions"
    "${test_dir}/test_stella_app_regressions"
    "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Itests/lvgl_ui_stubs -Imain         tests/test_stella_ui_visibility.c main/stella_ui.c         tests/lvgl_ui_stubs/lvgl_ui_stubs.c         -o "${test_dir}/test_stella_ui_visibility"
    "${test_dir}/test_stella_ui_visibility"
    if ! "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Itests/image_stubs         -Itests/lvgl_ui_stubs -Itests/vendor/tjpgd -Imain tests/test_stella_image.c         main/stella_image.c main/stella_pack.c tests/vendor/tjpgd/tjpgd.c         tests/lvgl_ui_stubs/lvgl_ui_stubs.c -o "${test_dir}/test_stella_image"; then
        echo "FAIL | real image/TJpgDec host regression | compile failed"
        c_fail=$((c_fail + 1))
    elif ! "${test_dir}/test_stella_image"; then
        echo "FAIL | real image/TJpgDec host regression | run failed"
        c_fail=$((c_fail + 1))
    fi
    if [[ -n "${STELLA_SOURCE:-}" ]]; then
        if PYTHONDONTWRITEBYTECODE=1 python3 tools/stella_audit.py "${STELLA_SOURCE}" assets/stella/stella_pack.bin; then
            source_status="PASS"
        else
            source_status="FAIL"
            c_fail=$((c_fail + 1))
        fi
    else
        echo "SKIP | independent source audit | set STELLA_SOURCE to the stella-rw5 checkout"
    fi
    PYTHONDONTWRITEBYTECODE=1 python3 tools/stella_font.py --check         --pack assets/stella/stella_pack.bin --out-dir assets/fonts
    for demo in audio low_power ble wifi; do
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" tests/demo_stubs/lvgl_link_stubs.c "${dead_strip_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py
    # Keep the unique host test directory; never delete build evidence.
    local skips runs passed c_pass status
    skips="$((grep -oE 'skipped=[0-9]+' "${host_log}" || true) | cut -d= -f2 | awk '{s+=$1} END{print s+0}')"
    runs="$((grep -oE 'Ran [0-9]+ tests?' "${host_log}" || true) | awk '{s+=$2} END{print s+0}')"
    passed=$((runs - skips))
    c_pass=$((c_suites - c_fail))
    if [[ "${c_fail}" -gt 0 ]]; then
        status="FAIL"
    elif [[ "${skips}" -gt 0 || "${source_status}" == "SKIP" ]]; then
        status="PARTIAL"
    else
        status="PASS"
    fi
    echo "Host tests: ${status} (unittest PASS=${passed} SKIP=${skips} FAIL=0; C regressions PASS=${c_pass}/${c_suites} FAIL=${c_fail}; source_audit=${source_status})"
    echo "Host artifacts retained: ${test_dir}"
    if [[ "${c_fail}" -gt 0 ]]; then
        return 1
    fi
}

run_firmware_checks() (
    local validation_build_dir validation_build_win defaults_arg archive_root_arg

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi
    # Git Bash exports MSYSTEM; the Windows idf.py wrapper can otherwise exit
    # silently. Prefer the IDF Python module directly (native path), and fall
    # back to the wrapper when the env is not activated.
    unset MSYSTEM OSTYPE MSYSTEM_PREFIX
    local idf_py_cmd=(idf.py)
    if [[ -n "${IDF_PATH:-}" && -n "${IDF_PYTHON_ENV_PATH:-}" ]]; then
        local py="${IDF_PYTHON_ENV_PATH}/Scripts/python.exe"
        local py_script="${IDF_PATH}/tools/idf.py"
        if [[ -f "${py}" && -f "${py_script}" ]]; then
            idf_py_cmd=("${py}" "${py_script}")
        fi
    fi

    validation_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/ai-passport-firmware.XXXXXX")"
    # Windows Python (idf.py/esptool) cannot write an MSYS /tmp path; keep the
    # POSIX path for shell artifacts and pass a native path to Windows tools.
    validation_build_win="${validation_build_dir}"
    defaults_arg="${repo_root}/sdkconfig.defaults"
    archive_root_arg="${repo_root}/build/firmware"
    if command -v cygpath >/dev/null 2>&1; then
        validation_build_win="$(cygpath -w "${validation_build_dir}")"
        defaults_arg="$(cygpath -w "${defaults_arg}")"
        archive_root_arg="$(cygpath -w "${archive_root_arg}")"
    fi
    SDKCONFIG_DEFAULTS="${defaults_arg}" \
        "${idf_py_cmd[@]}" -B "${validation_build_win}" \
        -D "SDKCONFIG=${validation_build_win}/sdkconfig" build
    "${idf_py_cmd[@]}" -B "${validation_build_win}" merge-bin \
        -o "${validation_build_win}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_win}"
    PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_win}" --archive-root "${archive_root_arg}"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS (artifacts retained: ${validation_build_dir})"
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
