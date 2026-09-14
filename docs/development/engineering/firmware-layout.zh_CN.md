<p align="right">
  <strong>简体中文</strong> · <a href="firmware-layout.md">English</a>
</p>

# 固件布局

本仓库是供用户自定义固件使用的最小基础示例，目标为带 8 MB Flash 的
ESP32-C3。默认布局不预留产品专用身份、OTA 或未使用的数据分区。

## 默认布局

默认分区表只包含：

| 分区 | 类型/子类型 | 偏移 | 大小 | 用途 |
| --- | --- | ---: | ---: | --- |
| `nvs` | data/NVS | `0x9000` | `0x6000` | ESP-IDF 与应用键值存储 |
| `phy_init` | data/PHY | `0xF000` | `0x1000` | PHY 初始化数据 |
| `factory` | app/factory | `0x10000` | `0x7F0000` | 唯一应用镜像，占用全部剩余 Flash |

默认布局没有 OTA 槽；它只是起点，不限制用户固件。

## 自定义布局

用户可以根据应用需要编辑 `partitions.csv`，调整分区大小和偏移，也可以增加或
删除分区。自定义表可以包含 OTA 槽、文件系统/资源分区或其它应用专用数据。
布局应保持在 8 MB 设备边界内、不得重叠，应用镜像的烧录偏移必须是某个容量
足够的 app 分区起点。派生项目改变布局时，应同步更新该项目的文档与烧录说明。

## 强制验证

执行：

```bash
./tools/validate.sh --firmware
```

脚本会在隔离目录中构建并生成合并镜像，从 `flash_args` 读取实际镜像偏移，
校验分区表 MD5、分区边界、标签唯一性和分区不重叠，并确认应用偏移对应一个
容量足够的 app 分区。门禁不会强制要求默认分区列表。CI 执行同一门禁。

只上传 `build/FoloToy-AI-Passport-full.bin`。名称相近的应用单镜像
`build/FoloToy-AI-Passport.bin` 不包含 bootloader 和分区表。

## 烧录与已存数据

验证通过的合并镜像从 `0x0` 写入。合并文件会对各镜像之间的空隙做填充，因此
烧录时可能重置 NVS 与 PHY data 区域。空白设备初始化或有意完整刷新时使用合并
镜像；日常开发若要保留已有 NVS 状态，应使用分段 `idf.py flash`。
`idf.py erase-flash` 会擦除全部用户数据。
