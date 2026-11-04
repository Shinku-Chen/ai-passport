#!/usr/bin/env bash
# Gate negative: a compile failure in the image regression must be non-zero,
# and the failure must be the injected one. All evidence stays under build/.
set -uo pipefail

cc="${1:-cc}"
if [[ $# -ge 2 && -n "${2}" ]]; then
    out="${2}"
else
    out="${PWD}/build/gate-negative/$(date +%Y%m%d-%H%M%S)-$$"
fi
mkdir -p "${out}"
out_abs="$(cd "${out}" && pwd)"

cp tests/test_clannad_image.c "${out}/broken_image_test.c"
printf '\n#error injected-gate-failure\n' >>"${out}/broken_image_test.c"

log="${out}/broken_compile.log"
if "${cc}" -std=c11 -O2 -Wall -Wextra -Werror \
    -Itests/image_stubs -Itests/lvgl_ui_stubs -Itests/vendor/tjpgd \
    -Imain -Itests "${out}/broken_image_test.c" main/clannad_image.c \
    main/clannad_pack.c tests/vendor/tjpgd/tjpgd.c \
    tests/lvgl_ui_stubs/lvgl_ui_stubs.c -o "${out}/broken.exe" >"${log}" 2>&1; then
    printf 'gate negative: compile unexpectedly succeeded\n' | tee "${out}/result.log" >&2
    printf 'gate negative log: %s\n' "${log}"
    exit 1
fi
if ! grep -q 'injected-gate-failure' "${log}"; then
    printf 'gate negative: compile failed but not at the injected point\n' \
        | tee "${out}/result.log" >&2
    printf 'gate negative log: %s\n' "${log}"
    exit 1
fi
printf 'Gate negative: PASS (injected compile failure rejected)\n' \
    | tee "${out}/result.log"
printf 'gate negative evidence: %s\n' "${out_abs}"
exit 0
