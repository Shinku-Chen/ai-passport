#!/usr/bin/env bash
# Compile and run the real-image (vendored TJpgDec) host regression.
# Usage: tests/steins0_image_gate.sh <cc> <work-dir> [source.c]
# The source-override is used by tests/test_steins0_gate_negative.sh to prove a
# compile failure makes the gate fail (non-zero), not just print a message.
set -uo pipefail

cc="${1:?cc}"
read -r -a cc_cmd <<< "${cc}"
work="${2:?work dir}"
src="${3:-tests/test_steins0_image.c}"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${work}"
bin="${work}/test_steins0_image"

if ! "${cc_cmd[@]}" -std=c11 -Wall -Wextra -Werror \
        -I"${root}/tests/image_stubs" -I"${root}/tests/lvgl_ui_stubs" \
        -I"${root}/tests/vendor/tjpgd" -I"${root}/main" \
        "${src}" "${root}/main/steins0_image.c" "${root}/main/steins0_pack.c" \
        "${root}/tests/vendor/tjpgd/tjpgd.c" "${root}/tests/lvgl_ui_stubs/lvgl_ui_stubs.c" \
        -o "${bin}" >"${work}/image_compile.log" 2>&1; then
    echo "FAIL | real image/TJpgDec host regression | compile failed"
    tail -n 5 "${work}/image_compile.log" || true
    exit 1
fi

if ! "${bin}" >"${work}/image_run.log" 2>&1; then
    echo "FAIL | real image/TJpgDec host regression | run failed"
    tail -n 8 "${work}/image_run.log" || true
    exit 1
fi
echo "PASS | real image/TJpgDec host regression | $(tail -n 1 "${work}/image_run.log")"
