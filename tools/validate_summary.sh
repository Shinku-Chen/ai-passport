#!/usr/bin/env bash
# Shared terminal classification for tools/validate.sh and its maintenance tests.
#
# The rule is intentionally independent of any historical failure count:
#   FAIL    > PARTIAL > PASS
# A real non-zero result is always FAIL, even when its failure count happens to
# match a previously recorded baseline. SKIP is only for work that was not
# executed for an explicit reason (for example, no source checkout supplied).
validate_summary_status() {
    local fails="${1:-0}"
    local skips="${2:-0}"
    if (( fails > 0 )); then
        printf 'FAIL'
        return 1
    fi
    if (( skips > 0 )); then
        printf 'PARTIAL'
        return 0
    fi
    printf 'PASS'
    return 0
}
