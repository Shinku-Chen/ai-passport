[English](README.md) | [简体中文](README.zh_CN.md)

# Vendored TJpgDec host-test copy

`tjpgd.c`, `tjpgd.h` and `tjpgdcnf.h` are copied from the project's
`managed_components/espressif__esp_jpeg/tjpgd` dependency (TJpgDec R0.03,
ChaN, 2021; see the license header in `tjpgd.c`). They are vendored only so
`tests/test_stella_image.c` can compile the real image layer without relying on
an ignored `managed_components/` directory or a build-directory seed.

The only edit is in `tjpgd.h`: the `_WIN32` typedef branch was changed to
`_MSC_VER`, because Zig/Clang on Windows provides `<stdint.h>` and the original
branch redefines `uint32_t`/`int32_t`.
