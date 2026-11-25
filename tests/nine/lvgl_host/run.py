#!/usr/bin/env python3
"""Build and run the real 32-bit LVGL production-path test.

Requires `python -m ziglang` (32-bit i386-windows-gnu) and Pillow. Outputs
PPM/PNG pages, a contact sheet and pool statistics under build/evidence/.
"""
from __future__ import annotations
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
LVGL = ROOT / "managed_components/lvgl__lvgl"
HOST = ROOT / "tests/nine/lvgl_host"
BUILD = ROOT / "build/lvgl-prod"
LVGL_OBJ = ROOT / "build/lvgl-host"
EVIDENCE = ROOT / "build/evidence"
PAGES = EVIDENCE / "lvgl-pages"
ZIG = [sys.executable, "-m", "ziglang", "cc", "-target", "i386-windows-gnu"]
INC = [
    "-I" + str(HOST), "-I" + str(LVGL), "-I" + str(ROOT / "tests/nine"),
    "-I" + str(ROOT / "tests/gal_stubs"), "-I" + str(ROOT / "components/bsp/include"),
    "-I" + str(ROOT / "main"), "-I" + str(ROOT / "main/gal"),
    "-I" + str(ROOT / "main/nine"), "-I" + str(ROOT / "main/nine/tjpgd"),
]
DEFS = ["-DLV_CONF_INCLUDE_SIMPLE", "-DNINE_READER", "-DNINE_MEDIA_HOST", "-DNINE_STORE_HOST"]
APP = [
    "main/nine/nine_app.c", "main/nine/nine_pack.c", "main/nine/nine_vm.c",
    "main/nine/nine_store.c", "main/nine/nine_image.c", "main/nine/nine_media.c",
    "main/nine/tjpgd/tjpgd.c", "main/gal/gal_text.c", "main/gal/gal_settings.c",
    "main/gal/gal_ui.c", "tests/nine/nine_assets_host.c", "tests/nine/nvs_stub_large.c",
    "tests/nine/tinfl_shim.c", "tests/nine/lvgl_host/host_stubs.c",
    "assets/nine/yukiiro/nine_cjk_16.c", "assets/nine/yukiiro/nine_cjk_20.c",
    "tests/nine/lvgl_host/test_lvgl_production.c",
]


def run(cmd, log):
    p = subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if p.returncode:
        log.flush()
        raise SystemExit(f"command failed ({p.returncode}): {' '.join(cmd)}")


def main() -> int:
    if not (ROOT / "assets/nine/yukiiro/nine_script.bin").exists():
        raise SystemExit("missing pack assets")
    for d in (BUILD, LVGL_OBJ, PAGES):
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    with open(EVIDENCE / "lvgl-prod-build.log", "w", encoding="utf-8", errors="replace") as log:
        lvgl_srcs = [p for p in sorted(LVGL.glob("src/**/*.c")) if "drivers" not in p.parts]
        for i, p in enumerate(lvgl_srcs):
            obj = LVGL_OBJ / f"{i:03d}.o"
            run(ZIG + ["-O1", *DEFS, *INC, "-c", str(p.relative_to(ROOT)), "-o", str(obj)], log)
        lvgl_objs = [str(p) for p in sorted(LVGL_OBJ.glob("*.o"))]
        for n in range(0, len(lvgl_objs), 100):
            chunk = lvgl_objs[n:n + 100]
            cmd = [sys.executable, "-m", "ziglang", "ar", "rcs" if n == 0 else "q",
                   str(BUILD / "liblvgl.a"), *chunk]
            run(cmd, log)
        app_objs = []
        test_obj = None
        for p in APP:
            obj = BUILD / (p.replace("/", "_") + ".o")
            run(ZIG + ["-O1", "-g", *DEFS, *INC, "-c", p, "-o", str(obj)], log)
            if p.endswith("test_lvgl_production.c"):
                test_obj = obj
            else:
                app_objs.append(obj)
        run([sys.executable, "-m", "ziglang", "ar", "rcs", str(BUILD / "libapp.a"),
             *[str(p) for p in app_objs]], log)
        run(ZIG + ["-o", str(BUILD / "test_lvgl_production.exe"), str(test_obj),
                   str(BUILD / "libapp.a"), str(BUILD / "liblvgl.a")], log)
    with open(EVIDENCE / "lvgl-prod-run.log", "w", encoding="utf-8", errors="replace") as log:
        p = subprocess.run([str(BUILD / "test_lvgl_production.exe")], cwd=ROOT,
                           stdout=log, stderr=subprocess.STDOUT, timeout=300)
        log.flush()
        if p.returncode:
            print((EVIDENCE / "lvgl-prod-run.log").read_text(errors="replace")[-2000:])
            return p.returncode
    # PPM -> PNG + contact sheet.
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("Pillow is required for the contact sheet")
        return 2
    files = sorted(PAGES.glob("*.ppm"))
    thumbs = []
    for f in files:
        im = Image.open(f).convert("RGB")
        im.thumbnail((320, 240))
        thumbs.append((f.stem, im.copy()))
    cols = 4
    rows = (len(thumbs) + cols - 1) // cols
    lh = 18
    sheet = Image.new("RGB", (cols * 320, rows * (240 + lh)), (16, 16, 16))
    dr = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(thumbs):
        x, y = (i % cols) * 320, (i // cols) * (240 + lh)
        sheet.paste(im, (x, y))
        dr.text((x + 4, y + 242), name, fill=(230, 230, 230))
        im.save(PAGES / (name + ".png"))
    sheet.save(PAGES / "contact-sheet.png")
    print(f"PASS: {len(thumbs)} pages; {PAGES / 'contact-sheet.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
