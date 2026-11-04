#!/usr/bin/env bash
set -uo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# Shared, count-independent terminal classification.
# shellcheck source=tools/validate_summary.sh
source "${repo_root}/tools/validate_summary.sh"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

detect_host_cc() {
    if [[ -n "${CC:-}" ]]; then
        read -r -a HOST_CC <<< "${CC}"
        return 0
    fi
    if command -v cc >/dev/null 2>&1; then
        HOST_CC=(cc)
        return 0
    fi
    if python3 -c "import ziglang" >/dev/null 2>&1; then
        HOST_CC=(python3 -m ziglang cc)
        return 0
    fi
    echo "ERROR: no host C compiler (set CC, install cc, or python -m ziglang)" >&2
    return 1
}

run_static_checks() {
    local actionlint_bin
    local test_dir
    local c_suites=0
    local c_fail=0
    local c_skips=0
    local source_status="SKIP"

    cd "${repo_root}"
    detect_host_cc || return 1
    test_dir="build/validate-host-$(date +%Y%m%d-%H%M%S)-$$"
    mkdir -p "${test_dir}"
    echo "Host test artifacts: ${repo_root}/${test_dir}"

    c_suites=$((c_suites + 1))
    if ! python3 tools/check_repo.py; then
        echo "FAIL | repository checks"
        c_fail=$((c_fail + 1))
    else
        echo "PASS | repository checks"
    fi

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh 2>/dev/null || true)"
    fi
    if [[ -n "${actionlint_bin}" && -x "${actionlint_bin}" ]]; then
        c_suites=$((c_suites + 1))
        if ! "${actionlint_bin}" -color .github/workflows/*.yml; then
            echo "FAIL | actionlint"
            c_fail=$((c_fail + 1))
        else
            echo "PASS | actionlint"
        fi
    else
        echo "SKIP | actionlint | not installed"
        c_skips=$((c_skips + 1))
    fi

    c_suites=$((c_suites + 1))
    if ! bash tests/test_validate_classification.sh             > "${test_dir}/validate_classification.log" 2>&1; then
        echo "FAIL | validation classification maintenance test"
        tail -n 12 "${test_dir}/validate_classification.log" || true
        c_fail=$((c_fail + 1))
    else
        echo "PASS | validation classification maintenance test"
    fi

    # compile_and_run <name> <compiler args...>
    compile_and_run() {
        local name="$1"
        shift
        c_suites=$((c_suites + 1))
        if ! "${HOST_CC[@]}" "$@" "tests/${name}.c" -o "${test_dir}/${name}" \
                > "${test_dir}/${name}.log" 2>&1; then
            echo "FAIL | C regression ${name} | compile failed"
            tail -n 5 "${test_dir}/${name}.log" || true
            c_fail=$((c_fail + 1))
            return 1
        fi
        if ! "${test_dir}/${name}" >> "${test_dir}/${name}.log" 2>&1; then
            echo "FAIL | C regression ${name} | run failed"
            tail -n 8 "${test_dir}/${name}.log" || true
            c_fail=$((c_fail + 1))
            return 1
        fi
        echo "PASS | C regression ${name}"
        return 0
    }

    # Baseline framework/BSP regressions (unchanged sources).
    compile_and_run test_ui_pixel_math -std=c11 -Wall -Wextra -Werror -Imain \
        main/ui_pixel_math.c
    compile_and_run test_demo_navigation -std=c11 -Wall -Wextra -Werror -Imain \
        main/demo_navigation.c
    compile_and_run test_key_guard -std=c11 -Wall -Wextra -Werror -Imain
    compile_and_run test_bsp_display_rounding -std=c11 -Wall -Wextra -Werror \
        -Icomponents/bsp/src components/bsp/src/bsp_display_rounding.c
    compile_and_run test_bsp_es8311_sleep_check -std=c11 -Wall -Wextra -Werror \
        -Icomponents/bsp/src components/bsp/src/bsp_es8311_sleep_check.c
    compile_and_run test_bsp_button -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include
    compile_and_run test_bsp_lvgl_init -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include components/bsp/src/bsp_display_rounding.c
    compile_and_run test_bsp_audio_recovery -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        components/bsp/src/bsp_es8311_sleep_check.c
    compile_and_run test_gal_text -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        main/gal/gal_text.c
    compile_and_run test_gal_tone -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        main/gal/gal_tone.c
    for demo in audio low_power ble wifi; do
        c_suites=$((c_suites + 1))
        if ! "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror \
                -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
                "tests/test_demo_${demo}_runtime.c" tests/demo_stubs/lvgl_link_stubs.c \
                -Wl,--gc-sections \
                -o "${test_dir}/test_demo_${demo}_runtime" \
                > "${test_dir}/test_demo_${demo}.log" 2>&1 ||
           ! "${test_dir}/test_demo_${demo}_runtime" >> "${test_dir}/test_demo_${demo}.log" 2>&1; then
            echo "FAIL | C regression test_demo_${demo}_runtime"
            tail -n 8 "${test_dir}/test_demo_${demo}.log" || true
            c_fail=$((c_fail + 1))
        else
            echo "PASS | C regression test_demo_${demo}_runtime"
        fi
    done

    # Steins;Gate 0 reader: pack/text/story/app/UI regressions.
    compile_and_run test_steins0_pack -std=c11 -Wall -Wextra -Werror -Imain \
        main/steins0_pack.c
    compile_and_run test_steins0_text -std=c11 -Wall -Wextra -Werror -Imain \
        main/steins0_text.c main/steins0_pack.c
    compile_and_run test_steins0_story -std=c11 -Wall -Wextra -Werror -Imain \
        main/steins0_engine.c main/steins0_pack.c main/steins0_text.c
    compile_and_run test_steins0_app -std=c11 -Wall -Wextra -Werror \
        -Itests/lvgl_ui_stubs -Itests/steins0_host_stubs -Imain \
        main/steins0_app.c main/steins0_pack.c main/steins0_text.c \
        main/steins0_engine.c main/steins0_ui.c \
        tests/steins0_host_stubs/host_stubs.c \
        tests/steins0_host_stubs/steins0_image_stub.c \
        tests/lvgl_ui_stubs/lvgl_ui_stubs.c
    compile_and_run test_steins0_ui_visibility -std=c11 -Wall -Wextra -Werror \
        -Itests/lvgl_ui_stubs -Itests/steins0_host_stubs -Imain \
        main/steins0_ui.c tests/steins0_host_stubs/steins0_image_stub.c \
        tests/lvgl_ui_stubs/lvgl_ui_stubs.c

    compile_and_run test_steins0_rine -std=c11 -Wall -Wextra -Werror -Imain \
        main/steins0_engine.c main/steins0_pack.c main/steins0_text.c
    compile_and_run test_steins0_all_open -std=c11 -Wall -Wextra -Werror \
        -Itests/lvgl_ui_stubs -Itests/steins0_host_stubs -Imain \
        main/steins0_app.c main/steins0_pack.c main/steins0_text.c \
        main/steins0_engine.c main/steins0_ui.c \
        tests/steins0_host_stubs/host_stubs.c \
        tests/steins0_host_stubs/steins0_image_stub.c \
        tests/lvgl_ui_stubs/lvgl_ui_stubs.c
    compile_and_run test_steins0_event_save -std=c11 -Wall -Wextra -Werror         -Itests/lvgl_ui_stubs -Itests/steins0_host_stubs -Imain         main/steins0_app.c main/steins0_pack.c main/steins0_text.c         main/steins0_engine.c main/steins0_ui.c         tests/steins0_host_stubs/host_stubs.c         tests/steins0_host_stubs/steins0_image_stub.c         tests/lvgl_ui_stubs/lvgl_ui_stubs.c
    compile_and_run test_steins0_io_replay -std=c11 -Wall -Wextra -Werror \
        -Itests/lvgl_ui_stubs -Itests/steins0_host_stubs -Imain \
        main/steins0_app.c main/steins0_pack.c main/steins0_text.c \
        main/steins0_engine.c main/steins0_ui.c \
        tests/steins0_host_stubs/host_stubs.c \
        tests/steins0_host_stubs/steins0_image_stub.c \
        tests/lvgl_ui_stubs/lvgl_ui_stubs.c

    # Real TJpgDec image regression + tracked gate negative test.
    c_suites=$((c_suites + 1))
    if ! tests/steins0_image_gate.sh "${HOST_CC[*]}" "${test_dir}/image" \
            > "${test_dir}/image_gate.log" 2>&1; then
        echo "FAIL | real image/TJpgDec host regression"
        tail -n 8 "${test_dir}/image_gate.log" || true
        c_fail=$((c_fail + 1))
    else
        cat "${test_dir}/image_gate.log"
    fi
    c_suites=$((c_suites + 1))
    if ! CC="${HOST_CC[*]}" bash tests/test_steins0_gate_negative.sh \
            > "${test_dir}/gate_negative.log" 2>&1; then
        echo "FAIL | gate negative test"
        cat "${test_dir}/gate_negative.log" || true
        c_fail=$((c_fail + 1))
    else
        cat "${test_dir}/gate_negative.log"
    fi

    c_suites=$((c_suites + 1))
    if ! PYTHONDONTWRITEBYTECODE=1 python3 tools/steins0_font.py --check \
            --pack assets/steins0/steins0_pack.bin --out-dir assets/fonts \
            > "${test_dir}/font_check.log" 2>&1; then
        echo "FAIL | font coverage check"
        tail -n 8 "${test_dir}/font_check.log" || true
        c_fail=$((c_fail + 1))
    else
        cat "${test_dir}/font_check.log"
    fi

    # Independent source audit (not importing the packer); a missing source is
    # reported as SKIP. SKIP can only lead to PARTIAL when no real FAIL exists.
    if [[ -n "${STEINS0_SOURCE:-}" ]]; then
        c_suites=$((c_suites + 1))
        if PYTHONDONTWRITEBYTECODE=1 python3 tools/steins0_audit.py \
                "${STEINS0_SOURCE}" --pack assets/steins0/steins0_pack.bin \
                --json "${test_dir}/source_audit.json" \
                > "${test_dir}/source_audit.log" 2>&1; then
            source_status="PASS"
            tail -n 9 "${test_dir}/source_audit.log"
        else
            source_status="FAIL"
            c_fail=$((c_fail + 1))
            tail -n 12 "${test_dir}/source_audit.log" || true
        fi
    else
        echo "SKIP | independent source audit | set STEINS0_SOURCE to the steins0-rw5 checkout"
        c_skips=$((c_skips + 1))
    fi

    # Python gates. Every executed suite is counted; a non-zero exit is a real
    # FAIL. Never downgrade by matching a historical failure count: equal counts
    # may represent a different regression.
    for py in test_deep_sleep_contract test_key_wake_contract test_check_repo \
              test_verify_firmware test_archive_firmware test_install_passport_skills; do
        c_suites=$((c_suites + 1))
        if PYTHONDONTWRITEBYTECODE=1 python3 "tests/${py}.py"                 > "${test_dir}/${py}.log" 2>&1; then
            echo "PASS | python gate ${py}"
        else
            fails="$(grep -oE 'failures=[0-9]+' "${test_dir}/${py}.log" | tail -n 1 | cut -d= -f2)"
            echo "FAIL | python gate ${py} | failures=${fails:-unknown}"
            tail -n 8 "${test_dir}/${py}.log" || true
            c_fail=$((c_fail + 1))
        fi
    done

    local passed=$((c_suites - c_fail))
    local host_status
    host_status="$(validate_summary_status "${c_fail}" "${c_skips}")"
    local host_rc=$?
    echo "Host tests: ${host_status} (suites PASS=${passed}/${c_suites} FAIL=${c_fail} SKIP=${c_skips}; source_audit=${source_status})"
    echo "Host artifacts retained: ${repo_root}/${test_dir}"
    return "${host_rc}"
}

run_firmware_checks() (
    local build_dir
    local build_win
    local idf_py_cmd=()
    local py_bin
    local repo_win
    local idf_win

    cd "${repo_root}"
    build_dir="${repo_root}/build/validate-fw-$(date +%Y%m%d-%H%M%S)-$$"
    mkdir -p "${build_dir}"
    build_win="${build_dir}"
    if command -v cygpath >/dev/null 2>&1; then
        build_win="$(cygpath -w "${build_dir}")"
        repo_win="$(cygpath -w "${repo_root}")"
    fi
    echo "Firmware artifacts: ${build_dir}"

    # Prefer the native PowerShell entry on Windows/Git Bash (MSYSTEM makes the
    # idf.py wrapper exit silently).  It activates ESP-IDF 5.5.3 itself.
    if command -v powershell.exe >/dev/null 2>&1 && [[ -n "${repo_win:-}" ]]; then
        idf_win="${IDF_PATH:-C:/Espressif/frameworks/esp-idf-v5.5.3}"
        if command -v cygpath >/dev/null 2>&1; then
            idf_win="$(cygpath -w "${idf_win}")"
        fi
        if ! powershell.exe -NoProfile -ExecutionPolicy Bypass                 -File "$(cygpath -w "${repo_root}/tools/build_firmware.ps1")"                 -BuildDir "${build_win}" -ProjectRoot "${repo_win}" -IdfPath "${idf_win}"                 > "${build_dir}/build.log" 2>&1; then
            echo "Firmware build: FAIL"
            tail -n 20 "${build_dir}/build.log"
            return 1
        fi
    else
        if ! command -v idf.py >/dev/null 2>&1 && [[ -z "${IDF_PATH:-}" ]]; then
            echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
            return 1
        fi
        unset MSYSTEM OSTYPE MSYSTEM_PREFIX
        idf_py_cmd=(idf.py)
        if [[ -n "${IDF_PATH:-}" && -n "${IDF_PYTHON_ENV_PATH:-}" ]]; then
            py_bin="${IDF_PYTHON_ENV_PATH}/Scripts/python.exe"
            if [[ -f "${py_bin}" && -f "${IDF_PATH}/tools/idf.py" ]]; then
                idf_py_cmd=("${py_bin}" "${IDF_PATH}/tools/idf.py")
            fi
        fi
        SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults"             "${idf_py_cmd[@]}" -B "${build_win}"             -D "SDKCONFIG=${build_win}/sdkconfig" build             > "${build_dir}/build.log" 2>&1 || {
                echo "Firmware build: FAIL"
                tail -n 20 "${build_dir}/build.log"
                return 1
            }
        "${idf_py_cmd[@]}" -B "${build_win}" merge-bin             -o "${build_win}/FoloToy-AI-Passport-full.bin"             >> "${build_dir}/build.log" 2>&1 || {
                echo "Firmware merge: FAIL"
                tail -n 20 "${build_dir}/build.log"
                return 1
            }
    fi
    if ! python3 tools/verify_firmware.py "${build_dir}" > "${build_dir}/verify.log" 2>&1; then
        echo "Firmware verify: FAIL"
        cat "${build_dir}/verify.log"
        return 1
    fi
    cat "${build_dir}/verify.log"
    if ! PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
            "${build_dir}" --archive-root "${repo_root}/build/firmware" \
            > "${build_dir}/archive.log" 2>&1; then
        echo "Firmware archive: FAIL"
        cat "${build_dir}/archive.log"
        return 1
    fi
    local archive
    archive="$(sed -n 's/^Firmware debug archive: //p' "${build_dir}/archive.log" | tail -n 1)"
    if [[ -n "${archive}" ]]; then
        if ! python3 tools/archive_firmware.py verify "${archive}" \
                >> "${build_dir}/archive.log" 2>&1; then
            echo "Firmware archive verify: FAIL"
            tail -n 12 "${build_dir}/archive.log"
            return 1
        fi
    fi
    mkdir -p "${repo_root}/build"
    install -m 0644 "${build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS (artifacts retained: ${build_dir})"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        static_status=0
        run_static_checks || static_status=$?
        fw_status=0
        run_firmware_checks || fw_status=$?
        [[ "${static_status}" -eq 0 && "${fw_status}" -eq 0 ]]
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
