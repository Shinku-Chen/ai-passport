#!/usr/bin/env python3
"""Startup / locking contract for the 9-nine- reader.

This is a source contract, not a hardware test. It asserts the boot ordering that
the UI depends on and that no non-LVGL task calls a UI setter without the BSP
lock:

  landscape success is required before the app is considered ready
  a CJK-font error page is shown when landscape or app init fails
  the first frame is flushed before the backlight is enabled
  queue/task/button failures build an error page and roll back what was created
  the idling/drawing paths hold bsp_lvgl_lock around UI calls

Usage: python3 tests/nine_host/test_boot_contract.py
"""
from __future__ import annotations
import re
import sys
from pathlib import Path


def slice_between(text: str, start: str, end: str) -> str:
    a = text.index(start)
    b = text.index(end, a)
    return text[a:b]


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    main_c = (root / 'main/main.c').read_text('utf-8')
    app_c = (root / 'main/nine/nine_app.c').read_text('utf-8')
    errors = []

    if 'bsp_lvgl_set_landscape(true)' not in main_c or 'landscape_ok' not in main_c:
        errors.append('landscape result is not checked')
    if 'show_boot_error("横屏初始化失败")' not in main_c:
        errors.append('landscape failure has no visible error')
    if '&nine_cjk_16' not in slice_between(main_c, 'static void show_boot_error', 'static void set_backlight'):
        errors.append('boot error page does not use the generated CJK font')
    init_at = main_c.index('nine_app_init(&s_app)')
    refresh_at = main_c.index('lv_refr_now(NULL)', init_at)
    backlight_at = main_c.index('bsp_display_backlight(BACKLIGHT_FULL)', refresh_at)
    if not (init_at < refresh_at < backlight_at):
        errors.append('backlight is enabled before the first frame is flushed')
    if 'show_boot_error("阅读器初始化失败")' not in main_c:
        errors.append('app init failure has no visible error')
    for msg, cleanup in (('输入队列创建失败', 'nine_media_stop'),
                         ('输入任务创建失败', 'vQueueDelete'),
                         ('按键初始化失败', 'vTaskDelete')):
        if f'show_boot_error("{msg}")' not in main_c:
            errors.append(f'{msg}: no error page')
        block_start = main_c.index(f'ESP_LOGE(TAG, "{msg}')
        block = main_c[block_start:block_start + 700]
        if cleanup not in block:
            errors.append(f'{msg}: missing rollback ({cleanup})')

    # UI ownership: each entry point that may touch LVGL takes the BSP lock.
    for fn, end in (('bool nine_app_key(', 'void nine_app_tick('),
                    ('void nine_app_tick(', 'uint32_t nine_app_idle_ms('),
                    ('void nine_app_show_sleeping(', '// ---------------------------------------------------------------- 初始化')):
        body = slice_between(app_c, fn, end)
        if 'bsp_lvgl_lock' not in body:
            errors.append(f'{fn.strip()}: missing bsp_lvgl_lock')
    init_body = app_c[app_c.index('bool nine_app_init('):]
    if init_body.index('bsp_lvgl_lock') > init_body.index('gal_ui_create'):
        errors.append('nine_app_init calls gal_ui_create before taking the lock')

    if errors:
        for e in errors:
            print('FAIL', e)
        return 1
    print('PASS boot contract: landscape/backlight ordering, CJK error page, rollback, locks')
    return 0


if __name__ == '__main__':
    sys.exit(main())
