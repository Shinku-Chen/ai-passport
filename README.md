<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 生字卡片识记（Shengzi Cards）

AI Passport 上的**横屏**汉字闪卡识记应用：滚动字卡、自测、拼读三种模式，已认识的字持久化到 NVS。

## 操作

**长按上 / 下** 切换 `BROWSE` → `TEST` → `SPELL`。BROWSE 同时显示汉字与拼音，**上** 回上一张、**下** 随机跳一张；TEST 先隐藏拼音，**OK** 揭晓，随后**上** 标记认识、**下** 标记不认识（两者都随机推进）；SPELL 同样 **OK** 揭晓，再按 **OK** 随机推进。

## 功能

- **三种模式** —— 浏览 / 自测 / 拼读，底部信息行显示当前模式、字卡序号与已认识数量。
- **认识标记持久化** —— 已认识的字写入 NVS，重启后保留。
- **随机推进** —— 避免按固定顺序背诵。

## 依赖

**单机离线**：字符表与拼音随固件打包，不需要手机、网络或账号。

## 构建与刷写

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## 发布

tag `v1.0.0`。

## 已知限制

- 字表固定在固件里，不能自行导入。
- 只有「认识 / 不认识」两档，没有复习间隔或错题本。

## 许可

MIT，见 [`LICENSE`](LICENSE)。
