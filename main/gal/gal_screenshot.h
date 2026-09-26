// main/gal/gal_screenshot.h -- FAP_SCREENSHOT_V1 serial screenshot.
//
// Publishing this play to the community market requires a real device frame: the
// publisher captures one over USB-CDC before it accepts a submission.
//
// Protocol: the host sends the ASCII line `FAP_SCREENSHOT_V1\n` on USB-CDC and the
// device answers `FAP_SCREENSHOT_V1 <width> <height> RGB565LE <bytes>\n` followed by
// exactly <bytes> little-endian RGB565 pixels. The command is observational: it never
// reboots, flashes, or changes settings.
//
// Every trap below is recorded in
// docs/reference/y2lin/serial-screenshot-protocol.md:
//   * install the USB-serial-JTAG driver first, otherwise reading the command
//     dereferences a NULL driver object -- a boot with no REPL never installs it;
//   * reserve the full-screen buffer at compile time: a contiguous 150 KB block is
//     not reliably available from the runtime heap on a part without PSRAM;
//   * write the payload in chunks sized to the tx ring buffer, and mute logs inside
//     the binary window so a log byte cannot shift the image.
#pragma once

#include "esp_err.h"

// Starts the capture service: installs the USB-serial-JTAG driver, points the console
// at it, and spawns the command reader task. A failure is reported and the reader
// keeps running without screenshots.
esp_err_t gal_screenshot_start(void);
