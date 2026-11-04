#!/usr/bin/env bash
# Lightweight maintenance test for tools/validate.sh's terminal classification.
#
# It does not claim that a known failure count is acceptable: FAIL wins over
# SKIP, and only a run with zero real failures and at least one explicit SKIP is
# PARTIAL.
set -u

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/validate_summary.sh
source "${root}/tools/validate_summary.sh"

failures=0
check_status() {
    local expected="$1" fails="$2" skips="$3" label="$4"
    local out rc
    out="$(validate_summary_status "${fails}" "${skips}")"
    rc=$?
    if [[ "${out}" != "${expected}" ]]; then
        echo "FAIL | ${label}: expected ${expected}, got ${out}"
        failures=$((failures + 1))
        return 1
    fi
    if [[ "${expected}" == "FAIL" && "${rc}" -eq 0 ]]; then
        echo "FAIL | ${label}: FAIL returned zero"
        failures=$((failures + 1))
        return 1
    fi
    if [[ "${expected}" != "FAIL" && "${rc}" -ne 0 ]]; then
        echo "FAIL | ${label}: ${expected} returned non-zero"
        failures=$((failures + 1))
        return 1
    fi
    echo "PASS | ${label}: ${out} (rc=${rc})"
}

# Historical-looking counts (5/3) are not an input to the classifier; a real
# non-zero failure is always FAIL, whatever the number.
check_status FAIL 2 0 "two failed suites, historical-shaped counts but different failures"
check_status FAIL 1 0 "compile/run non-zero"
check_status FAIL 1 1 "failure plus explicit missing source"
check_status FAIL 5 3 "exactly historical 5+3 but still a real failure"
check_status PARTIAL 0 1 "skip-only control"
check_status PASS 0 0 "normal control"

if grep -nE "baseline_partial|known.*failures|failures.*-le.*known" "${root}/tools/validate.sh" \
        >/dev/null 2>&1; then
    echo "FAIL | validate.sh still contains failure-count downgrade logic"
    failures=$((failures + 1))
else
    echo "PASS | no known-failure-count downgrade logic"
fi
if ! grep -q 'c_fail=$((c_fail + 1))' "${root}/tools/validate.sh" ||
   ! grep -q 'validate_summary_status' "${root}/tools/validate.sh"; then
    echo "FAIL | validate.sh does not record failures through the shared classifier"
    failures=$((failures + 1))
else
    echo "PASS | validate.sh records real failures"
fi

if [[ "${failures}" -gt 0 ]]; then
    echo "FAIL | classification maintenance checks=${failures}"
    exit 1
fi
echo "PASS | validation classification maintenance test"
