#!/usr/bin/env bash
# Negative self-checks for the Summer host gate.
#
# Each case is an independent copy of the repository's needed inputs under a
# unique build/ directory.  The copied tests are broken deliberately, so the
# production tree, pack, fonts and gate are never modified and no evidence is
# deleted.  A case passes only when the copied gate reports FAIL (exit 1).
set -uo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date +%Y%m%d-%H%M%S)"
mkdir -p "${repo_root}/build/gate-selfcheck"
case_root="$(mktemp -d "${repo_root}/build/gate-selfcheck/run-${stamp}.XXXXXX")" || exit 1

copy_repo() {
    local dst="$1"
    mkdir -p "${dst}/assets"
    cp -a "${repo_root}/main" "${dst}/main" || return 1
    cp -a "${repo_root}/tests" "${dst}/tests" || return 1
    cp -a "${repo_root}/tools" "${dst}/tools" || return 1
    cp -a "${repo_root}/assets/fonts" "${dst}/assets/fonts" || return 1
}

run_gate_case() {
    local name="$1"
    local case_dir="${case_root}/${name}"
    local copy="${case_dir}/repo"
    local gate_log="${case_dir}/gate.log"
    mkdir -p "${case_dir}"
    if ! copy_repo "${copy}"; then
        echo "FAIL ${name}: copy failed"
        return 1
    fi

    case "${name}" in
        compile)
            printf '\nthis_is_not_valid_c;\n' >>"${copy}/tests/test_summer_text.c"
            ;;
        run)
            # Return 1 from the copied test's main; the gate must notice a
            # runtime failure, not just a compile failure.
            python3 - "${copy}/tests/test_summer_text.c" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
old = "int main(void)\n{"
new = "int main(void)\n{\n    return 1;"
if old not in s:
    raise SystemExit("main signature not found")
open(p, "w", encoding="utf-8").write(s.replace(old, new, 1))
PY
            ;;
        *)
            echo "FAIL ${name}: unknown case"
            return 1
            ;;
    esac

    local rc=0
    (
        cd "${copy}" &&
        CC="${copy}/tools/cc-host.sh" env -u SUMMER_SOURCE bash tools/summer_gate.sh
    ) >"${gate_log}" 2>&1 || rc=$?

    echo "negative ${name}: gate rc=${rc} log=${gate_log}"
    if [[ ${rc} -ne 1 ]]; then
        echo "FAIL ${name}: expected gate exit 1, got ${rc}"
        tail -12 "${gate_log}" || true
        return 1
    fi
    if [[ "${name}" == "run" ]] && ! find "${copy}/build/logs" -name text.log -type f 2>/dev/null | grep -q .; then
        echo "FAIL run: the text test never reached execution (compile error?)"
        return 1
    fi
    grep -E "Summer gate summary|SUMMER GATE:" "${gate_log}" || true
    return 0
}

failures=0
run_gate_case compile || failures=$((failures + 1))
run_gate_case run || failures=$((failures + 1))

echo "Selfcheck dir: ${case_root}"
if [[ ${failures} -ne 0 ]]; then
    echo "SUMMER GATE SELFCHECK: FAIL (${failures} case(s))"
    exit 1
fi
echo "SUMMER GATE SELFCHECK: PASS (2/2 independently broken copies rejected)"
exit 0
