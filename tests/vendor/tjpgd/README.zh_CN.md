<p align="right"><a href="README.md">English</a> · <strong>简体中文</strong></p>

# 内置的 TJpgDec（仅宿主机测试）

本目录是 ESP-IDF 组件仓库中 `espressif/esp_jpeg` 1.3.2 的 `tjpgd/` 源码副本
（`tjpgd.c`、`tjpgd.h`、`tjpgdcnf.h`），只供宿主机图像回归使用，让生产代码
`main/steins0_image.c` 的解码回调跑在真实 TJpgDec 上，而不是 Pillow 镜像。

许可：Apache-2.0（随 Espressif `esp_jpeg` 组件分发）。固件构建使用
`main/idf_component.yml` 声明的 managed component，不使用本副本。

本地改动：`tjpgd.h` 始终包含 `<stdint.h>`，不再在 `_WIN32` 下自行定义整数
类型；旧版 Windows 编译器与宿主 C 库的 `stdint.h` 会冲突。固件侧使用未改动
的组件副本（不定义 `_WIN32`）。
