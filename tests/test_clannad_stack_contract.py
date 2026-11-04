#!/usr/bin/env python3
"""Stack-budget contract for the CLANNAD input task.

This is a source-level guard, not a runtime high-water measurement. It keeps
the measured fixes from silently regressing:
  * input task stack is at least 12 KiB. The 2,064/2,096 B numbers are the
    inflate read/validation chains, not the whole-VM worst; the conservative
    expression budget uses 9 * 768 + 736 = 7,648 B, leaving margin for library
    and scheduler calls;
  * refresh_page/show_ending use the app-owned row scratch, not a 1 KiB stack
    local;
  * dynamic DEFLATE uses a 30-symbol distance table instead of a 288-symbol
    table;
  * main.c creates the input task with the named macro.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
errors = []


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


main_c = read("main/main.c")
m = re.search(r"#define\s+CLANNAD_INPUT_TASK_STACK\s+(\d+)", main_c)
if not m:
    errors.append("main.c: CLANNAD_INPUT_TASK_STACK macro is missing")
else:
    stack = int(m.group(1))
    if stack < 12288:
        errors.append(f"main.c: input stack {stack} < 12288")
    if not re.search(
            r'xTaskCreate\(input_task,\s*"clannad_input",\s*CLANNAD_INPUT_TASK_STACK',
            main_c):
        errors.append("main.c: xTaskCreate does not use CLANNAD_INPUT_TASK_STACK")
    if not re.search(r"_Static_assert\(CLANNAD_INPUT_TASK_STACK\s*>=\s*12288", main_c):
        errors.append("main.c: stack compile-time assertion is missing")

app_h = read("main/clannad_app.h")
if "clannad_ui_row_t rows[CLAN_UI_LIST_ROWS]" not in app_h:
    errors.append("clannad_app.h: app-owned row scratch is missing")

app_c = read("main/clannad_app.c")
if "clannad_ui_row_t rows[CLAN_UI_LIST_ROWS];" in app_c:
    errors.append("clannad_app.c: large row array is still a stack local")
if app_c.count("clannad_ui_row_t *rows = app->rows;") < 2:
    errors.append("clannad_app.c: refresh_page/show_ending do not use app->rows")

pack_c = read("main/clannad_pack.c")
if "huff_dist_t" not in pack_c:
    errors.append("clannad_pack.c: compact distance table is missing")
if re.search(r"\bhuff_t\s+dist\b", pack_c):
    errors.append("clannad_pack.c: 288-symbol distance table is still used")
if "build_huff(lit.count, lit.symbol" not in pack_c:
    errors.append("clannad_pack.c: literal table is not built with count/symbol")

if errors:
    for error in errors:
        print("FAIL:", error)
    sys.exit(1)
print("clannad stack contract: PASS "
      "(input 12288, app row scratch, 30-symbol distance table)")
