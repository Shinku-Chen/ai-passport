#!/usr/bin/env bash
# Repository host/firmware validation.
#
# Every host test case gets a unique directory and a complete log under
# build/; nothing is deleted.  The host summary is the real per-case result:
# a failed C compile/run, a skipped Summer check or a missing source snapshot
# cannot be hidden by a later passing case.
set -uo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date +%Y%m%d-%H%M%S)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

host_cc=()
choose_host_cc() {
    if [[ -n "${CC:-}" ]]; then
        read -r -a host_cc <<< "${CC}"
        return 0
    fi
    # Prefer the repository wrapper over a personal/ignored build/bin tool.
    if [[ -x "${repo_root}/tools/cc-host.sh" ]]; then
        host_cc=("${repo_root}/tools/cc-host.sh")
        return 0
    fi
    if command -v cc >/dev/null 2>&1; then
        host_cc=(cc)
        return 0
    fi
    if python3 -m ziglang cc --version >/dev/null 2>&1; then
        host_cc=(python3 -m ziglang cc)
        return 0
    fi
    return 1
}

run_static_checks() {
    local host_run_dir host_log_dir
    local host_pass=0 host_fail=0 host_skip=0 host_partial=0
    local cflags=(-std=c11 -Wall -Wextra -Werror)
    local actionlint_bin
    local summer_rc=0
    local -a results=()

    if ! choose_host_cc; then
        mkdir -p "${repo_root}/build/logs"
        local nocc_log="${repo_root}/build/logs/host-tests-${stamp}-no-cc.log"
        printf '%s\n' "FAIL: no host C compiler; set CC or install ziglang/cc" >"${nocc_log}"
        echo "Host tests: FAIL (no host C compiler; ${nocc_log})" >&2
        return 1
    fi

    mkdir -p "${repo_root}/build/host-tests" "${repo_root}/build/logs"
    host_run_dir="$(mktemp -d "${repo_root}/build/host-tests/run-${stamp}.XXXXXX")" || return 1
    host_log_dir="$(mktemp -d "${repo_root}/build/logs/host-tests-${stamp}.XXXXXX")" || return 1

    run_case() {
        local name="$1"
        shift
        local log="${host_log_dir}/${name}.log"
        local rc=0
        "$@" >"${log}" 2>&1 || rc=$?
        if [[ ${rc} -eq 0 ]]; then
            results+=("PASS | ${name} | ${log}")
            host_pass=$((host_pass + 1))
            printf 'PASS %-30s %s\n' "${name}" "${log}"
        elif [[ ${rc} -eq 77 ]]; then
            results+=("SKIP | ${name} (exit 77) | ${log}")
            host_skip=$((host_skip + 1))
            printf 'SKIP %-30s %s\n' "${name}" "${log}"
        else
            results+=("FAIL | ${name} (rc=${rc}) | ${log}")
            host_fail=$((host_fail + 1))
            printf 'FAIL %-30s %s\n' "${name}" "${log}"
            tail -5 "${log}" || true
        fi
    }

    cc_case() {
        local name="$1"
        shift
        local bin="${host_run_dir}/${name}"
        "${host_cc[@]}" "$@" -o "${bin}" && "${bin}"
    }

    actionlint_case() {
        actionlint_bin="${ACTIONLINT_BIN:-}"
        if [[ -z "${actionlint_bin}" ]]; then
            actionlint_bin="$(command -v actionlint || true)"
        fi
        if [[ -z "${actionlint_bin}" ]]; then
            local dest="${repo_root}/build/tools/actionlint-1.7.12"
            if [[ -x "${dest}/actionlint" ]]; then
                actionlint_bin="${dest}/actionlint"
            elif [[ -x /tmp/actionlint-1.7.12/actionlint ]]; then
                mkdir -p "${dest}"
                cp /tmp/actionlint-1.7.12/actionlint "${dest}/actionlint"
                chmod +x "${dest}/actionlint"
                actionlint_bin="${dest}/actionlint"
            else
                actionlint_bin="$(./tools/install-actionlint.sh "${dest}")" || return 1
            fi
        fi
        "${actionlint_bin}" -color .github/workflows/*.yml
    }

    run_case repository-checks env PYTHONDONTWRITEBYTECODE=1 python3 tools/check_repo.py
    run_case actionlint actionlint_case
    run_case ui-pixel-math cc_case ui-pixel-math "${cflags[@]}" -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c
    run_case demo-navigation cc_case demo-navigation "${cflags[@]}" -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c
    run_case key-guard cc_case key-guard "${cflags[@]}" -Imain \
        tests/test_key_guard.c
    run_case bsp-display-rounding cc_case bsp-display-rounding "${cflags[@]}" \
        -Icomponents/bsp/src tests/test_bsp_display_rounding.c \
        components/bsp/src/bsp_display_rounding.c
    run_case bsp-es8311-sleep-check cc_case bsp-es8311-sleep-check "${cflags[@]}" \
        -Icomponents/bsp/src tests/test_bsp_es8311_sleep_check.c \
        components/bsp/src/bsp_es8311_sleep_check.c
    run_case bsp-button cc_case bsp-button "${cflags[@]}" \
        -Itests/bsp_stubs -Icomponents/bsp/include tests/test_bsp_button.c
    run_case bsp-lvgl-init cc_case bsp-lvgl-init "${cflags[@]}" \
        -Itests/bsp_stubs -Icomponents/bsp/include tests/test_bsp_lvgl_init.c \
        components/bsp/src/bsp_display_rounding.c
    run_case bsp-audio-recovery cc_case bsp-audio-recovery "${cflags[@]}" \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c
    run_case gal-text cc_case gal-text "${cflags[@]}" -Imain -Imain/gal \
        tests/test_gal_text.c main/gal/gal_text.c
    run_case gal-tone cc_case gal-tone "${cflags[@]}" -Imain -Imain/gal \
        tests/test_gal_tone.c main/gal/gal_tone.c
    run_case gal-font-check env PYTHONDONTWRITEBYTECODE=1 python3 tools/gal_font.py \
        --check --out-dir assets/fonts
    local demo
    for demo in audio low_power ble wifi; do
        run_case "demo-${demo}-runtime" cc_case "demo-${demo}-runtime" "${cflags[@]}" \
            -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
            "tests/test_demo_${demo}_runtime.c" tests/demo_stubs/demo_missing_stubs.c \
            -Wl,--gc-sections
    done
    run_case deep-sleep-contract env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_deep_sleep_contract.py
    run_case key-wake-contract env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_key_wake_contract.py
    run_case check-repo-tests env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_check_repo.py
    run_case verify-firmware-tests env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_verify_firmware.py
    run_case archive-firmware-tests env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_archive_firmware.py
    run_case install-passport-skills-tests env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_install_passport_skills.py

    # Summer exit 3 is PARTIAL (source audit or trace skipped), not a PASS.
    bash tools/summer_gate.sh >"${host_log_dir}/summer-gate.log" 2>&1 || summer_rc=$?
    if [[ ${summer_rc} -eq 0 ]]; then
        results+=("PASS | summer-gate | ${host_log_dir}/summer-gate.log")
        host_pass=$((host_pass + 1))
        printf 'PASS %-30s %s\n' "summer-gate" "${host_log_dir}/summer-gate.log"
    elif [[ ${summer_rc} -eq 3 ]]; then
        results+=("PARTIAL | summer-gate (exit 3) | ${host_log_dir}/summer-gate.log")
        host_partial=$((host_partial + 1))
        printf 'PARTIAL %-27s %s\n' "summer-gate" "${host_log_dir}/summer-gate.log"
    else
        results+=("FAIL | summer-gate (rc=${summer_rc}) | ${host_log_dir}/summer-gate.log")
        host_fail=$((host_fail + 1))
        printf 'FAIL %-30s %s\n' "summer-gate" "${host_log_dir}/summer-gate.log"
        tail -8 "${host_log_dir}/summer-gate.log" || true
    fi

    echo
    echo "Host test results:"
    local line
    for line in "${results[@]}"; do echo "  ${line}"; done
    echo "Host summary: PASS=${host_pass} FAIL=${host_fail} SKIP=${host_skip} PARTIAL=${host_partial}"
    echo "Host run dir: ${host_run_dir}"
    echo "Host log dir: ${host_log_dir}"

    if [[ ${host_fail} -ne 0 ]]; then
        echo "Host tests: FAIL (${host_fail} failure(s))"
        return 1
    fi
    if [[ ${host_skip} -ne 0 || ${host_partial} -ne 0 ]]; then
        echo "Host tests: PARTIAL (${host_skip} skip(s), ${host_partial} partial(s))"
        return 3
    fi
    echo "Host tests: PASS"
    return 0
}

run_firmware_checks() (
    local build_dir log_dir
    mkdir -p "${repo_root}/build/firmware-validate" "${repo_root}/build/logs"
    build_dir="$(mktemp -d "${repo_root}/build/firmware-validate/run-${stamp}.XXXXXX")" || return 1
    log_dir="$(mktemp -d "${repo_root}/build/logs/firmware-validate-${stamp}.XXXXXX")" || return 1

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." \
            | tee "${log_dir}/env.log" >&2
        echo "Firmware log dir: ${log_dir}"
        return 1
    fi

    if ! SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${build_dir}" -D "SDKCONFIG=${build_dir}/sdkconfig" build \
        >"${log_dir}/build.log" 2>&1; then
        tail -8 "${log_dir}/build.log" || true
        echo "Firmware build: FAIL (log=${log_dir}/build.log)" >&2
        return 1
    fi
    if ! idf.py -B "${build_dir}" merge-bin \
        -o "${build_dir}/FoloToy-AI-Passport-full.bin" \
        >"${log_dir}/merge.log" 2>&1; then
        tail -8 "${log_dir}/merge.log" || true
        echo "Firmware merge: FAIL (log=${log_dir}/merge.log)" >&2
        return 1
    fi
    if ! python3 tools/verify_firmware.py "${build_dir}" \
        >"${log_dir}/verify.log" 2>&1; then
        tail -8 "${log_dir}/verify.log" || true
        echo "Firmware verify: FAIL (log=${log_dir}/verify.log)" >&2
        return 1
    fi
    if ! PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${build_dir}" --archive-root "${repo_root}/build/firmware" \
        >"${log_dir}/archive.log" 2>&1; then
        tail -8 "${log_dir}/archive.log" || true
        echo "Firmware archive: FAIL (log=${log_dir}/archive.log)" >&2
        return 1
    fi
    if ! install -m 0644 "${build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"; then
        echo "Firmware copy: FAIL (${repo_root}/build/FoloToy-AI-Passport-full.bin)" >&2
        return 1
    fi
    echo "Firmware build: PASS"
    echo "Firmware build dir: ${build_dir}"
    echo "Firmware log dir: ${log_dir}"
    return 0
)

run_all_checks() {
    local static_rc=0 firmware_rc=0
    run_static_checks || static_rc=$?
    run_firmware_checks || firmware_rc=$?
    # FAIL wins over PARTIAL, and PARTIAL wins over a later PASS.
    if [[ ${static_rc} -eq 1 || ${firmware_rc} -eq 1 ]]; then
        return 1
    fi
    if [[ ${static_rc} -eq 3 || ${firmware_rc} -eq 3 ]]; then
        return 3
    fi
    return 0
}

cd "${repo_root}"
case "${mode}" in
    --all)
        run_all_checks
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
