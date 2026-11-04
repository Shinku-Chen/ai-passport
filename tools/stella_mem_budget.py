#!/usr/bin/env python3
"""Derive Stella's C3 DRAM budget from the same MAP as the shipped ELF.

The numbers are static/link-time evidence, not a device heap measurement:
runtime driver allocations (SPI/panel/I2C/NVS) are deliberately left as an
explicit unmeasured reserve instead of being guessed.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path


def parse_map(path: Path) -> dict[str, int | str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    out: dict[str, int | str] = {}
    m = re.search(r"^dram0_0_seg\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)", text, re.M)
    if not m:
        raise SystemExit("dram0_0_seg not found in MAP")
    out["dram_origin"] = int(m.group(1), 16)
    out["dram_length"] = int(m.group(2), 16)
    for name in ("_data_start", "_data_end", "_bss_start", "_bss_end", "_heap_start", "_heap_end"):
        mm = re.search(rf"^\s*0x([0-9a-fA-F]+)\s+{name}\s*=", text, re.M)
        if mm:
            out[name] = int(mm.group(1), 16)
    for key in ("_heap_start", "_heap_end", "dram_origin", "dram_length"):
        if key not in out:
            raise SystemExit(f"{key} not found in MAP")
    return out


def parse_int(path: Path, pattern: str, default: int | None = None) -> int:
    if not path.is_file():
        if default is None:
            raise SystemExit(f"missing {path}")
        return default
    m = re.search(pattern, path.read_text(encoding="utf-8", errors="replace"))
    if not m:
        if default is None:
            raise SystemExit(f"{pattern!r} not found in {path}")
        return default
    return int(m.group(1), 0) if m.group(1).startswith("0x") else int(m.group(1))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="build/FoloToy-AI-Passport.map")
    ap.add_argument("--json-out", default="")
    ap.add_argument("--soc-usable-end", default="",
                    help="override SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE")
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    mp = root / args.map
    vals = parse_map(mp)

    model_h = root / "main/stella_image.h"
    screen_w = parse_int(model_h, r"STELLA_SCREEN_W\s+(\d+)")
    screen_h = parse_int(model_h, r"STELLA_SCREEN_H\s+(\d+)")
    bsp_w = parse_int(root / "components/bsp/include/bsp_pins.h", r"BSP_LCD_W\s+(\d+)")
    draw_lines = parse_int(root / "components/bsp/src/bsp_display_lvgl.c",
                           r"BSP_LVGL_DRAW_BUFFER_LINES\s+(\d+)")
    lvgl_stack = parse_int(root / "managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port.h",
                           r"\.task_stack\s*=\s*(\d+)")
    input_stack = parse_int(root / "main/main.c", r"xTaskCreate\([^,]+,\s*[^,]+,\s*(\d+)")
    sdk = root / "sdkconfig"
    main_stack = parse_int(sdk, r"CONFIG_ESP_MAIN_TASK_STACK_SIZE=(\d+)", 3584)
    idle_stack = parse_int(sdk, r"CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=(\d+)", 1536)
    timer_stack = parse_int(sdk, r"CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=(\d+)", 2048)
    esp_timer_stack = parse_int(sdk, r"CONFIG_ESP_TIMER_TASK_STACK_SIZE=(\d+)", 3584)
    event_stack = parse_int(sdk, r"CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=(\d+)", 2304)
    event_queue_items = parse_int(sdk, r"CONFIG_ESP_SYSTEM_EVENT_QUEUE_SIZE=(\d+)", 32)
    timer_queue_items = parse_int(sdk, r"CONFIG_FREERTOS_TIMER_QUEUE_LENGTH=(\d+)", 10)
    queue_depth = parse_int(root / "main/main.c", r"INPUT_QUEUE_DEPTH\s+(\d+)")
    jpeg_pool = parse_int(root / "main/stella_image.c", r"s_jpeg_pool\s*\[(\d+)\]")

    linker_dram_end = int(vals["dram_origin"]) + int(vals["dram_length"])
    # The heap does not stop at dram0_0_seg: ESP-IDF's C3 memory_layout adds the
    # D/IRAM region up to APP_USABLE_DRAM_END = SOC_ROM_STACK_START - SOC_ROM_STACK_SIZE.
    # Parse those two constants from the same IDF version; allow an explicit override.
    if args.soc_usable_end:
        heap_end = int(args.soc_usable_end, 0)
    else:
        idf_path = Path(os.environ.get("IDF_PATH", ""))
        soc_h = idf_path / "components/soc/esp32c3/include/soc/soc.h"
        if soc_h.is_file():
            txt = soc_h.read_text(encoding="utf-8", errors="replace")
            stack_start = re.search(r"#define\s+SOC_ROM_STACK_START\s+0x([0-9a-fA-F]+)", txt)
            stack_size = re.search(r"#define\s+SOC_ROM_STACK_SIZE\s+0x([0-9a-fA-F]+)", txt)
            if not stack_start or not stack_size:
                raise SystemExit("SOC_ROM_STACK_START/SIZE not found in soc.h")
            heap_end = int(stack_start.group(1), 16) - int(stack_size.group(1), 16)
        else:
            raise SystemExit("IDF_PATH/soc.h unavailable; pass --soc-usable-end")
    heap = heap_end - int(vals["_heap_start"])
    static = int(vals["_heap_start"]) - int(vals["dram_origin"])
    iram_used = int(vals["_data_start"]) - int(vals["dram_origin"])
    data_used = int(vals["_data_end"]) - int(vals["_data_start"])
    bss_used = int(vals["_bss_end"]) - int(vals["_bss_start"])
    align_used = static - iram_used - data_used - bss_used
    canvas = screen_w * screen_h * 2
    draw = bsp_w * draw_lines * 2
    queue = queue_depth * 2  # stella_key_t = two bytes; rounded by FreeRTOS
    task_stacks = (main_stack + idle_stack + timer_stack + esp_timer_stack +
                   event_stack + lvgl_stack + input_stack)
    # TCB/queue control blocks and queued event payloads are estimates, not
    # measured; keep them explicit so the remaining number is a conservative
    # upper bound rather than a claimed runtime heap.
    tcb_estimate = 7 * 128
    queue_control_estimate = 4 * 80
    event_payload_estimate = event_queue_items * 16
    timer_payload_estimate = timer_queue_items * 8
    controls = tcb_estimate + queue_control_estimate + event_payload_estimate + timer_payload_estimate
    remaining = heap - draw - task_stacks - queue - controls
    result = {
        "map": str(mp),
        "dram_origin": f"0x{int(vals['dram_origin']):08x}",
        "dram_length": int(vals["dram_length"]),
        "dram_linker_end": f"0x{linker_dram_end:08x}",
        "soc_usable_end": f"0x{heap_end:08x}",
        "data_start": f"0x{int(vals['_data_start']):08x}",
        "data_end": f"0x{int(vals['_data_end']):08x}",
        "bss_start": f"0x{int(vals['_bss_start']):08x}",
        "bss_end": f"0x{int(vals['_bss_end']):08x}",
        "heap_start": f"0x{int(vals['_heap_start']):08x}",
        "heap_end": f"0x{heap_end:08x}",
        "static_dram": static,
        "iram_used": iram_used,
        "data_used": data_used,
        "bss_used": bss_used,
        "alignment_used": align_used,
        "heap_bytes": heap,
        "canvas_static_bytes": canvas,
        "draw_buffer_bytes": draw,
        "lvgl_task_stack_bytes": lvgl_stack,
        "input_task_stack_bytes": input_stack,
        "main_task_stack_bytes": main_stack,
        "idle_task_stack_bytes": idle_stack,
        "timer_task_stack_bytes": timer_stack,
        "esp_timer_task_stack_bytes": esp_timer_stack,
        "event_task_stack_bytes": event_stack,
        "task_stacks_bytes": task_stacks,
        "input_queue_bytes": queue,
        "tcb_queue_control_estimate_bytes": controls,
        "jpeg_workspace_static_bytes": jpeg_pool,
        "remaining_after_named_allocations": remaining,
        "note": "Static/link-time budget. Not a device measurement; SPI/panel/I2C/NVS runtime allocations remain unmeasured.",
    }
    print(f"MAP: {mp}")
    print(f"DRAM linker {result['dram_origin']}..{result['dram_linker_end']} ({result['dram_length']} B)")
    print(f"SOC usable end (ROM stack excluded): {result['soc_usable_end']}")
    print(f"data {result['data_start']}..{result['data_end']}, bss {result['bss_start']}..{result['bss_end']}")
    print(f"static DRAM={static} B = IRAM {iram_used} + data {data_used} + bss {bss_used} + align {align_used}")
    print(f"heap {result['heap_start']}..{result['heap_end']} = {heap} B")
    print(f"canvas(static)={canvas} draw={draw} stacks={task_stacks} "
          f"(main={main_stack} idle={idle_stack} timer={timer_stack} esp_timer={esp_timer_stack} "
          f"event={event_stack} lvgl={lvgl_stack} input={input_stack}) "
          f"queue={queue} controls~={controls} jpeg(static)={jpeg_pool}")
    print(f"remaining after named runtime allocations = {remaining} B (device NOT RUN)")
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
