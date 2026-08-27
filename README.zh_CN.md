<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 今天吃啥（What to Eat Today）

面向 AI Passport 的食物转盘决策小助手。不知道吃什么？按住按键滚动食物彩票，松开停在某个随机食物上。

这是本 `feature/cheerful-goodall` 分支承载的应用。

## 交互

三个按键驱动应用（按键回调跑在 button 任务里、保持非阻塞；动画循环用 LVGL timer 跑）：

- **按住 UP** —— 播放引导动画（"今天午餐要吃什么呢？"）。
- **按住 DOWN** —— 滚动食物选择器动画。
- **松开** —— 停在当前帧（落在的食物）。
- **OK（长按）** —— 返回菜单（由 `main.c` 统一拦截）。

"松开"靠轮询 `bsp_button_read_mv()` 检测（本 BSP 无 RELEASE 事件）：松开 ≈ 3300 mV，按住 < 2000 mV。

## 功能

- 右上角电量指示（读取 `bsp_battery_soc()`）实时显示电量；读值为 `-1`（不可用）时优雅降级。
- 自动关机：空闲一段时间后自动断电。
- 无需 PSRAM：食物帧以 LVGL `LV_COLOR_FORMAT_I8` 索引色二进制嵌入（CMake `EMBED_FILES`），动画直接
  从 Flash 播放，无需帧缓冲。

## 固件 / 构建

帧画面来自 `main/eat_what_g1.bin` / `main/eat_what_g2.bin`（256 色调色板 + 每帧像素索引，由
`tools/generate_eat_what_assets.py` 生成）。应用只保留少量懒构建的 `lv_image_dsc_t`（每帧一个小对象，
图像数据留在 Flash），以适配内存预算。

## 封面

- 封面素材：`assets/images/eat-what-cover.png`（2048 × 2048），"展示汉堡、拉面、饺子、火锅、奶茶的可爱卡通电视"。

## 来源

- **分支**：[`feature/cheerful-goodall`](https://github.com/Shinku-Chen/ai-passport/tree/feature/cheerful-goodall)
- 入口：`main/demo_eat_what.c`
