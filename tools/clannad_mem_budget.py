#!/usr/bin/env python3
"""Derive the CLANNAD reader's C3 DRAM budget from the same build MAP.

This is link-time evidence, not a device heap measurement. Static buffers
(canvas, VM, story scratch, LVGL pool) are inside the MAP's DRAM segment; the
named dynamic allocations below are subtracted from the post-BSS heap region,
and the remainder is reported as an unmeasured reserve for SPI/panel/I2C/NVS
driver allocations and fragmentation.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path


def sym(text: str, name: str) -> int:
    match = re.search(rf"^\s*0x([0-9a-fA-F]+)\s+{name}\s*=", text, re.M)
    if not match:
        raise SystemExit(f"{name} not found in MAP")
    return int(match.group(1), 16)


def parse_int(path: Path, pattern: str, default: int) -> int:
    if not path.is_file():
        return default
    match = re.search(pattern, path.read_text(encoding="utf-8", errors="replace"))
    return int(match.group(1), 0) if match else default


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="build/fw/FoloToy-AI-Passport.map")
    ap.add_argument("--sdkconfig", default="build/fw/sdkconfig")
    ap.add_argument("--json-out", default="build/clannad_mem_budget.json")
    ap.add_argument("--soc-usable-end", default="",
                    help="override SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    map_path = root / args.map
    text = map_path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^dram0_0_seg\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)",
                      text, re.M)
    if not match:
        raise SystemExit("dram0_0_seg not found")
    dram_origin = int(match.group(1), 16)
    dram_length = int(match.group(2), 16)
    data_start = sym(text, r"_data_start")
    data_end = sym(text, r"_data_end")
    bss_start = sym(text, r"_bss_start")
    bss_end = sym(text, r"_bss_end")
    heap_start = sym(text, r"_heap_start")

    if args.soc_usable_end:
        heap_end = int(args.soc_usable_end, 0)
    else:
        idf = Path(os.environ.get("IDF_PATH", ""))
        soc_h = idf / "components/soc/esp32c3/include/soc/soc.h"
        if not soc_h.is_file():
            raise SystemExit("IDF_PATH/soc.h unavailable; pass --soc-usable-end")
        soc = soc_h.read_text(encoding="utf-8", errors="replace")
        start = re.search(r"#define\s+SOC_ROM_STACK_START\s+0x([0-9a-fA-F]+)", soc)
        size = re.search(r"#define\s+SOC_ROM_STACK_SIZE\s+0x([0-9a-fA-F]+)", soc)
        if not start or not size:
            raise SystemExit("SOC_ROM_STACK_* not found")
        heap_end = int(start.group(1), 16) - int(size.group(1), 16)

    sdk = root / args.sdkconfig
    lvgl_pool = parse_int(sdk, r"CONFIG_LV_MEM_SIZE=(\d+)", 32768)
    lvgl_stack = 7168
    input_stack = parse_int(root / "main/main.c",
                            r"#define\s+CLANNAD_INPUT_TASK_STACK\s+(\d+)", 0)
    if input_stack == 0:
        input_stack = parse_int(root / "main/main.c",
                                r"xTaskCreate\(input_task[^,]*,[^,]*,\s*(\d+)", 4096)
    main_stack = parse_int(sdk, r"CONFIG_ESP_MAIN_TASK_STACK_SIZE=(\d+)", 4096)
    idle_stack = parse_int(sdk, r"CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=(\d+)", 1536)
    timer_stack = parse_int(sdk, r"CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=(\d+)", 2048)
    esp_timer_stack = parse_int(sdk, r"CONFIG_ESP_TIMER_TASK_STACK_SIZE=(\d+)", 3584)
    event_stack = parse_int(sdk, r"CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=(\d+)", 2304)

    # The display draw buffer is allocated by esp_lvgl_port from heap. It is
    # sized from the panel's physical width (240), not the LVGL landscape width
    # (320): one buffer, RGB565, BSP_LVGL_DRAW_BUFFER_LINES lines.
    bsp_w = parse_int(root / "components/bsp/include/bsp_pins.h",
                      r"#define\s+BSP_LCD_W\s+(\d+)", 240)
    draw_lines = parse_int(root / "components/bsp/src/bsp_display_lvgl.c",
                           r"BSP_LVGL_DRAW_BUFFER_LINES\s+(\d+)", 40)
    draw = bsp_w * draw_lines * 2
    # The idle task stack is static in the MAP, so it is not subtracted again.
    task_stacks = (main_stack + timer_stack + esp_timer_stack +
                   event_stack + lvgl_stack + input_stack)
    canvas = 320 * 168 * 2
    vm = 14584     # clannad_vm_t, measured by tests/size probe; inside BSS
    raw = 28672    # story block scratch, inside the app struct
    queue_and_tcb = 7 * 128 + 2 * 96 + 16 * 32

    static_used = bss_end - dram_origin + (data_end - data_start)
    heap_region = heap_end - heap_start
    named_dynamic = draw + task_stacks + queue_and_tcb
    reserve = heap_region - named_dynamic
    result = {
        "map": str(map_path),
        "dram_origin": f"0x{dram_origin:08x}",
        "dram_length": dram_length,
        "data": data_end - data_start,
        "bss": bss_end - bss_start,
        "static_used": static_used,
        "heap_start": f"0x{heap_start:08x}",
        "usable_end": f"0x{heap_end:08x}",
        "heap_region": heap_region,
        "static_buffers": {
            "canvas_320x168_rgb565": canvas,
            "story_scratch": raw,
            "vm_struct_approx": vm,
            "app_list_rows": 1040,
            "lvgl_pool": lvgl_pool,
        },
        "display": {
            "physical_lcd_width": bsp_w,
            "draw_buffer_lines": draw_lines,
            "draw_buffer_bytes": draw,
        },
        "idle_stack_static_in_map": idle_stack,
        "lvgl_port_task_stack": lvgl_stack,
        "named_dynamic": {
            "display_draw_buffer": draw,
            "task_stacks": task_stacks,
            "queue_tcb_estimates": queue_and_tcb,
        },
        "reserve_before_unmeasured_drivers": reserve,
        "note": ("Link-time evidence from this MAP. The draw buffer uses the "
                 "physical 240-pixel panel width; the idle task stack is static in "
                 "the MAP and not subtracted again. SPI/panel/I2C/NVS driver "
                 "allocations and fragmentation are not measured; runtime "
                 "free/largest and frame timing require a device."),
    }
    out = root / args.json_out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, ensure_ascii=False, indent=1) + "\n",
                   encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
