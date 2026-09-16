<p align="right">
  <a href="ai-guide.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Agent Development Guide

This guide is for AI coding assistants. `AGENTS.md` is the only mandatory starting document; read this guide for code work and route to hardware or engineering references only when the task requires them.

## Establish context

1. Read `AGENTS.md` and follow its task routing. Do not load every README or the entire hardware guide by default.
2. Run `git status --short --branch` and preserve existing changes.
3. Read affected public headers, implementations, and neighboring code. Do not infer this board's behavior from a generic ESP32-C3 board.
4. Search `origin/demo/*` for a relevant example and reuse only applicable design ideas.
5. Decompose the request into inputs, outputs, state, tasks, persistence, memory budget, and failure behavior before choosing `main` or `components/bsp`.
6. Run focused checks while iterating and `./tools/validate.sh` before delivery. Keep hardware checks explicit.

## Source-of-truth priority

```text
product specification / measurement
  > components/bsp/include/bsp_pins.h
  > BSP public headers and implementation
  > docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md
  > README and demo applications
```

If a task requires a board revision, wiring, polarity, register value, or GPIO assignment not defined by these sources, ask the user. Never substitute values from another ESP32-C3 board.

## Application/BSP boundary

```text
requirement
  └─ main/                         pages, state machines, animation, app tasks, assets
      └─ components/bsp/include/  stable board APIs
          └─ components/bsp/src/  buses, devices, and driver details
              └─ bsp_pins.h       pin and hardware-parameter source of truth
```

A new page implements the `enter`, `exit`, and `key` interface in `main/demo_<feature>.c`, is declared in `main/demo.h`, added to `main/CMakeLists.txt`, and registered in `main.c`. Extend menu initialization and failure degradation for new optional peripherals.

Only reusable hardware capabilities belong in the BSP. Document blocking behavior, task context, ownership, failures, and initialization order. Pins and I2C addresses belong only in `bsp_pins.h`.

## Runtime invariants

- Hold `bsp_lvgl_lock()` whenever non-LVGL context accesses LVGL objects.
- Button callbacks dispatch lightweight events only; move audio, storage, networking, and other slow work to worker tasks.
- Stop tasks and timers that may access a page before deleting its screen.
- Preserve menu `UP`/`DOWN`, `OK` click to enter, and page `OK` long-press to return unless the change explicitly redefines them.
- The `main` menu, `demo_*.c` pages, and `ui_pixel` visuals are a board-capability demonstration, not a production application shell. A newly developed application must redesign and implement its own screens and interaction flow from its product requirements instead of directly shipping or copying the current demo UI. Reuse BSP interfaces and applicable implementation patterns; inherit the demo menu or visual theme only when the developer explicitly requests it. A hardware-validation page that intentionally remains part of the baseline demo should still use `ui_pixel_screen_create()` / `ui_pixel_panel_create()` for consistency.
- By default show the battery level in the top-right corner of a user interface (read via `bsp_battery_soc()`), unless the developer specifies a different placement or explicitly does not want it. Degrade gracefully when the reading is `-1` (unavailable) instead of drawing a number. Place it so it does not overlap the cloud decoration (`add_cloud`, around `x≈188, y≈8`) in the top-right; use the clear sky space rather than covering the cloud.
- Budget internal RAM for images, fonts, networking, audio, LVGL, and task stacks; this board has no PSRAM.
- Treat Chinese text as a font-integration task, not just a string translation. Follow the [Chinese font checklist](engineering/coding-conventions.md#chinese-fonts-and-missing-glyphs) when changing text, fonts, sizes, themes, or dynamic content; never hide missing-glyph boxes as a fix.
- Isolate testable state machines, protocols, timing, and layout calculations from ESP-IDF/LVGL and cover them with host tests.

## Material placement

When the developer submits a reusable asset through you — an image, font, audio clip, or similar project material — save it under the repository-root [`assets/`](../../assets/README.md) by default so it stays available for development and later reuse. Place it in the matching subdirectory (`assets/images/`, `assets/fonts/`, `assets/music/`) and record the destination, naming, integration method, and source/license in the [`assets/` README](../../assets/README.md). Never mix binary assets with Markdown documentation. Application or experience archive records (cover, manual, summary) belong in `reference/<username>/`, not in `assets/`; deviate from `assets/` only when the developer explicitly directs another location.

## Delivery

The automated gate is not hardware acceptance. Report `Build`, `Host tests`, `Device tests`, and `Unverified` separately. Use the [hardware guide](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) for the applicable on-device matrix.

### Offer on-device testing

After fully implementing each user-requested firmware feature or fix and
running the applicable validation, proactively ask whether to flash the result
to the device for testing. This applies to each completed development iteration,
not just a project release. Do not end the handoff with only a build result or
firmware path. For example:

> The requested implementation is complete. Would you like me to flash the
> validated firmware to your device and test it now?

1. When device access is available, perform read-only USB/serial discovery
   during handoff. Do not open/reset arbitrary ports or flash a device just
   because it is connected. If multiple candidates exist or the target is
   uncertain, ask the user to identify the intended device and port.
2. If no device is detected, explicitly prompt:

   > No device was detected. Please turn the device on, then connect it to a
   > USB port on your computer using a data-capable USB cable, not a charge-only
   > cable. Let me know when it is connected so I can check again.

   Recheck after the user connects it. If this environment cannot access the
   user's USB devices, state that limitation instead of claiming no device is
   connected; guide the user through local detection/flashing and request the
   test results.
3. Before writing, confirm the target device, the exact validated firmware,
   and its [flashing/data impact](engineering/firmware-layout.md#flashing-and-stored-data),
   then obtain explicit approval for this flash. A connected device or a past
   approval for another build is not authorization. Do not make a backup of the
   original firmware a prerequisite, and do not assume permission for a
   full-chip erase.
4. After approved flashing, verify startup and the requested behavior using
   the applicable hardware checklist, with user observations where needed.
   Successful flashing alone is not `Device tests: PASS`. If the user declines
   or postpones testing, no device is available, or access is unavailable,
   report `Device tests: NOT RUN` and list the pending checks in `Unverified`.

For documentation-only or other tasks that do not change firmware, explain
that on-device testing is not applicable; do not flash unrelated firmware just
to satisfy this workflow. This test invitation does not authorize commits,
pushes, or any of the optional [project-closing actions](release/project-completion.md).

Related documents: [build and test](engineering/build-and-test.md), [coding conventions](engineering/coding-conventions.md), [hardware guide](../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md), [documentation index](../README.md), and [root `AGENTS.md`](../../AGENTS.md).
