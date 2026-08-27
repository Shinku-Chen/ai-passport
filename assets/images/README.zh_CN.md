<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 图片资源（Images）

本目录存放项目可复用的图片资源，如 UI 图标、背景、RGB565 资源等。

## 如何使用

- 图片文件复制到本目录，并在本项目 `README.md` 记录分辨率、格式、用途与来源。
- 与固件集成时，参考 [`components/bsp/include/bsp_display.h`](../../components/bsp/include/bsp_display.h) 与相关示例分支的图片资源管线，转换为固件所需格式（如 RGB565 数组）。
- 图片资源占用 Flash 与内存，集成前请评估 ESP32-C3 无 PSRAM 的限制。

## 目录说明

> 加入资源时请同步更新本 `README.md` 的索引。

## 当前资源

| 文件 | 尺寸 | 格式 | 说明 |
| --- | --- | --- | --- |
| `eat-what-cover.png` | 2048 × 2048 | PNG (RGB) | 「今天吃啥」应用封面图：卡通电视展示汉堡、拉面、饺子、火锅、奶茶。来源为用户提供的应用封面，保留为可编辑原图。 |
