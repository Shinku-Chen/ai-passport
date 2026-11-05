<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 《PARQUET》· AI Passport 320×240 横屏阅读器

把《PARQUET》（YUZUSOFTSOUR / JUNOS / NekoNyan / HIKARI FIELD）的 RW5 手表移植
`parquet-rw5` 做成 AI Passport 的 **320×240 横屏**视觉小说阅读器：
**107 场景 / 434 剧本块 / 11,205 条 op / 11,098 句正文 / 2 处选项（4 个选项）/
4 个结局端 / 37 组 367 张 CG / 979 个立绘 id**，全程离线。

## 玩法与三键

| 操作 | 阅读中 | 列表 / 选项 |
| --- | --- | --- |
| OK 短按 | 推进一页；本页还在打字时立刻显示整页 | 确认 |
| OK 长按 | 打开菜单 | 返回上一级 |
| UP 短按 | 推进一页 | 光标上移 |
| UP 长按 | 快进，松手即停 | — |
| DOWN 短按 | 历史回看 | 光标下移 |
| DOWN 长按 | 开关自动阅读 | — |

## 功能

- 打字机正文，速度可调（可设立即显示）
- 自动阅读：整页打完后自动翻页，期间屏幕不休眠
- 快进瞬显：整页立刻出现，不逐字追赶
- 存档 / 读档：多槽手动档 + 自动档（每次转场写入）
- 历史回看、章节列表、CG 鉴赏
- 空闲：60 s 调暗、300 s 熄屏、1800 s 深睡，任意键唤醒并回到原进度

## 构建与刷写

```bash
idf.py -B build-release build
idf.py -B build-release merge-bin -o "$PWD/build-release/FoloToy-AI-Passport-full.bin"
python -m esptool --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 \
  build-release/FoloToy-AI-Passport-full.bin
```

板卡：ESP32-C3、8 MB Flash、无 PSRAM；刷机与数据保留规则见 [`docs/development/engineering/firmware-layout.md`](docs/development/engineering/firmware-layout.md)。

## 发布

- tag：`v1.0.0-parquet`（分支 `feature/port-parquet-landscape`）
- 合并固件：`FoloToy-AI-Passport-full.bin`（7,247,152 B）
- 应用镜像：7,181,616 B，版本 `1.0.0`
- 分区占用：**86.29%**，余量 **1,141,456 B**

## 已知限制

- 分区占用 86.29%，余量 1,141,456 B；改动内容前仍需确认 Flash 预算。
- 内容覆盖范围与取舍见 [docs/parquet-port.zh_CN.md](docs/parquet-port.zh_CN.md)。

## 上游与来源

- 上游移植仓库：`parquet-rw5`（快照 `8ab38afe77c6`；仓库地址上游未公开，此处记录快照提交）
- 应用基线：[`FoloToy/ai-passport`](https://github.com/FoloToy/ai-passport) 的 `feature/galgame-framework`
- 原作品：《PARQUET》；剧本、美术与字库版权归原权利方，上游项目未声明许可，本移植仅供个人、非商业使用

## 移植经验

十二部作品共用的阅读器契约、真机踩坑与验收清单：[`docs/reference/shinku-chen/galgame-port/`](https://github.com/Shinku-Chen/ai-passport/tree/feature/galgame-framework/docs/reference/shinku-chen/galgame-port)。

## 许可

本仓代码与上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 同为 MIT（见 `LICENSE`）。剧本、美术与字库来自原作及其社区移植，版权归原权利方；本移植仅供个人、非商业使用。
