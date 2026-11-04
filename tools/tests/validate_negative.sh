#!/usr/bin/env bash
# 门禁负例自检：验证 tools/tests/validate_lib.sh 的逐 case 统计会把「本作 C
# 编译失败 / 运行失败」记为 FAIL、使汇总为非零，而不是 echo PASS 或吞掉。
# 只编译独立副本（本目录下生成），不修改生产源码，不删除任何输出。
#
# 退出码：0 = 负例被正确记录（gate 会 FAIL）；1 = 统计机制没有按预期失败。
set -uo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=validate_lib.sh
source "${repo_root}/tools/tests/validate_lib.sh"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)-$$"
NEG_DIR="${NEG_OUT_DIR:-${repo_root}/build/validate-negative-${STAMP}}"
CASE_LOG_DIR="${NEG_DIR}/cases"
mkdir -p "${CASE_LOG_DIR}"
pick_host_cc

cat >"${NEG_DIR}/normal_work_copy.c" <<'EOF'
int main(void) { return 0; }
EOF
cat >"${NEG_DIR}/runfail_work_copy.c" <<'EOF'
int main(void) { return 3; }
EOF
cat >"${NEG_DIR}/broken_work_copy.c" <<'EOF'
#error "forced negative gate: work C compile must fail"
int main(void) { return 0; }
EOF

neg_normal() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror \
        "${NEG_DIR}/normal_work_copy.c" -o "${NEG_DIR}/normal_work_copy.exe" &&
        "${NEG_DIR}/normal_work_copy.exe"
}

neg_broken_compile() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror \
        -c "${NEG_DIR}/broken_work_copy.c" -o "${NEG_DIR}/broken_work_copy.o"
}

neg_broken_run() {
    "${HOST_CC[@]}" -std=c11 -Wall -Wextra -Werror \
        "${NEG_DIR}/runfail_work_copy.c" -o "${NEG_DIR}/runfail_work_copy.exe" &&
        "${NEG_DIR}/runfail_work_copy.exe"
}

printf 'negative output: %s\n' "$(_abs_path "$NEG_DIR")"
record_case negative-normal-compile-run neg_normal
record_case negative-broken-compile neg_broken_compile
record_case negative-broken-run neg_broken_run

summary_rc=0
case_summary "Gate simulation (negative cases expected)" || summary_rc=1

fail=0
if [[ "$CASE_PASS" -ne 1 ]]; then
    printf 'NEGATIVE CHECK FAIL: expected 1 passing control case, got %d\n' "$CASE_PASS"
    fail=1
fi
if [[ "$CASE_FAIL" -ne 2 ]]; then
    printf 'NEGATIVE CHECK FAIL: expected 2 failing cases, got %d\n' "$CASE_FAIL"
    fail=1
fi
if [[ "$summary_rc" -eq 0 ]]; then
    printf 'NEGATIVE CHECK FAIL: gate summary did not turn non-zero\n'
    fail=1
fi
if [[ "$fail" -ne 0 ]]; then
    exit 1
fi
printf 'Negative accounting: PASS | expected FAIL recorded, gate would exit non-zero | %s\n' \
    "$(_abs_path "$CASE_LOG_DIR")"
exit 0
