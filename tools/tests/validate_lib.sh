#!/usr/bin/env bash
# tools/tests/validate_lib.sh —— validate.sh 与负例自检共用的逐 case 统计与编译器发现。
#
# 设计要点：
#   * 每个 case 写自己的日志到 $CASE_LOG_DIR/<name>.log，PASS/FAIL/SKIP 逐条打印；
#   * 任何 case 失败只记 FAIL，不中断后续 case（由调用方最后决定退出码）；
#   * 本机已知的 baseline demo 链接差异（LVGL/ui_pixel stub 缺符号）会显式记为 SKIP，
#     其它任何失败都记 FAIL，不会被吞掉；
#   * 编译器发现不依赖仓库外或被 gitignore 的个人 wrapper：ZIG_CC → zig →
#     python -m ziglang cc（生成在本次输出目录内）→ CC → cc。
#
# 调用方需先设置：repo_root、CASE_LOG_DIR（目录）。

CASE_PASS=0
CASE_FAIL=0
CASE_SKIP=0
declare -a CASE_FAILED=()
declare -a CASE_SKIPPED=()

_abs_path() {
    local p="$1"
    if [[ -d "$p" ]]; then
        (cd -- "$p" && pwd)
    else
        (cd -- "$(dirname -- "$p")" && printf '%s/%s\n' "$(pwd)" "$(basename -- "$p")")
    fi
}

pick_host_cc() {
    local wrapper
    if [[ -n "${ZIG_CC:-}" ]]; then
        HOST_CC=("$ZIG_CC" -target x86_64-windows-gnu)
    elif command -v zig >/dev/null 2>&1; then
        HOST_CC=("$(command -v zig)" -target x86_64-windows-gnu)
    elif python -c "import ziglang" >/dev/null 2>&1; then
        wrapper="${CASE_LOG_DIR}/zigcc.sh"
        {
            echo '#!/usr/bin/env bash'
            echo 'exec python -m ziglang cc "$@"'
        } > "$wrapper"
        chmod +x "$wrapper"
        HOST_CC=("$wrapper" -target x86_64-windows-gnu)
    else
        HOST_CC=("${CC:-cc}")
    fi
    printf 'compiler: %s\n' "${HOST_CC[*]}"
}

case_log() {
    printf '%s/%s.log' "$CASE_LOG_DIR" "$1"
}

run_case() {
    local name="$1"
    shift
    local log
    log="$(case_log "$name")"
    if "$@" >"$log" 2>&1; then
        CASE_PASS=$((CASE_PASS + 1))
        printf 'PASS | %s | %s\n' "$name" "$(_abs_path "$log")"
        return 0
    fi
    CASE_FAIL=$((CASE_FAIL + 1))
    CASE_FAILED+=("$name")
    printf 'FAIL | %s | %s\n' "$name" "$(_abs_path "$log")"
    local first
    first="$(grep -m1 -E 'error:|undefined symbol|FAIL|Assertion' "$log" || true)"
    [[ -n "$first" ]] && printf '       %s\n' "$first"
    return 1
}

# 只用于本机已知的 baseline demo/LVGL stub 链接差异：命中特征才记为 SKIP。
run_case_allow_baseline_linker_skip() {
    local name="$1"
    shift
    local log
    log="$(case_log "$name")"
    if "$@" >"$log" 2>&1; then
        CASE_PASS=$((CASE_PASS + 1))
        printf 'PASS | %s | %s\n' "$name" "$(_abs_path "$log")"
        return 0
    fi
    if grep -qE 'undefined symbol: (lv_|ui_pixel_)' "$log"; then
        CASE_SKIP=$((CASE_SKIP + 1))
        CASE_SKIPPED+=("$name")
        printf 'SKIP | %s | baseline demo/LVGL stub linker difference | %s\n' \
            "$name" "$(_abs_path "$log")"
        return 0
    fi
    CASE_FAIL=$((CASE_FAIL + 1))
    CASE_FAILED+=("$name")
    printf 'FAIL | %s | %s\n' "$name" "$(_abs_path "$log")"
    local first
    first="$(grep -m1 -E 'error:|undefined symbol|FAIL|Assertion' "$log" || true)"
    [[ -n "$first" ]] && printf '       %s\n' "$first"
    return 1
}

# 包一层，避免 set -e 在 case 失败时提前退出（统计仍由 run_case 记录）。
record_case() { run_case "$@" || true; }
record_case_any() { run_case_allow_baseline_linker_skip "$@" || true; }

skip_case() {
    local name="$1" reason="$2"
    CASE_SKIP=$((CASE_SKIP + 1))
    CASE_SKIPPED+=("$name")
    printf 'SKIP | %s | %s\n' "$name" "$reason"
}

case_summary() {
    local label="$1" result="PASS"
    if [[ "$CASE_FAIL" -gt 0 ]]; then
        result="FAIL"
    elif [[ "$CASE_SKIP" -gt 0 ]]; then
        result="PARTIAL"
    fi
    printf '%s: %s (PASS %d, FAIL %d, SKIP %d)\n' \
        "$label" "$result" "$CASE_PASS" "$CASE_FAIL" "$CASE_SKIP"
    if [[ "$CASE_FAIL" -gt 0 ]]; then
        printf 'failed cases: %s\n' "${CASE_FAILED[*]}"
    fi
    if [[ "$CASE_SKIP" -gt 0 ]]; then
        printf 'skipped cases: %s\n' "${CASE_SKIPPED[*]}"
    fi
    [[ "$CASE_FAIL" -eq 0 ]]
}
