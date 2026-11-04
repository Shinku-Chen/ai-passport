<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Vendored TJpgDec (host tests only)

This directory is a copy of `espressif/esp_jpeg` 1.3.2's `tjpgd/` sources
(`tjpgd.c`, `tjpgd.h`, `tjpgdcnf.h`) from the ESP-IDF component registry. It is
used only by the host image regression so the production
`main/steins0_image.c` decoder callback is exercised with the real TJpgDec
implementation instead of a Pillow mirror.

License: Apache-2.0, as distributed by Espressif's `esp_jpeg` component. The
firmware build uses the managed component (declared in `main/idf_component.yml`),
not this copy.

Local change: `tjpgd.h` always includes `<stdint.h>` instead of defining the
integer types under `_WIN32`; older Windows compilers disagreed with the host
C library's `stdint.h`. The firmware uses the unmodified component copy, where
`_WIN32` is not defined.
