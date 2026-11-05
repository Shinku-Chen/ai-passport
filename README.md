<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 今天吃啥（What to Eat Today）

AI Passport 上的**横屏**食物转盘小助手：不知道吃什么时，按住按键滚动食物彩票，松开停在某个随机食物上。

## 操作

**按下任意键（UP / DOWN / OK）** 开始高速滚动，**松开** 停在最后一次显示的帧上（画面自带菜名 / 店名）；**长按 OK** 切换渲染路径（LVGL 局部刷新 / 直刷 panel），仅用于上板 A/B 对比。

## 功能

- **两类转盘** —— 「去处盘」与「吃啥盘」各是一段帧序列，每轮等概率选一类。
- **结果即画面** —— 帧里自带菜名 / 店名，不需要额外文字层。
- **非阻塞按键** —— 按键回调跑在 button 任务里，动画用 LVGL timer 驱动。

## 依赖

**单机离线**：不需要手机、网络或账号。

## 构建与刷写

```bash
idf.py set-target esp32c3 && idf.py build
idf.py merge-bin
esptool.py --chip esp32c3 -p <PORT> -b 460800 write_flash 0x0 build/merged-binary.bin
```

## 已知限制

- 结果完全随机，不按口味、距离或营业时间筛选。
- 帧序列固定，新增食物需要重新生成帧并重新烧录。

## 许可

MIT，见 [`LICENSE`](LICENSE)。
