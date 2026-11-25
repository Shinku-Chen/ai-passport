#!/usr/bin/env bash
# Production-path LVGL regression: NO NINE_IMAGE_HOST, real LVGL objects/fonts,
# real pack. Builds a 72 KB-pool host LVGL library once, then runs image init,
# app/UI startup and error-screen tests. This is a host proxy, not a device test.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
lv="${root}/managed_components/lvgl__lvgl"
confdir="${NINE_LV_CONF_DIR:-${root}/tests/nine_realgl}"
conf="${confdir}/lv_conf.h"
out="${NINE_REALGL_OUT:-${root}/build/nine/realgl}"
mkdir -p "${out}/obj"
cc="${CC:-cc}"
common=(-std=c11 -O2 -DLV_CONF_INCLUDE_SIMPLE -I"${confdir}" -I"${root}/tests/nine_realgl" -I"${lv}" -I"${lv}/src")
if [[ ! -f "${out}/liblvgl.a" || "${conf}" -nt "${out}/liblvgl.a" ]]; then
    rm -rf "${out}/obj"; mkdir -p "${out}/obj"
    n=0
    while IFS= read -r f; do
        n=$((n+1))
        o="${out}/obj/$(printf '%06d' "$n").o"
        "${cc}" -std=c11 -O2 -w -DLV_CONF_INCLUDE_SIMPLE -I"${confdir}" -I"${root}/tests/nine_realgl" -I"${lv}" -I"${lv}/src" -c "$f" -o "$o"
    done < <(find "${lv}/src" -name '*.c' | sort)
    find "${out}/obj" -name '*.o' -print0 | xargs -0 -n 100 ar rcs "${out}/liblvgl.a"
fi
if [[ ! -f "${out}/font16.o" || "${root}/assets/nine/kokoiro/nine_cjk_16.c" -nt "${out}/font16.o" ]]; then
    "${cc}" -std=c11 -O2 -w -DLV_CONF_INCLUDE_SIMPLE -I"${confdir}" -I"${root}/tests/nine_realgl" -I"${lv}" -I"${lv}/src" -c "${root}/assets/nine/kokoiro/nine_cjk_16.c" -o "${out}/font16.o"
    "${cc}" -std=c11 -O2 -w -DLV_CONF_INCLUDE_SIMPLE -I"${confdir}" -I"${root}/tests/nine_realgl" -I"${lv}" -I"${lv}/src" -c "${root}/assets/nine/kokoiro/nine_cjk_20.c" -o "${out}/font20.o"
fi
inc=(-I"${confdir}" -I"${root}/tests/nine_realgl" -I"${lv}" -I"${lv}/src" -I"${root}/main" -I"${root}/main/gal" -I"${root}/main/nine" -I"${root}/main/nine/tjpgd" -I"${root}/tests/nine_host")
map=(-DNINE_READER=1 -DNINE_MEDIA_HOST)
"${cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -O2 -DLV_CONF_INCLUDE_SIMPLE "${inc[@]}" \
    "${root}/tests/nine_realgl/test_image_production.c" "${root}/main/nine/nine_image.c" \
    "${root}/main/nine/nine_pack.c" "${root}/main/nine/tjpgd/tjpgd.c" \
    "${root}/tests/nine_realgl/heap_stub.c" "${root}/tests/nine_host/tinfl_shim.c" \
    "${out}/liblvgl.a" -o "${out}/test_image_production"
"${cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -O2 -DLV_CONF_INCLUDE_SIMPLE "${inc[@]}" "${map[@]}" \
    "${root}/tests/nine_realgl/test_app_production.c" "${root}/main/nine/nine_app.c" \
    "${root}/main/nine/nine_pack.c" "${root}/main/nine/nine_vm.c" "${root}/main/nine/nine_store.c" \
    "${root}/main/nine/nine_image.c" "${root}/main/nine/nine_media.c" "${root}/main/nine/tjpgd/tjpgd.c" \
    "${root}/main/gal/gal_ui.c" "${root}/main/gal/gal_settings.c" "${root}/main/gal/gal_text.c" \
    "${root}/tests/nine_host/nvs_stub.c" "${root}/tests/nine_realgl/bsp_stub.c" \
    "${root}/tests/nine_realgl/heap_stub.c" "${root}/tests/nine_host/tinfl_shim.c" \
    "${out}/font16.o" "${out}/font20.o" "${out}/liblvgl.a" -o "${out}/test_app_production"
"${cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -O2 -DLV_CONF_INCLUDE_SIMPLE "${inc[@]}" "${map[@]}" \
    "${root}/tests/nine_realgl/test_error_production.c" "${root}/main/nine/nine_app.c" \
    "${root}/main/nine/nine_pack.c" "${root}/main/nine/nine_vm.c" "${root}/main/nine/nine_store.c" \
    "${root}/main/nine/nine_image.c" "${root}/main/nine/nine_media.c" "${root}/main/nine/tjpgd/tjpgd.c" \
    "${root}/main/gal/gal_ui.c" "${root}/main/gal/gal_settings.c" "${root}/main/gal/gal_text.c" \
    "${root}/tests/nine_host/nvs_stub.c" "${root}/tests/nine_realgl/bsp_stub.c" \
    "${root}/tests/nine_realgl/heap_stub.c" "${root}/tests/nine_host/tinfl_shim.c" \
    "${out}/font16.o" "${out}/font20.o" "${out}/liblvgl.a" -o "${out}/test_error_production"
"${cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -O2 -DLV_CONF_INCLUDE_SIMPLE "${inc[@]}" "${map[@]}" \
    "${root}/tests/nine_realgl/test_ui_contract.c" \
    "${root}/main/gal/gal_ui.c" "${root}/main/gal/gal_settings.c" "${root}/main/gal/gal_text.c" \
    "${root}/tests/nine_host/nvs_stub.c" "${root}/tests/nine_realgl/bsp_stub.c" \
    "${out}/font16.o" "${out}/font20.o" "${out}/liblvgl.a" -o "${out}/test_ui_contract"
"${cc}" -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -O2 -DLV_CONF_INCLUDE_SIMPLE "${inc[@]}" "${map[@]}"     "${root}/tests/nine_realgl/render_shots.c" "${root}/main/nine/nine_pack.c"     "${root}/main/nine/nine_image.c" "${root}/main/nine/tjpgd/tjpgd.c"     "${root}/main/gal/gal_ui.c" "${root}/main/gal/gal_settings.c" "${root}/main/gal/gal_text.c"     "${root}/tests/nine_host/nvs_stub.c" "${root}/tests/nine_realgl/bsp_stub.c"     "${root}/tests/nine_realgl/heap_stub.c" "${root}/tests/nine_host/tinfl_shim.c"     "${out}/font16.o" "${out}/font20.o" "${out}/liblvgl.a" -o "${out}/render_shots"
mkdir -p "${root}/build/nine/shots"
"${out}/render_shots" "${root}/assets/nine/kokoiro/nine_script.bin" "${root}/assets/nine/kokoiro/nine_images.bin" "${root}/build/nine/shots"
"${out}/test_image_production" "${root}/assets/nine/kokoiro/nine_script.bin" "${root}/assets/nine/kokoiro/nine_images.bin"
"${out}/test_app_production" "${root}/assets/nine/kokoiro/nine_script.bin" "${root}/assets/nine/kokoiro/nine_images.bin"
"${out}/test_error_production" "${root}/assets/nine/kokoiro/nine_script.bin" "${root}/assets/nine/kokoiro/nine_images.bin"
"${out}/test_ui_contract"
echo "Real-LVGL production regressions: PASS"
