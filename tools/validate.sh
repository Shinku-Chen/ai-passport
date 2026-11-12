#!/usr/bin/env bash
# Repository gate: static checks (repo lint + host C/unittest suites) and the
# firmware build/merge/verify/archive pipeline.
#
# Evidence policy (deliberate, do not "simplify" back):
#   * Every run gets a unique directory under build/gate-logs/<stamp>-<pid>/.
#     Nothing is deleted afterwards — not the host test binaries, not the
#     firmware build tree, not a single log. Previous runs stay on disk.
#   * Each case is counted individually (PASS / FAIL / SKIP). The final summary
#     prints those counts and the log directory; there is no unconditional
#     "PASS" line.
#   * Only `CLICK`-style evidence matters: a case passes when its command exits
#     0 and its log was captured. Compile/run output is never redirected to
#     /dev/null; on failure the single most relevant error line is printed and
#     the full log path is shown.
#   * SKIP is used only when a case was not executed because evidence/input was
#     not supplied (currently STEINS_SOURCE). A command that actually ran and
#     failed — including a baseline demo compile/link failure — is FAIL, never
#     SKIP. SKIP never becomes PASS, and any SKIP keeps the result PARTIAL.
#   * A failing case always makes the script exit non-zero. PARTIAL exits 0 by
#     default so a machine without the read-only source snapshot can still run
#     the gate; set STEINS_GATE_STRICT=1 to make PARTIAL exit 3 as well.
set -uo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

host_cc=()
if [[ -n "${CC:-}" ]]; then
    read -r -a host_cc <<< "${CC}"
elif python -c 'import ziglang' >/dev/null 2>&1; then
    host_cc=(python -m ziglang cc)
elif python3 -c 'import ziglang' >/dev/null 2>&1; then
    host_cc=(python3 -m ziglang cc)
else
    host_cc=(cc)
fi

run_stamp="$(date +%Y%m%d-%H%M%S)-$$"
gate_dir="${STEINS_GATE_DIR:-${repo_root}/build/gate-logs/${run_stamp}}"
host_dir="${gate_dir}/host"
firmware_dir="${gate_dir}/firmware"
mkdir -p "${host_dir}" "${firmware_dir}"

pass_count=0
fail_count=0
skip_count=0        # 用例级 SKIP:无法执行或证据缺失,影响整体结论
subskip_count=0     # 套件内部 SKIP:平台受限的子用例,随套件一起报,不翻转结论
failed_cases=()
skipped_cases=()
subskipped_cases=()

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
    echo "  evidence dir: build/gate-logs/<stamp>-<pid>/ (override with STEINS_GATE_DIR)" >&2
    echo "  STEINS_SOURCE=<steins-rw5 checkout> enables the independent source audit" >&2
    echo "  STEINS_GATE_STRICT=1 makes PARTIAL exit non-zero" >&2
}

# 最相关的一行错误:编译看 error:/undefined symbol,运行看 assert/FAIL。
first_error() {
    local log="$1"
    [[ -s "${log}" ]] || { echo "(empty log)"; return 0; }
    local line
    line="$(grep -a -m1 -E "error:|undefined symbol|Assertion|assert|FAIL |Error:" "${log}" || true)"
    if [[ -z "${line}" ]]; then
        line="$(tail -n 1 "${log}")"
    fi
    printf '%s' "${line}" | tr -d '\r' | cut -c1-240
}

record_pass() {
    pass_count=$((pass_count + 1))
    printf 'PASS | %s | %s\n' "$1" "$2"
}

record_fail() { # name, detail, log
    fail_count=$((fail_count + 1))
    failed_cases+=("$1")
    printf 'FAIL | %s | %s | log: %s\n' "$1" "$2" "$3"
}

record_skip() { # name, reason, log
    skip_count=$((skip_count + 1))
    skipped_cases+=("$1")
    if [[ -n "${3:-}" ]]; then
        printf 'SKIP | %s | %s | log: %s\n' "$1" "$2" "$3"
    else
        printf 'SKIP | %s | %s\n' "$1" "$2"
    fi
}

# 套件内部被跳过的子用例(平台受限):进汇总,但不翻转整体结论。
record_subskip() { # name, count, log
    subskip_count=$((subskip_count + $2))
    subskipped_cases+=("$1: $2")
    printf 'SKIP(sub) | %s | %s sub-case(s) skipped inside the suite | log: %s\n' "$1" "$2" "${3}"
}

# 编译一个宿主机用例:日志进 gate_dir,失败时打印唯一相关错误行。
host_compile() {
    local name="$1"
    shift
    local log="${host_dir}/${name}.compile.log"
    # 命令本身写进日志头:证据可追溯到具体编译器与参数。
    printf '# cmd: %s
' "$*" >"${log}"
    if "$@" >>"${log}" 2>&1; then
        return 0
    fi
    record_fail "${name}" "compile: $(first_error "${log}")" "${log}"
    return 1
}

host_run() {
    local name="$1"
    shift
    local log="${host_dir}/${name}.run.log"
    printf '# cmd: %s
' "$*" >"${log}"
    if "$@" >>"${log}" 2>&1; then
        record_pass "${name}" "run ok (log: ${log})"
        return 0
    fi
    record_fail "${name}" "run: $(first_error "${log}")" "${log}"
    return 1
}

# 编译 + 运行一个 C 用例。编译失败不再尝试运行。
c_case() {
    local name="$1"
    local binary="${host_dir}/${name}"
    shift
    if host_compile "${name}" "$@" -o "${binary}"; then
        host_run "${name}" "${binary}"
    fi
}

# Python 用例:统计套件内部的 skip,并把它们计进 SKIP 汇总(不记成 PASS)。
python_case() {
    local name="$1"
    shift
    local log="${host_dir}/${name}.log"
    printf '# cmd: %s
' "$*" >"${log}"
    if "$@" >>"${log}" 2>&1; then
        local skipped
        skipped="$(grep -a -o -E '\(skipped=[0-9]+\)' "${log}" | tail -n 1 | tr -dc '0-9' || true)"
        record_pass "${name}" "ok (log: ${log})"
        if [[ -n "${skipped}" && "${skipped}" != "0" ]]; then
            record_subskip "${name}" "${skipped}" "${log}"
        fi
    else
        record_fail "${name}" "$(first_error "${log}")" "${log}"
    fi
}

run_static_checks() {
    printf 'gate dir: %s\n' "${gate_dir}"
    printf 'host cc: %s\n' "${host_cc[*]}"

    # Apple ld does not accept --gc-sections; -dead_strip is the equivalent.
    local dead_strip_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        dead_strip_flag="-Wl,-dead_strip"
    fi

    # ---- 仓库自检
    python_case check_repo env PYTHONDONTWRITEBYTECODE=1 python3 tools/check_repo.py

    # ---- GitHub Actions 语法
    local actionlint_bin
    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh 2>"${host_dir}/actionlint-install.log")" || actionlint_bin=""
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        record_fail "actionlint" "actionlint unavailable: $(first_error "${host_dir}/actionlint-install.log")" \
            "${host_dir}/actionlint-install.log"
    else
        local log="${host_dir}/actionlint.log"
        if "${actionlint_bin}" -color .github/workflows/*.yml >"${log}" 2>&1; then
            record_pass "actionlint" "workflows ok (log: ${log})"
        else
            record_fail "actionlint" "$(first_error "${log}")" "${log}"
        fi
    fi

    # ---- 基线 BSP 纯逻辑用例
    c_case test_ui_pixel_math \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Itests/vendor/miniz/up -Wno-unused-function \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c
    c_case test_demo_navigation \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Itests/vendor/miniz/up -Wno-unused-function \
        tests/test_demo_navigation.c main/demo_navigation.c
    c_case test_key_guard \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Itests/vendor/miniz/up -Wno-unused-function \
        tests/test_key_guard.c
    c_case test_bsp_display_rounding \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c
    c_case test_bsp_es8311_sleep_check \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c
    c_case test_bsp_button \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include tests/test_bsp_button.c
    c_case test_bsp_lvgl_init \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c
    c_case test_bsp_audio_recovery \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c
    # GalGame 框架骨架:排版分页 + 方波合成(本应用不编译它们,但基线要保持可用)。
    c_case test_gal_text \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_text.c main/gal/gal_text.c
    c_case test_gal_tone \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Imain/gal \
        tests/test_gal_tone.c main/gal/gal_tone.c
    # 骨架字体的字符集来自 main/gal:本应用用自己的 steins_* 字体。
    python_case gal_font_check \
        env PYTHONDONTWRITEBYTECODE=1 python3 tools/gal_font.py --check \
        --source-root main/gal --out-dir assets/fonts

    # ---- 《命运石之门》阅读器:直接编译 main/ 下的生产源码
    c_case steins_pack_model \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Itests/vendor/miniz/up -Wno-unused-function \
        tests/test_steins_pack_model.c main/steins_pack.c tests/vendor/miniz/up/miniz.c tests/vendor/miniz/up/miniz_tdef.c tests/vendor/miniz/up/miniz_tinfl.c main/steins_model.c \
        assets/fonts/steins_text_metrics.c
    c_case steins_image \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/host_stubs -Itests/image_stubs -Itests/vendor/tjpgd -Imain -Itests/vendor/miniz/up -Wno-unused-function \
        tests/test_steins_image.c main/steins_image.c main/steins_pack.c tests/vendor/miniz/up/miniz.c tests/vendor/miniz/up/miniz_tdef.c tests/vendor/miniz/up/miniz_tinfl.c \
        tests/vendor/tjpgd/tjpgd.c tests/host_stubs/lvgl_stub.c
    # 手机/邮件 checkpoint 编解码(S2 inbox 115 条 + 满上限 700+700 两档)
    c_case steins_checkpoint         "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Itests/vendor/miniz/up -Wno-unused-function         tests/test_steins_checkpoint.c main/steins_model.c main/steins_pack.c tests/vendor/miniz/up/miniz.c tests/vendor/miniz/up/miniz_tdef.c tests/vendor/miniz/up/miniz_tinfl.c         assets/fonts/steins_text_metrics.c
    # 手机/邮件正文的分页容量:源里最长正文 2988 B / 996 码点,必须能完整分页。
    c_case steins_mobile_bodies \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -Imain -Itests/vendor/miniz/up -Wno-unused-function \
        tests/test_steins_mobile_bodies.c main/steins_model.c main/steins_pack.c tests/vendor/miniz/up/miniz.c tests/vendor/miniz/up/miniz_tdef.c tests/vendor/miniz/up/miniz_tinfl.c \
        assets/fonts/steins_text_metrics.c
    c_case steins_app_ui \
        "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
        -Itests/host_stubs -Itests/image_stubs -Itests/vendor/tjpgd -Imain -Itests/vendor/miniz/up -Wno-unused-function \
        tests/test_steins_app.c main/steins_app.c main/steins_ui.c \
        main/steins_image.c main/steins_pack.c tests/vendor/miniz/up/miniz.c tests/vendor/miniz/up/miniz_tdef.c tests/vendor/miniz/up/miniz_tinfl.c main/steins_model.c \
        main/steins_mobile.c main/steins_mobile_pack.c \
        assets/fonts/steins_text_metrics.c tests/host_stubs/lvgl_stub.c \
        tests/host_stubs/nvs_stub.c tests/host_stubs/bsp_battery_stub.c \
        tests/vendor/tjpgd/tjpgd.c

    # 手机页面 + v2 checkpoint 接线。v5 reader 已 landed；实际 adopted pack
    # (assets/steins/steins_pack.bin, 当前 v5) 必须被真实执行，不能因为可选
    # candidate 目录不存在而 SKIP。
    # 路径优先级：显式 override > pack-ready 的 plain/pack > canonical assets。
    # override 缺失/版本不支持是 FAIL，不静默 fallback。
    local mobile_pack="${STEINS_MOBILE_PACK:-}"
    if [[ -n "${mobile_pack}" && ! -f "${mobile_pack}" ]]; then
        record_fail "steins_mobile_pack_override" \
            "STEINS_MOBILE_PACK missing: ${mobile_pack}" ""
        mobile_pack=""
    fi
    if [[ -z "${mobile_pack}" && -f "${repo_root}/build/mobile-integration/pack-ready.json" ]]; then
        mobile_pack="$(python3 -c 'import json,struct,sys; d=json.load(open(sys.argv[1], encoding="utf-8")); p=(d.get("plain") or {}).get("path") or d.get("pack"); b=open(p,"rb").read(16) if p else b""; print(p if p and struct.unpack_from("<I",b,8)[0] in (4,5) else "")' "${repo_root}/build/mobile-integration/pack-ready.json" 2>/dev/null || true)"
    fi
    if [[ -z "${mobile_pack}" ]]; then
        mobile_pack="${repo_root}/assets/steins/steins_pack.bin"
    fi
    local mobile_version=""
    if [[ -f "${mobile_pack}" ]]; then
        mobile_version="$(python3 -c 'import struct,sys; b=open(sys.argv[1],"rb").read(16); print(struct.unpack_from("<I",b,8)[0])' "${mobile_pack}" 2>/dev/null || true)"
    fi
    if [[ ! -f "${mobile_pack}" ]]; then
        record_fail "steins_mobile_ui_store" \
            "adopted mobile pack missing: ${mobile_pack} (restore canonical pack or set STEINS_MOBILE_PACK)" ""
    elif [[ "${mobile_version}" != "4" && "${mobile_version}" != "5" ]]; then
        record_fail "steins_mobile_ui_store" \
            "unsupported mobile pack version=${mobile_version:-unknown} path=${mobile_pack}" ""
    else
        host_compile steins_mobile_ui_store \
            "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
            -Itests/host_stubs -Itests/image_stubs -Itests/vendor/tjpgd -Imain -Itests/vendor/miniz/up -Wno-unused-function \
            tests/test_steins_mobile_ui_store.c main/steins_app.c main/steins_ui.c \
            main/steins_image.c main/steins_pack.c tests/vendor/miniz/up/miniz.c tests/vendor/miniz/up/miniz_tdef.c tests/vendor/miniz/up/miniz_tinfl.c main/steins_model.c \
            main/steins_mobile.c main/steins_mobile_pack.c assets/fonts/steins_text_metrics.c \
            tests/host_stubs/lvgl_stub.c tests/host_stubs/nvs_stub.c \
            tests/host_stubs/bsp_battery_stub.c tests/vendor/tjpgd/tjpgd.c \
            -o "${host_dir}/steins_mobile_ui_store" || true
        if [[ -x "${host_dir}/steins_mobile_ui_store" ]]; then
            if [[ "${mobile_version}" == "5" ]]; then
                host_run steins_mobile_ui_store_v5_run \
                    "${host_dir}/steins_mobile_ui_store" "${mobile_pack}"
            else
                host_run steins_mobile_ui_store_run \
                    "${host_dir}/steins_mobile_ui_store" "${mobile_pack}"
            fi
        fi
    fi

    # ---- 门禁负例:故意让图片测试编译失败,必须拿到非零退出码。
    # 失败即说明门禁会把"编译失败"当成功,这里判 FAIL。
    local neg_log="${host_dir}/gate_negative_compile.log"
    if "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror -DSTEINS_GATE_NEGATIVE \
            -Itests/host_stubs -Itests/image_stubs -Itests/vendor/tjpgd -Imain -Itests/vendor/miniz/up -Wno-unused-function \
            tests/test_steins_image.c main/steins_image.c main/steins_pack.c tests/vendor/miniz/up/miniz.c tests/vendor/miniz/up/miniz_tdef.c tests/vendor/miniz/up/miniz_tinfl.c \
            tests/vendor/tjpgd/tjpgd.c tests/host_stubs/lvgl_stub.c \
            -o "${host_dir}/test_steins_image_negative" >"${neg_log}" 2>&1; then
        record_fail "gate_negative_compile" \
            "broken image test compiled successfully (gate would report PASS)" "${neg_log}"
    else
        record_pass "gate_negative_compile" \
            "intentional compile failure propagated as non-zero (log: ${neg_log})"
    fi

    # ---- 字库覆盖:包里出现过的每个码位都必须有字形
    python_case steins_font_coverage \
        env PYTHONDONTWRITEBYTECODE=1 python3 tools/steins_font.py --check \
        --pack assets/steins/steins_pack.bin --out-dir assets/fonts

    # ---- 独立源审计(需要只读源快照;没有就 SKIP,绝不记 PASS)
    if [[ -n "${STEINS_SOURCE:-}" ]]; then
        python_case steins_source_audit \
            env PYTHONDONTWRITEBYTECODE=1 python3 tools/steins_audit.py \
            --source "${STEINS_SOURCE}" --pack assets/steins/steins_pack.bin \
            --report "${gate_dir}/steins-audit.json"
    else
        record_skip "steins_source_audit" \
            "STEINS_SOURCE not set; point it at the read-only steins-rw5 checkout"
    fi

    # ---- 基线 demo runtime 用例:实际执行编译；非零即 FAIL（不是 SKIP）。
    #       缺源/未执行才是 SKIP；这里不按已知错误串降级。
    local demo
    for demo in audio low_power ble wifi; do
        local name="baseline_demo_${demo}_runtime"
        local bin="${host_dir}/${name}"
        local clog="${host_dir}/${name}.compile.log"
        if ! "${host_cc[@]}" -std=c11 -Wall -Wextra -Werror \
                -ffunction-sections -fdata-sections -Itests/demo_stubs -Imain -Itests/vendor/miniz/up -Wno-unused-function \
                "tests/test_demo_${demo}_runtime.c" main/ui_pixel.c "${dead_strip_flag}" \
                -o "${bin}" >"${clog}" 2>&1; then
            record_fail "${name}" \
                "compile: $(first_error "${clog}")" "${clog}"
            continue
        fi
        local rlog="${host_dir}/${name}.run.log"
        if "${bin}" >"${rlog}" 2>&1; then
            record_pass "${name}" "run ok (log: ${rlog})"
        else
            record_fail "${name}" "run: $(first_error "${rlog}")" "${rlog}"
        fi
    done

    # ---- 工具与固件脚本的单元测试
    python_case steins_mobile_pack_builder \
        env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_steins_mobile_pack_builder.py
    python_case deep_sleep_contract \
        env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    python_case key_wake_contract \
        env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    python_case test_check_repo \
        env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_check_repo.py
    python_case test_verify_firmware \
        env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    python_case test_archive_firmware \
        env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    python_case test_install_passport_skills \
        env PYTHONDONTWRITEBYTECODE=1 python3 tests/test_install_passport_skills.py

    report_summary "Host tests"
}

# 汇总:逐项统计,失败即非零;SKIP 让整体停在 PARTIAL。
report_summary() {
    local label="$1"
    local total=$((pass_count + fail_count + skip_count))
    printf '\n%s summary: %d case(s) — PASS %d, FAIL %d, SKIP %d (plus %d sub-case skip(s))\n' \
        "${label}" "${total}" "${pass_count}" "${fail_count}" "${skip_count}" "${subskip_count}"
    printf 'Evidence dir: %s\n' "${gate_dir}"
    local i
    for ((i = 0; i < ${#subskipped_cases[@]}; i++)); do
        printf '  skipped sub-cases: %s\n' "${subskipped_cases[i]}"
    done
    for ((i = 0; i < ${#skipped_cases[@]}; i++)); do
        printf '  skipped: %s\n' "${skipped_cases[i]}"
    done
    for ((i = 0; i < ${#failed_cases[@]}; i++)); do
        printf '  failed: %s\n' "${failed_cases[i]}"
    done
    if [[ "${fail_count}" -ne 0 ]]; then
        printf '%s: FAIL (%d failing case(s))\n' "${label}" "${fail_count}"
        return 1
    fi
    if [[ "${skip_count}" -ne 0 ]]; then
        printf '%s: PARTIAL (%d skipped case(s) — see evidence dir)\n' "${label}" "${skip_count}"
        return 0
    fi
    printf '%s: PASS\n' "${label}"
    return 0
}

run_firmware_checks() {
    printf 'gate dir: %s\n' "${gate_dir}"
    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available in this shell." >&2
        echo "       On Windows use: powershell -ExecutionPolicy Bypass -File tools/gate-firmware.ps1" >&2
        echo "       (ESP-IDF refuses to run under MSYS; it also writes its own run dir under build/.)" >&2
        return 1
    fi

    local build_log="${firmware_dir}/build.log"
    local merge_log="${firmware_dir}/merge.log"
    local verify_log="${firmware_dir}/verify.log"
    local archive_log="${firmware_dir}/archive.log"
    local archive_verify_log="${firmware_dir}/archive-verify.log"

    if ! SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
            idf.py -B "${firmware_dir}" \
            -D "SDKCONFIG=${firmware_dir}/sdkconfig" build >"${build_log}" 2>&1; then
        record_fail "firmware_build" "$(first_error "${build_log}")" "${build_log}"
        report_summary "Firmware build"
        return 1
    fi
    record_pass "firmware_build" "build ok (log: ${build_log})"

    if ! idf.py -B "${firmware_dir}" merge-bin \
            -o "${firmware_dir}/FoloToy-AI-Passport-full.bin" >"${merge_log}" 2>&1; then
        record_fail "firmware_merge" "$(first_error "${merge_log}")" "${merge_log}"
        report_summary "Firmware build"
        return 1
    fi
    record_pass "firmware_merge" "merge ok (log: ${merge_log})"

    if ! python3 tools/verify_firmware.py "${firmware_dir}" >"${verify_log}" 2>&1; then
        record_fail "firmware_verify" "$(first_error "${verify_log}")" "${verify_log}"
        report_summary "Firmware build"
        return 1
    fi
    record_pass "firmware_verify" "layout ok (log: ${verify_log})"

    if ! PYTHONDONTWRITEBYTECODE=1 python3 tools/archive_firmware.py create \
            "${firmware_dir}" --archive-root "${repo_root}/build/firmware" \
            >"${archive_log}" 2>&1; then
        record_fail "firmware_archive" "$(first_error "${archive_log}")" "${archive_log}"
        report_summary "Firmware build"
        return 1
    fi
    record_pass "firmware_archive" "archive created (log: ${archive_log})"

    local archive_path
    archive_path="$(grep -a -o -E 'build[/\\]firmware[/\\][0-9a-f]{64}' "${archive_log}" | head -n 1 || true)"
    if [[ -z "${archive_path}" ]]; then
        record_fail "firmware_archive_verify" \
            "archive path not found in ${archive_log}" "${archive_log}"
        report_summary "Firmware build"
        return 1
    fi
    archive_path="${repo_root}/${archive_path//\\//}"
    if ! python3 tools/archive_firmware.py verify "${archive_path}" \
            >"${archive_verify_log}" 2>&1; then
        record_fail "firmware_archive_verify" "$(first_error "${archive_verify_log}")" \
            "${archive_verify_log}"
        report_summary "Firmware build"
        return 1
    fi
    record_pass "firmware_archive_verify" "archive verified: ${archive_path} (log: ${archive_verify_log})"

    mkdir -p "${repo_root}/build"
    install -m 0644 "${firmware_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    report_summary "Firmware build"
}

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        static_rc=$?
        run_firmware_checks
        firmware_rc=$?
        if [[ "${static_rc}" -ne 0 || "${firmware_rc}" -ne 0 ]]; then
            exit 1
        fi
        ;;
    --static)
        if ! run_static_checks; then
            exit 1
        fi
        ;;
    --firmware)
        if ! run_firmware_checks; then
            exit 1
        fi
        ;;
    *)
        usage
        exit 2
        ;;
esac

# PARTIAL 默认退出 0(允许没有源快照的机器跑门禁);需要失败即非零时置
# STEINS_GATE_STRICT=1,此时 PARTIAL 退出 3。
if [[ "${skip_count}" -ne 0 && "${STEINS_GATE_STRICT:-0}" == "1" ]]; then
    echo "STRICT: PARTIAL is treated as failure (${skip_count} skipped case(s))" >&2
    exit 3
fi
exit 0
