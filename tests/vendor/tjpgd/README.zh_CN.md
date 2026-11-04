[English](README.md) | [简体中文](README.zh_CN.md)

# 宿主测试用 TJpgDec 副本

`tjpgd.c`、`tjpgd.h`、`tjpgdcnf.h` 复制自项目的
`managed_components/espressif__esp_jpeg/tjpgd` 依赖（TJpgDec R0.03，
ChaN，2021；许可证见 `tjpgd.c` 文件头）。仅用于让
`tests/test_stella_image.c` 编译真实图像层，避免依赖被忽略的
`managed_components/` 或 build 目录种子。

唯一修改在 `tjpgd.h`：把 `_WIN32` 的 typedef 分支改为 `_MSC_VER`；
Zig/Clang 在 Windows 上提供 `<stdint.h>`，原分支会重复定义
`uint32_t`/`int32_t`。
