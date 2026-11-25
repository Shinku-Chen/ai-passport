#!/usr/bin/env python3
"""Source-level startup/lifecycle contract for the Cafe Stella reader.

This is not a device test; it catches the P1 ordering regressions that host
state tests cannot see (backlight before first frame, landscape ignored,
button before input task without rollback, UI created outside the BSP lock).
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parent.parent
main = (root / "main/main.c").read_text(encoding="utf-8")
ui = (root / "main/cafe/cafe_ui.c").read_text(encoding="utf-8")
errors: list[str] = []

def need(text: str, needle: str, why: str) -> None:
    if needle not in text:
        errors.append(why)

need(main, "static uint8_t s_backlight;", "backlight must start off until the first frame")
need(main, "const esp_err_t land_err = bsp_lvgl_set_landscape(true);", "landscape result must be captured")
need(main, "if (land_err != ESP_OK)", "landscape failure must be handled")
need(main, "show_fatal(\"横屏初始化失败，无法开始阅读\")", "landscape failure must show a CJK fatal screen")
need(main, "cafe_ui_show_fatal(message);", "fatal screen must use the CJK UI helper")
need(main, "if (first_frame_ok) lv_refr_now(NULL);", "successful first frame must be refreshed before backlight")
need(main, "set_backlight(BACKLIGHT_FULL);\n    s_app.now_ms", "backlight must be enabled after the first frame")
need(main, "阅读器界面初始化失败", "UI create failure must show the fatal screen, not a lit blank page")
need(main, "if (!s_input_queue)", "input queue creation failure must be handled")
need(main, "s_input_ready = true;", "input events must be gated until the task exists")
need(main, "bsp_lvgl_lock(-1)", "UI/app initialization must run under the BSP LVGL lock")
need(main, "first_frame_ok = cafe_ui_create(&s_ui) && cafe_app_init(&s_app, &s_ui, &s_bands);",
     "UI and app init must share the lock boundary")
need(main, "bsp_button_init(on_key, NULL)", "button must be initialized before input task")
need(main, "(void)bsp_button_init(NULL, NULL);", "button callback must be detached on task failure")
need(main, "vQueueDelete(s_input_queue);", "queue must be rolled back on button/task failure")
need(main, "&s_input_task", "task handle must be captured")
need(ui, "if (!ui->typer)", "LVGL timer allocation failure must fail UI create")
need(ui, "return false;", "UI create must be able to fail")
if errors:
    for e in errors:
        print("FAIL:", e, file=sys.stderr)
    raise SystemExit(1)
print("cafe lifecycle contract: PASS (static source ordering; device NOT RUN)")
