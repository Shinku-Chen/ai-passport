#!/usr/bin/env bash
# 9-nine- reader host gate: real packages, real source traces and a real image
# decoder. Used by tools/validate.sh --static. Set NINE_SOURCE to the pinned
# nine-haruiro-rw5 checkout to also run the independent source/package audit.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
pack="assets/nine/haruiro"
work="$(mktemp -d "${TMPDIR:-/tmp}/nine-host.XXXXXX")"
cc_bin="${CC:-cc}"
if ! command -v "$cc_bin" >/dev/null 2>&1; then cc_bin=gcc; fi
trap 'rm -rf "$work"' EXIT

run() { "$@" >>"$work/log" 2>&1; }

echo "== 9-nine host tests =="
"$cc_bin" -std=gnu11 -Wall -Wextra -Werror -Imain/nine -Itests/nine_host \
    tests/nine_host/test_nine_trace.c main/nine/nine_pack.c main/nine/nine_crc32.c main/nine/nine_vm.c \
    tests/nine_host/tinfl_shim.c -o "$work/test_nine_trace"
run "$work/test_nine_trace" "$pack/nine_script.bin" tests/nine_host/fixtures/nine_haruiro.trace
echo "PASS trace: $(tail -1 "$work/log")"

"$cc_bin" -std=gnu11 -Wall -Wextra -Werror -Imain/nine -Itests/nine_host \
    tests/nine_host/test_nine_synth.c main/nine/nine_pack.c main/nine/nine_crc32.c main/nine/nine_vm.c \
    tests/nine_host/tinfl_shim.c -o "$work/test_nine_synth"
run "$work/test_nine_synth" tests/nine_host/fixtures/synth
echo "PASS synth: $(grep -c '^PASS synth' "$work/log") cases"

"$cc_bin" -std=gnu11 -Wall -Wextra -Werror -Imain/nine -Itests/nine_host \
    tests/nine_host/test_nine_vm_state.c main/nine/nine_pack.c main/nine/nine_crc32.c main/nine/nine_vm.c \
    tests/nine_host/tinfl_shim.c -o "$work/test_nine_vm_state"
run "$work/test_nine_vm_state" "$pack/nine_script.bin"
echo "PASS vm state: save/load, corruption, preview, permanent merge, history"

"$cc_bin" -std=gnu11 -Wall -Wextra -Werror -DNINE_APP_HOST -DNINE_READER=1 \
    -Imain -Imain/nine -Imain/gal -Itests/nine_host/stubs -Itests/nine_host \
    -Itests/gal_app_stubs \
    tests/nine_host/test_nine_app.c main/nine/nine_app.c main/nine/nine_pack.c \
    main/nine/nine_crc32.c main/nine/nine_vm.c main/nine/nine_input.c main/gal/gal_settings.c main/gal/gal_text.c \
    tests/nine_host/stubs/gal_ui_stub.c tests/nine_host/stubs/nine_io_stub.c \
    tests/nine_host/stubs/nvs_stub.c tests/nine_host/stubs/nine_host_data.c \
    tests/nine_host/tinfl_shim.c -o "$work/test_nine_app"
run "$work/test_nine_app" "$pack/nine_script.bin" "$pack/nine_images.bin"
echo "PASS app: $(tail -1 "$work/log")"

"$cc_bin" -std=gnu11 -O2 -Wall -Wextra -Werror -DNINE_APP_HOST -DNINE_READER=1     -DNINE_IMAGE_HOST -DNINE_MEDIA_HOST     -Imain -Imain/nine -Imain/nine/tjpgd -Imain/gal -Itests/nine_host/stubs     -Itests/nine_host -Itests/gal_app_stubs     tests/nine_host/test_nine_badpack.c main/nine/nine_app.c main/nine/nine_pack.c main/nine/nine_crc32.c     main/nine/nine_vm.c main/nine/nine_image.c main/nine/nine_media.c     main/nine/tjpgd/tjpgd.c main/gal/gal_settings.c main/gal/gal_text.c     tests/nine_host/stubs/gal_ui_stub.c tests/nine_host/stubs/nine_bsp_stub.c     tests/nine_host/stubs/nvs_stub.c tests/nine_host/stubs/nine_host_data.c     tests/nine_host/tinfl_shim.c -o "$work/test_nine_badpack"
run "$work/test_nine_badpack" "$pack/nine_script.bin" "$pack/nine_images.bin"
echo "PASS badpack: $(tail -1 "$work/log")"

"$cc_bin" -std=gnu11 -O2 -Wall -Wextra -Werror -DNINE_MEDIA_HOST -DNINE_IMAGE_HOST     -Imain/nine -Imain/nine/tjpgd -Itests/nine_host     tests/nine_host/test_nine_audio.c main/nine/nine_media.c main/nine/nine_image.c     main/nine/nine_pack.c main/nine/nine_crc32.c main/nine/tjpgd/tjpgd.c tests/nine_host/tinfl_shim.c     -o "$work/test_nine_audio"
run "$work/test_nine_audio" "$pack/nine_script.bin" "$pack/nine_images.bin"
echo "PASS audio: $(tail -1 "$work/log")"

python3 tests/nine_host/nine_image_ref.py "$pack/nine_script.bin" "$pack/nine_images.bin" \
    "$work/imgref" >"$work/imgref.log" 2>&1
"$cc_bin" -std=gnu11 -O2 -Wall -Wextra -Werror -DNINE_IMAGE_HOST \
    -Imain/nine -Imain/nine/tjpgd -Itests/nine_host \
    tests/nine_host/test_nine_image.c main/nine/nine_image.c main/nine/nine_pack.c main/nine/nine_crc32.c \
    main/nine/tjpgd/tjpgd.c tests/nine_host/tinfl_shim.c -o "$work/test_nine_image"
run "$work/test_nine_image" "$pack/nine_script.bin" "$pack/nine_images.bin" "$work/imgref"
echo "PASS image: $(tail -1 "$work/log" | sed 's/^PASS image: //')"

python3 tools/nine_font_check.py --pack "$pack" --fonts assets/fonts
python3 tests/nine_host/test_boot_contract.py
if [[ -n "${NINE_SOURCE:-}" && -d "$NINE_SOURCE/src/common/story" ]]; then
    python3 tools/nine_audit.py --source "$NINE_SOURCE" --pack "$pack"
else
    echo "SKIP source audit: set NINE_SOURCE to the pinned nine-haruiro-rw5 checkout"
fi
echo "== 9-nine host tests PASS =="
