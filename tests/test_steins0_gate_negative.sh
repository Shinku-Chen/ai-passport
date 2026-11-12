#!/usr/bin/env bash
# Gate negative test: a broken image-test translation unit must make the image
# gate exit non-zero.  Also checks that the real source still passes.
set -uo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work="${TMPDIR:-/tmp}/steins0-gate-negative-$$"
mkdir -p "${work}"
cc="${CC:-}"
if [[ -z "${cc}" ]]; then
    if command -v cc >/dev/null 2>&1; then cc="cc"; else cc="python -m ziglang cc"; fi
fi

if bash "${root}/tests/steins0_image_gate.sh" "${cc}" "${work}/broken" \
        "${root}/tests/fixtures/steins0_image_broken.c" >"${work}/broken.log" 2>&1; then
    echo "FAIL | gate negative test | broken image source was accepted"
    cat "${work}/broken.log" || true
    exit 1
fi
if ! grep -q "compile failed" "${work}/broken.log"; then
    echo "FAIL | gate negative test | missing compile-failure diagnostic"
    cat "${work}/broken.log" || true
    exit 1
fi
if ! bash "${root}/tests/steins0_image_gate.sh" "${cc}" "${work}/good" \
        "${root}/tests/test_steins0_image.c" >"${work}/good.log" 2>&1; then
    echo "FAIL | gate negative test | real image source failed"
    cat "${work}/good.log" || true
    exit 1
fi
echo "PASS | gate negative test | broken source exits non-zero, real source passes"
