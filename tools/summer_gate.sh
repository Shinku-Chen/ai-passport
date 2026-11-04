#!/usr/bin/env bash
# Summer Pockets host-test gate.  Everything is built into a unique OUT_DIR and
# log directory under build/ and kept for review; nothing is deleted.  Exit codes:
#   0 = PASS, 1 = FAIL, 3 = PARTIAL (a test was skipped).
#
#   SUMMER_SOURCE=<rw5 snapshot> bash tools/summer_gate.sh
#   CC=<wrapper> ... (otherwise tools/cc-host.sh, cc or ziglang is selected)
set -uo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date +%Y%m%d-%H%M%S)"
mkdir -p "${repo_root}/build/summer-gate" "${repo_root}/build/logs"
if [[ -n "${SUMMER_GATE_OUT:-}" ]]; then
    out_dir="${SUMMER_GATE_OUT}"
    mkdir -p "${out_dir}"
else
    out_dir="$(mktemp -d "${repo_root}/build/summer-gate/run-${stamp}.XXXXXX")" || exit 1
fi
log_dir="$(mktemp -d "${repo_root}/build/logs/summer-gate-${stamp}.XXXXXX")" || exit 1
pack="${repo_root}/main/summer_data/summer_pack.bin"

cc_cmd="${CC:-}"
if [[ -z "${cc_cmd}" ]]; then
    # Prefer the repository wrapper; do not depend on an ignored build/bin tool.
    if [[ -x "${repo_root}/tools/cc-host.sh" ]]; then
        cc_cmd="${repo_root}/tools/cc-host.sh"
    elif command -v cc >/dev/null 2>&1; then
        cc_cmd="cc"
    elif python3 -m ziglang cc --version >/dev/null 2>&1; then
        cc_cmd="python3 -m ziglang cc"
    else
        echo "FAIL: no host C compiler (set CC)" | tee "${log_dir}/cc.log"
        exit 1
    fi
fi
read -r -a host_cc <<< "${cc_cmd}"
common=(-std=c11 -Wall -Wextra -Werror -O2 -fno-sanitize=all -fno-stack-protector)
inc=(-I"${repo_root}/tests/summer_host_stubs" -I"${repo_root}/main" -I"${repo_root}/main/vendor/tjpgd")

failures=0
skips=0
declare -a results=()

run_test() {
    local name="$1"
    shift
    local log="${log_dir}/${name}.log"
    if "$@" >"${log}" 2>&1; then
        results+=("PASS | ${name} | ${log}")
        echo "PASS ${name}"
    else
        local rc=$?
        if [[ ${rc} -eq 77 ]]; then
            results+=("SKIP | ${name} | ${log}")
            skips=$((skips + 1))
            echo "SKIP ${name} (exit 77)"
        else
            results+=("FAIL | ${name} (rc=${rc}) | ${log}")
            failures=$((failures + 1))
            echo "FAIL ${name} (rc=${rc})"
            tail -5 "${log}" || true
        fi
    fi
}

if [[ ! -f "${pack}" ]]; then
    echo "FAIL: missing ${pack} (run tools/summer_pack.py)" | tee "${log_dir}/pack.log"
    exit 1
fi

echo "Summer gate: cc=${cc_cmd}"
echo "OUT=${out_dir}"

# --- pure text
if ! "${host_cc[@]}" "${common[@]}" -I"${repo_root}/main" \
    "${repo_root}/tests/test_summer_text.c" "${repo_root}/main/summer_text.c" \
    -o "${out_dir}/test_summer_text" 2>"${log_dir}/text-build.log"; then
    results+=("FAIL | text build | ${log_dir}/text-build.log"); failures=$((failures + 1))
else
    run_test "text" "${out_dir}/test_summer_text"
fi

# --- NVS store (stubbed NVS, fault injection)
if ! "${host_cc[@]}" "${common[@]}" "${inc[@]}" \
    "${repo_root}/tests/test_summer_store.c" "${repo_root}/main/summer_store.c" \
    "${repo_root}/main/summer_model.c" "${repo_root}/main/summer_pack.c" \
    "${repo_root}/tests/summer_host_stubs/stubs.c" \
    -o "${out_dir}/test_summer_store" 2>"${log_dir}/store-build.log"; then
    results+=("FAIL | store build | ${log_dir}/store-build.log"); failures=$((failures + 1))
else
    run_test "store" "${out_dir}/test_summer_store"
fi

# --- real JPEG / mask
if ! "${host_cc[@]}" "${common[@]}" -DSUMMER_IMAGE_HOST "${inc[@]}" \
    "-DSUMMER_PACK_PATH=\"${pack}\"" \
    "${repo_root}/tests/test_summer_image.c" "${repo_root}/main/summer_image.c" \
    "${repo_root}/main/summer_pack.c" "${repo_root}/main/vendor/tjpgd/tjpgd.c" \
    -o "${out_dir}/test_summer_image" 2>"${log_dir}/image-build.log"; then
    results+=("FAIL | image build | ${log_dir}/image-build.log"); failures=$((failures + 1))
else
    run_test "image" "${out_dir}/test_summer_image"
fi

# --- production interpreter over the real pack
if ! "${host_cc[@]}" "${common[@]}" -I"${repo_root}/main" \
    "-DSUMMER_PACK_PATH=\"${pack}\"" \
    "${repo_root}/tests/summer_walk.c" "${repo_root}/main/summer_pack.c" \
    "${repo_root}/main/summer_model.c" \
    -o "${out_dir}/summer_walk" 2>"${log_dir}/walk-build.log"; then
    results+=("FAIL | walk build | ${log_dir}/walk-build.log"); failures=$((failures + 1))
else
    run_test "walk" "${out_dir}/summer_walk"
fi

# --- production app + UI with thin recording stubs
if ! "${host_cc[@]}" "${common[@]}" "${inc[@]}" \
    "${repo_root}/tests/test_summer_app.c" "${repo_root}/main/summer_app.c" \
    "${repo_root}/main/summer_ui.c" "${repo_root}/main/summer_image.c" \
    "${repo_root}/main/summer_store.c" "${repo_root}/main/summer_text.c" \
    "${repo_root}/main/summer_model.c" "${repo_root}/main/summer_pack.c" \
    "${repo_root}/main/vendor/tjpgd/tjpgd.c" \
    "${repo_root}/tests/summer_host_stubs/lvgl_stub.c" \
    "${repo_root}/tests/summer_host_stubs/stubs.c" \
    -o "${out_dir}/test_summer_app" 2>"${log_dir}/app-build.log"; then
    results+=("FAIL | app build | ${log_dir}/app-build.log"); failures=$((failures + 1))
else
    run_test "app" "${out_dir}/test_summer_app"
fi

# --- map visited bits + save version
if ! "${host_cc[@]}" "${common[@]}" -I"${repo_root}/main"     "${repo_root}/tests/test_summer_map_save.c" "${repo_root}/main/summer_pack.c"     "${repo_root}/main/summer_model.c"     -o "${out_dir}/test_summer_map_save" 2>"${log_dir}/map-save-build.log"; then
    results+=("FAIL | map-save build | ${log_dir}/map-save-build.log"); failures=$((failures + 1))
else
    run_test "map-save" "${out_dir}/test_summer_map_save"
fi

# --- strings[106] dates + chapter 137 inherited date label
if ! "${host_cc[@]}" "${common[@]}" -I"${repo_root}/main"     "${repo_root}/tests/test_summer_date.c" "${repo_root}/main/summer_pack.c"     "${repo_root}/main/summer_model.c"     -o "${out_dir}/test_summer_date" 2>"${log_dir}/date-build.log"; then
    results+=("FAIL | date build | ${log_dir}/date-build.log"); failures=$((failures + 1))
else
    run_test "date" "${out_dir}/test_summer_date"
fi

# --- top-level pack table: duplicate/offset/overlap/strict widths
if ! "${host_cc[@]}" "${common[@]}" -I"${repo_root}/main"     "${repo_root}/tests/test_summer_pack_format.c" "${repo_root}/main/summer_pack.c"     -o "${out_dir}/test_summer_pack_format" 2>"${log_dir}/pack-format-build.log"; then
    results+=("FAIL | pack-format build | ${log_dir}/pack-format-build.log"); failures=$((failures + 1))
else
    run_test "pack-format" "${out_dir}/test_summer_pack_format"
fi

# --- save preview 20 chars + ellipsis
if ! "${host_cc[@]}" "${common[@]}" -I"${repo_root}/main"     "${repo_root}/tests/test_summer_save_preview.c" "${repo_root}/main/summer_pack.c"     "${repo_root}/main/summer_model.c"     -o "${out_dir}/test_summer_save_preview" 2>"${log_dir}/preview-build.log"; then
    results+=("FAIL | preview build | ${log_dir}/preview-build.log"); failures=$((failures + 1))
else
    run_test "preview" "${out_dir}/test_summer_save_preview"
fi

# --- app-level bad-portrait rollback + retry
if ! "${host_cc[@]}" "${common[@]}" "${inc[@]}"     "${repo_root}/tests/test_summer_app_image.c" "${repo_root}/main/summer_app.c"     "${repo_root}/main/summer_ui.c" "${repo_root}/main/summer_image.c"     "${repo_root}/main/summer_store.c" "${repo_root}/main/summer_text.c"     "${repo_root}/main/summer_model.c" "${repo_root}/main/summer_pack.c"     "${repo_root}/main/vendor/tjpgd/tjpgd.c"     "${repo_root}/tests/summer_host_stubs/lvgl_stub.c"     "${repo_root}/tests/summer_host_stubs/stubs.c"     -o "${out_dir}/test_summer_app_image" 2>"${log_dir}/app-image-build.log"; then
    results+=("FAIL | app-image build | ${log_dir}/app-image-build.log"); failures=$((failures + 1))
else
    run_test "app-image" "${out_dir}/test_summer_app_image"
fi

# --- source engine <-> production C trace (no review-model dependency)
if [[ -n "${SUMMER_SOURCE:-}" && -d "${SUMMER_SOURCE}" ]] && command -v node >/dev/null 2>&1; then
    if ! "${host_cc[@]}" "${common[@]}" -I"${repo_root}/main"         "${repo_root}/tests/summer_c_trace.c" "${repo_root}/main/summer_pack.c"         "${repo_root}/main/summer_model.c"         -o "${out_dir}/summer_c_trace" 2>"${log_dir}/trace-build.log"; then
        results+=("FAIL | trace build | ${log_dir}/trace-build.log"); failures=$((failures + 1))
    else
        if python3 "${repo_root}/tests/summer_trace_compare.py" --source "${SUMMER_SOURCE}"             --pack "${pack}" --c-trace "${out_dir}/summer_c_trace"             --log-dir "${log_dir}/trace" >"${log_dir}/trace.log" 2>&1; then
            results+=("PASS | source-trace | ${log_dir}/trace.log")
            echo "PASS source-trace"
        else
            results+=("FAIL | source-trace | ${log_dir}/trace.log")
            failures=$((failures + 1))
            echo "FAIL source-trace"
            tail -8 "${log_dir}/trace.log" || true
        fi
    fi
else
    results+=("SKIP | source-trace (SUMMER_SOURCE or node missing) | ${log_dir}/trace.log")
    skips=$((skips + 1))
    echo "SKIP source-trace (SUMMER_SOURCE/node)"
fi

# --- independent source audit
if [[ -n "${SUMMER_SOURCE:-}" && -d "${SUMMER_SOURCE}" ]]; then
    if python3 "${repo_root}/tools/summer_audit.py" --source "${SUMMER_SOURCE}" \
        --pack "${pack}" --report "${log_dir}/audit-report.json" \
        >"${log_dir}/audit.log" 2>&1; then
        results+=("PASS | audit | ${log_dir}/audit.log")
        echo "PASS audit"
    else
        results+=("FAIL | audit | ${log_dir}/audit.log")
        failures=$((failures + 1))
        echo "FAIL audit"
        tail -8 "${log_dir}/audit.log" || true
    fi
else
    results+=("SKIP | audit (SUMMER_SOURCE unset) | ${log_dir}/audit.log")
    skips=$((skips + 1))
    echo "SKIP audit (set SUMMER_SOURCE)"
fi

# --- font coverage
if python3 "${repo_root}/tools/gal_font.py" --check --out-dir "${repo_root}/assets/fonts" \
    >"${log_dir}/font.log" 2>&1; then
    results+=("PASS | font check | ${log_dir}/font.log")
    echo "PASS font check"
else
    results+=("FAIL | font check | ${log_dir}/font.log")
    failures=$((failures + 1))
    echo "FAIL font check"
    tail -4 "${log_dir}/font.log" || true
fi

# --- negative gate 1: a broken image-test compile must be rejected
if "${host_cc[@]}" "${common[@]}" -DSUMMER_IMAGE_HOST "${inc[@]}" \
    "${repo_root}/tests/test_summer_image.c" "${repo_root}/main/summer_image.c" \
    "${repo_root}/main/summer_pack.c" "${repo_root}/main/vendor/tjpgd/tjpgd.c" \
    "${repo_root}/main/this-file-does-not-exist.c" \
    -o "${out_dir}/negative-should-not-exist" >"${log_dir}/negative-compile.log" 2>&1; then
    results+=("FAIL | negative compile unexpectedly succeeded | ${log_dir}/negative-compile.log")
    failures=$((failures + 1))
    echo "FAIL negative compile unexpectedly succeeded"
else
    results+=("PASS | negative compile rejected | ${log_dir}/negative-compile.log")
    echo "PASS negative compile rejected"
fi

# --- negative gate 2: a missing glyph in a copied font subset must be reported
font_neg="${out_dir}/font-negative"
mkdir -p "${font_neg}"
cp "${repo_root}/assets/fonts/gal_cjk_16.c" "${repo_root}/assets/fonts/gal_cjk_20.c" \
   "${repo_root}/assets/fonts/gal_cjk_symbols.txt" "${font_neg}/" 2>/dev/null
printf '\360\237\230\200\n' >> "${font_neg}/gal_cjk_symbols.txt"
if python3 "${repo_root}/tools/gal_font.py" --check --out-dir "${font_neg}" \
    >"${log_dir}/negative-font.log" 2>&1; then
    results+=("FAIL | negative font gate accepted missing glyph | ${log_dir}/negative-font.log")
    failures=$((failures + 1))
    echo "FAIL negative font gate accepted missing glyph"
else
    results+=("PASS | negative font gate rejected | ${log_dir}/negative-font.log")
    echo "PASS negative font gate rejected"
fi

echo
echo "Summer gate results:"
passes=0
for line in "${results[@]}"; do
    echo "  ${line}"
    case "${line%% |*}" in
        PASS) passes=$((passes + 1)) ;;
    esac
done
echo "Summer gate summary: PASS=${passes} FAIL=${failures} SKIP=${skips} TOTAL=${#results[@]}"
echo "OUT=${out_dir}"
echo "LOG=${log_dir}"
if [[ ${failures} -ne 0 ]]; then
    echo "SUMMER GATE: FAIL (${failures} failure(s), ${skips} skip(s))"
    exit 1
fi
if [[ ${skips} -ne 0 ]]; then
    echo "SUMMER GATE: PARTIAL (${skips} skip(s))"
    exit 3
fi
echo "SUMMER GATE: PASS (${passes} case(s))"
exit 0
