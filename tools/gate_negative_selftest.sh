#!/usr/bin/env bash
# Gate negative self-test: prove that a real failure cannot end with
# "Host tests: PASS" and exit 0.
#
# The injected failures happen **only inside a throw-away copy** under
# build/gate-negative/<stamp>/repo — the working tree is never modified.
# Nothing is deleted afterwards; every copy, log and gate evidence dir stays.
#
#   bash tools/gate_negative_selftest.sh
#
# Cases:
#   1. production C compile break   (append #error to main/steins_model.c)
#   2. production run-time assert   (make a real assertion fail in a host suite)
#   3. unmodified copy, no source   (must be PARTIAL, not PASS)
# Each case also asserts the *unmodified* copy still reports PASS/PARTIAL,
# so a broken copy cannot be confused with a broken gate.
set -uo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date +%Y%m%d-%H%M%S)-$$"
work="${repo_root}/build/gate-negative/${stamp}"
mkdir -p "${work}"

echo "negative self-test dir: ${work}"

# 只读复制:排除 build/、依赖目录与 .git;其余(含未跟踪的新文件)都带上,
# 因为门禁依赖 assets/steins 与 assets/fonts 里的生成物。
copy_tree() {
    local dest="$1"
    mkdir -p "${dest}"
    tar -C "${repo_root}" \
        --exclude=./build \
        --exclude=./.git \
        --exclude=./managed_components \
        --exclude=./tools/__pycache__ \
        -cf - . 2>"${work}/tar-$(basename "${dest}").log" |
        tar -C "${dest}" -xf - 2>>"${work}/tar-$(basename "${dest}").log"
    # 副本自己建一个仓储:check_repo.py 用 git 判"哪些文件必须被跟踪",
    # 没有 .git 的副本会被判 dependencies.lock 未跟踪而误报。
    git -C "${dest}" init -q 2>>"${work}/git-$(basename "${dest}").log"
    git -C "${dest}" add -A 2>>"${work}/git-$(basename "${dest}").log"
    git -C "${dest}" -c user.email=selftest@example.invalid \
        -c user.name=gate-selftest commit -q -m "self-test snapshot" \
        2>>"${work}/git-$(basename "${dest}").log"
}

run_gate() { # name, dir, extra env...
    local name="$1"
    local dir="$2"
    shift 2
    local log="${work}/${name}.log"
    ( cd "${dir}" && env "$@" ./tools/validate.sh --static ) >"${log}" 2>&1
    echo $? >"${work}/${name}.exit"
}

verdict() { # name
    local name="$1"
    local log="${work}/${name}.log"
    local code
    code="$(cat "${work}/${name}.exit" 2>/dev/null || echo missing)"
    printf '%s -> exit=%s | %s\n' "${name}" "${code}" \
        "$(grep -a -E '^Host tests: ' "${log}" | tail -n 1 || echo '(no verdict line)')"
}

fail_case() {
    echo "SELFTEST FAIL | $1 | $2" >&2
    ran_fail=$((ran_fail + 1))
}

ran_fail=0

# ---------------------------------------------------------------- 基线副本
baseline="${work}/repo-baseline"
copy_tree "${baseline}"
run_gate "baseline-no-source" "${baseline}"
verdict "baseline-no-source"
base_code="$(cat "${work}/baseline-no-source.exit")"
base_verdict="$(grep -a -E '^Host tests: ' "${work}/baseline-no-source.log" | tail -n 1 || true)"
if [[ "${base_code}" != "0" ]]; then
    fail_case "baseline-no-source" "unmodified copy exited ${base_code} (expected 0 / PARTIAL)"
fi
if [[ "${base_verdict}" != *"PARTIAL"* && "${base_verdict}" != *"PASS"* ]]; then
    fail_case "baseline-no-source" "unmodified copy verdict '${base_verdict}'"
fi

# ------------------------------------------------- 注入 1:生产 C 编译失败
inject_compile="${work}/repo-inject-compile"
copy_tree "${inject_compile}"
printf '\n#error "gate negative self-test: intentional compile break"\n' \
    >>"${inject_compile}/main/steins_model.c"
run_gate "inject-compile" "${inject_compile}"
verdict "inject-compile"
c_code="$(cat "${work}/inject-compile.exit")"
c_verdict="$(grep -a -E '^Host tests: ' "${work}/inject-compile.log" | tail -n 1 || true)"
if [[ "${c_code}" == "0" ]]; then
    fail_case "inject-compile" "compile break still exited 0"
fi
if [[ "${c_verdict}" == "Host tests: PASS" ]]; then
    fail_case "inject-compile" "compile break still printed 'Host tests: PASS'"
fi
if ! grep -a -q "FAIL | steins_pack_model" "${work}/inject-compile.log"; then
    fail_case "inject-compile" "failing case not reported for steins_pack_model"
fi

# ------------------------------------------- 注入 2:生产用例运行期断言失败
inject_run="${work}/repo-inject-run"
copy_tree "${inject_run}"
# 让真实包读出的一帧必须是空的:包没坏,断言故意写错 → 运行期失败。
python - "${inject_run}/tests/test_steins_pack_model.c" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
needle = 'CHECK(nodes == 74, "node count %u != 74", (unsigned)nodes);'
assert needle in text, "anchor not found"
text = text.replace(needle, 'CHECK(nodes == 999999, "injected failure: node count %u", (unsigned)nodes);', 1)
open(path, "w", encoding="utf-8").write(text)
print("injected assertion failure")
PY
run_gate "inject-run" "${inject_run}"
verdict "inject-run"
r_code="$(cat "${work}/inject-run.exit")"
r_verdict="$(grep -a -E '^Host tests: ' "${work}/inject-run.log" | tail -n 1 || true)"
if [[ "${r_code}" == "0" ]]; then
    fail_case "inject-run" "runtime assertion failure still exited 0"
fi
if [[ "${r_verdict}" == "Host tests: PASS" ]]; then
    fail_case "inject-run" "runtime assertion failure still printed 'Host tests: PASS'"
fi
if ! grep -a -q "FAILED (" "${work}"/repo-inject-run/build/gate-logs/*/host/steins_pack_model.run.log; then
    fail_case "inject-run" "suite did not report FAILED in its own log"
fi

# ------------------------------------------------------------------ 汇总
echo
if [[ "${ran_fail}" -ne 0 ]]; then
    echo "GATE NEGATIVE SELF-TEST: FAIL (${ran_fail}) — evidence: ${work}"
    exit 1
fi
echo "GATE NEGATIVE SELF-TEST: PASS — evidence: ${work}"
echo "  baseline-no-source : ${base_verdict} (exit ${base_code})"
echo "  inject-compile     : ${c_verdict} (exit ${c_code})"
echo "  inject-run         : ${r_verdict} (exit ${r_code})"
exit 0
