#!/usr/bin/env bash
# Host smoke: real cafe_ui + real LVGL software display, 320x240, 64 KB pool.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
lvgl=managed_components/lvgl__lvgl
if [[ ! -d "$lvgl" ]]; then
    echo "ui smoke: SKIP (managed_components/lvgl__lvgl absent)"
    exit 0
fi
out="${CAFE_UI_OUT:-reports/cafestella-media-20261123-1130/ui-smoke}"
log="${CAFE_UI_LOG:-reports/cafestella-media-20261123-1130/ui-smoke.log}"
mkdir -p "$out" "$(dirname "$log")"
export CAFE_UI_OUT="$out"
rsp="$(mktemp /tmp/cafe-ui-smoke.XXXXXX.rsp)"
trap 'rm -f "$rsp"' EXIT
{
    echo "-std=gnu11"
    echo "-O1"
    echo "-DLV_CONF_INCLUDE_SIMPLE"
    find "$lvgl/src" -name '*.c'
} > "$rsp"
${CC:-gcc} @"$rsp" \
    -Itests/ui_smoke -Itests/cafe_stubs -Itests/gal_stubs -Imain/cafe -Imain/gal \
    -I"$lvgl" -I"$lvgl/src" \
    tests/ui_smoke/smoke.c main/cafe/cafe_app.c main/cafe/cafe_pack.c \
    main/cafe/cafe_vm.c main/cafe/cafe_store.c main/cafe/cafe_input.c \
    main/cafe/cafe_compat.c main/cafe/cafe_image.c main/cafe/cafe_ui.c \
    main/cafe/tjpgd.c main/gal/gal_text.c assets/fonts/gal_cjk_16.c \
    assets/fonts/gal_cjk_20.c tests/cafe_app_stubs/cafe_audio_stub.c tests/cafe_stubs/tinfl_shim.c tests/gal_stubs/nvs_stub.c \
    -o /tmp/cafe_ui_smoke.exe
/tmp/cafe_ui_smoke.exe assets/cafe/cafe.pak | tee "$log"
python3 - "$out" <<'PY'
from PIL import Image, ImageDraw
from pathlib import Path
import sys
out = Path(sys.argv[1])
files = sorted(out.glob('*.ppm'))
for f in files:
    Image.open(f).convert('RGB').save(f.with_suffix('.png'))
ims = [Image.open(f.with_suffix('.png')) for f in files]
if ims:
    w, h = 320, 240
    cols = 2
    rows = (len(ims) + 1) // 2
    sheet = Image.new('RGB', (cols * w, rows * (h + 22)), (20, 20, 20))
    d = ImageDraw.Draw(sheet)
    for i, im in enumerate(ims):
        x = (i % cols) * w
        y = (i // cols) * (h + 22)
        sheet.paste(im, (x, y))
        d.text((x + 4, y + h + 4), files[i].stem, fill=(230, 230, 230))
    sheet.save(out / 'contact-sheet.png')
print('ui smoke PNGs:', len(files))
PY
python3 tools/cafe_quality_compare.py | tee -a "$log"
