#!/usr/bin/env bash
# CLANNAD gate: per-case PASS/FAIL/SKIP accounting, preserved evidence under
# build/, no deletion of test or firmware artifacts.
set -uo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

CASE_PASS=0
CASE_FAIL=0
CASE_SKIP=0
HOST_RUN_DIR=""
STAGE=""

note() {
    printf '%s\n' "$*"
}

ensure_host_cc() {
    if [[ -n "${CC:-}" ]]; then
        export CC
        return 0
    fi
    if command -v cc >/dev/null 2>&1; then
        export CC="$(command -v cc)"
        return 0
    fi
    if [[ -x "${repo_root}/tools/host_cc.sh" ]]; then
        export CC="${repo_root}/tools/host_cc.sh"
        return 0
    fi
    echo "ERROR: no host C compiler and tools/host_cc.sh is not executable" >&2
    return 1
}

record_skip() {
    local name="$1"
    local reason="$2"
    local log="${STAGE}/${name}.log"
    {
        printf 'case: %s\n' "${name}"
        printf 'result: SKIP\n'
        printf 'reason: %s\n' "${reason}"
    } >"${log}"
    CASE_SKIP=$((CASE_SKIP + 1))
    note "SKIP ${name}: ${reason} (${log})"
}

run_case() {
    local name="$1"
    shift
    local log="${STAGE}/${name}.log"
    local rc=0
    {
        printf 'case: %s\n' "${name}"
        printf 'started: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'command:'
        printf ' %q' "$@"
        printf '\n\n'
    } >"${log}"
    if "$@" >>"${log}" 2>&1; then
        rc=0
    else
        rc=$?
    fi
    local skipped
    skipped="$(grep -Eo 'skipped=[0-9]+' "${log}" | tail -n 1 | cut -d= -f2 || true)"
    if [[ -n "${skipped}" && "${skipped}" -gt 0 ]]; then
        CASE_SKIP=$((CASE_SKIP + skipped))
    fi
    if [[ "${rc}" -eq 0 ]]; then
        CASE_PASS=$((CASE_PASS + 1))
        note "PASS ${name}"
    else
        CASE_FAIL=$((CASE_FAIL + 1))
        note "FAIL ${name} (exit ${rc}, ${log})"
        printf 'FAIL %s (exit %s)\n' "${name}" "${rc}" >>"${log}"
    fi
    return "${rc}"
}

resolve_actionlint() {
    local dest="${repo_root}/build/tools/actionlint-1.7.12"
    local candidate
    for candidate in "${ACTIONLINT_BIN:-}" "${dest}/actionlint.exe" \
                     "${dest}/actionlint"; do
        if [[ -n "${candidate}" && -x "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    local installed
    installed="$(./tools/install-actionlint.sh "${dest}" \
        2>"${STAGE}/actionlint-install.log" || true)"
    if [[ -n "${installed}" && -x "${installed}" ]]; then
        printf '%s\n' "${installed}"
        return 0
    fi
    return 1
}

run_clannad_cases() {
    run_case "clannad-vm" bash -c \
        '"$CC" -std=c11 -O2 -Wall -Wextra -Werror -Imain -Itests \
         tests/test_clannad_vm.c main/clannad_vm.c main/clannad_pack.c \
         main/clannad_text.c -o "$STAGE/test_clannad_vm" && \
         "$STAGE/test_clannad_vm" assets/clannad/clannad_pack.bin'

    run_case "clannad-image" bash -c \
        '"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
         -Itests/image_stubs -Itests/lvgl_ui_stubs -Itests/vendor/tjpgd \
         -Imain -Itests tests/test_clannad_image.c main/clannad_image.c \
         main/clannad_pack.c tests/vendor/tjpgd/tjpgd.c \
         tests/lvgl_ui_stubs/lvgl_ui_stubs.c -o "$STAGE/test_clannad_image" && \
         "$STAGE/test_clannad_image" assets/clannad/clannad_pack.bin'

    run_case "clannad-app" bash -c \
        '"$CC" -std=c11 -O2 -Wall -Wextra -Werror \
         -Itests/app_stubs -Itests/image_stubs -Itests/lvgl_ui_stubs \
         -Itests/vendor/tjpgd -Imain -Itests tests/test_clannad_app.c \
         main/clannad_app.c main/clannad_ui.c main/clannad_image.c \
         main/clannad_vm.c main/clannad_pack.c main/clannad_text.c \
         tests/vendor/tjpgd/tjpgd.c tests/lvgl_ui_stubs/lvgl_ui_stubs.c \
         tests/app_stubs/nvs_stub.c tests/app_stubs/esp_timer_stub.c \
         -o "$STAGE/test_clannad_app" && \
         "$STAGE/test_clannad_app" assets/clannad/clannad_pack.bin'

    run_case "clannad-stack-contract" env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_clannad_stack_contract.py

    # Host proxy for the target stack probe: the old row array and 288-symbol
    # distance table produced 1,400/2,424-byte host frames. The target frames
    # are recorded separately with -fstack-usage; this compile gate keeps the
    # two measured fixes from silently returning.
    run_case "clannad-frame-budget" bash -c \
        '"$CC" -std=c11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=2048 \
         -Werror=frame-larger-than -Imain -c main/clannad_pack.c \
         -o "$STAGE/clannad_pack_frame.o" && \
         "$CC" -std=c11 -O2 -Wall -Wextra -Werror -Wframe-larger-than=1024 \
         -Werror=frame-larger-than -Itests/app_stubs -Itests/image_stubs \
         -Itests/lvgl_ui_stubs -Itests/vendor/tjpgd -Imain \
         -c main/clannad_app.c -o "$STAGE/clannad_app_frame.o"'

    run_case "clannad-font" env PYTHONDONTWRITEBYTECODE=1 \
        python3 tools/clannad_font.py --check --out-dir assets/fonts

    if [[ -n "${CLANNAD_SOURCE:-}" ]]; then
        run_case "clannad-source-audit" env PYTHONDONTWRITEBYTECODE=1 \
            python3 tools/clannad_audit.py \
            --source "${CLANNAD_SOURCE}" \
            --pack assets/clannad/clannad_pack.bin \
            --json "${STAGE}/clannad_audit.json"
    else
        record_skip "clannad-source-audit" \
            "CLANNAD_SOURCE is unset; independent source/pack audit skipped"
    fi

    run_case "clannad-gate-negative" bash tests/test_gate_negative.sh \
        "${CC}" "${STAGE}/gate-negative"
}

run_static_checks() {
    cd "${repo_root}" || return 1
    ensure_host_cc || {
        echo "Host tests: FAIL (no compiler)"
        return 1
    }

    HOST_RUN_DIR="${repo_root}/build/host-tests/$(date +%Y%m%d-%H%M%S)-$$"
    STAGE="${HOST_RUN_DIR}"
    mkdir -p "${STAGE}"
    export STAGE
    export CC
    note "Host test evidence: ${HOST_RUN_DIR}"
    note "Host compiler: ${CC}"

    run_case "repo-check" env PYTHONDONTWRITEBYTECODE=1 python3 tools/check_repo.py

    local actionlint_bin
    if actionlint_bin="$(resolve_actionlint)"; then
        run_case "actionlint" bash -c '"$1" -color .github/workflows/*.yml' \
            _ "${actionlint_bin}"
    else
        record_skip "actionlint" \
            "no actionlint binary for this platform; log: ${STAGE}/actionlint-install.log"
    fi

    run_case "ui-pixel-math" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror -Imain \
         tests/test_ui_pixel_math.c main/ui_pixel_math.c \
         -o "$STAGE/test_ui_pixel_math" && "$STAGE/test_ui_pixel_math"'
    run_case "demo-navigation" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror -Imain \
         tests/test_demo_navigation.c main/demo_navigation.c \
         -o "$STAGE/test_demo_navigation" && "$STAGE/test_demo_navigation"'
    run_case "key-guard" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror -Imain \
         tests/test_key_guard.c -o "$STAGE/test_key_guard" && "$STAGE/test_key_guard"'
    run_case "bsp-display-rounding" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
         tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
         -o "$STAGE/test_bsp_display_rounding" && "$STAGE/test_bsp_display_rounding"'
    run_case "bsp-es8311" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
         tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
         -o "$STAGE/test_bsp_es8311_sleep_check" && "$STAGE/test_bsp_es8311_sleep_check"'
    run_case "bsp-button" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror \
         -Itests/bsp_stubs -Icomponents/bsp/include \
         tests/test_bsp_button.c -o "$STAGE/test_bsp_button" && "$STAGE/test_bsp_button"'
    run_case "bsp-lvgl-init" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror \
         -Itests/bsp_stubs -Icomponents/bsp/include \
         tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
         -o "$STAGE/test_bsp_lvgl_init" && "$STAGE/test_bsp_lvgl_init"'
    run_case "bsp-audio-recovery" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror \
         -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
         tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
         -o "$STAGE/test_bsp_audio_recovery" && "$STAGE/test_bsp_audio_recovery"'
    run_case "gal-text" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
         tests/test_gal_text.c main/gal/gal_text.c \
         -o "$STAGE/test_gal_text" && "$STAGE/test_gal_text"'
    run_case "gal-tone" bash -c \
        '"$CC" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
         tests/test_gal_tone.c main/gal/gal_tone.c \
         -o "$STAGE/test_gal_tone" && "$STAGE/test_gal_tone"'

    local demo
    for demo in audio low_power ble wifi; do
        run_case "demo-${demo}-runtime" bash -c \
            '"$CC" -std=c11 -Wall -Wextra -Werror \
             -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain \
             "tests/test_demo_${1}_runtime.c" tests/demo_stubs/lvgl_link_stubs.c \
             -o "$STAGE/test_demo_${1}_runtime" && \
             "$STAGE/test_demo_${1}_runtime"' _ "${demo}"
    done

    run_case "deep-sleep-contract" env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_deep_sleep_contract.py
    run_case "key-wake-contract" env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_key_wake_contract.py
    run_case "check-repo-tests" env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_check_repo.py
    run_case "verify-firmware-tests" env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_verify_firmware.py
    run_case "archive-firmware-tests" env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_archive_firmware.py
    run_case "install-skills-tests" env PYTHONDONTWRITEBYTECODE=1 \
        python3 tests/test_install_passport_skills.py

    run_clannad_cases

    note "Host cases: PASS=${CASE_PASS} FAIL=${CASE_FAIL} SKIP=${CASE_SKIP}"
    note "Host test evidence: ${HOST_RUN_DIR}"
    if [[ "${CASE_FAIL}" -gt 0 ]]; then
        note "Host tests: FAIL"
        return 1
    fi
    if [[ "${CASE_SKIP}" -gt 0 ]]; then
        note "Host tests: PARTIAL (${CASE_SKIP} platform/baseline skips)"
        return 0
    fi
    note "Host tests: PASS"
    return 0
}

run_firmware_checks() {
    cd "${repo_root}" || return 1
    local validation_build_dir
    validation_build_dir="${repo_root}/build/firmware-validation/$(date +%Y%m%d-%H%M%S)-$$"
    mkdir -p "${validation_build_dir}"
    note "Firmware validation evidence: ${validation_build_dir}"

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    if ! SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build \
        >"${validation_build_dir}/build.log" 2>&1; then
        echo "Firmware build: FAIL (${validation_build_dir}/build.log)" >&2
        return 1
    fi
    if ! idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        >"${validation_build_dir}/merge.log" 2>&1; then
        echo "Firmware merge: FAIL (${validation_build_dir}/merge.log)" >&2
        return 1
    fi
    if ! python3 tools/verify_firmware.py "${validation_build_dir}" \
        >"${validation_build_dir}/verify.log" 2>&1; then
        echo "Firmware verify: FAIL (${validation_build_dir}/verify.log)" >&2
        return 1
    fi
    if ! PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
        "${validation_build_dir}" --archive-root "${repo_root}/build/firmware" \
        >"${validation_build_dir}/archive.log" 2>&1; then
        echo "Firmware archive: FAIL (${validation_build_dir}/archive.log)" >&2
        return 1
    fi

    # Do not overwrite the delivered image with a validation rebuild; compare.
    local delivered="${repo_root}/build/FoloToy-AI-Passport-full.bin"
    local candidate="${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    if [[ -f "${delivered}" ]]; then
        if cmp -s "${candidate}" "${delivered}"; then
            note "Firmware delivered identity: MATCH"
        else
            note "Firmware delivered identity: DIFFERS (delivered file preserved)"
        fi
    fi
    note "Firmware build: PASS"
    return 0
}

cd "${repo_root}" || exit 1
case "${mode}" in
    --all)
        run_static_checks
        static_rc=$?
        run_firmware_checks
        firmware_rc=$?
        if [[ "${static_rc}" -ne 0 || "${firmware_rc}" -ne 0 ]]; then
            exit 1
        fi
        exit 0
        ;;
    --static)
        run_static_checks
        exit $?
        ;;
    --firmware)
        run_firmware_checks
        exit $?
        ;;
    *)
        usage
        exit 2
        ;;
esac
