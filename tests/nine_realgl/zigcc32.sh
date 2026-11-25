#!/bin/sh
# Target-like 32-bit host compiler for the real-LVGL regression.
exec python3 -m ziglang cc -target i386-windows-gnu "$@"
