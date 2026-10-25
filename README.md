<p align="right">
  <strong>简体中文</strong> · <a href="README.en_US.md">English</a>
</p>

# 今天吃啥（What to Eat Today）

面向 AI Passport 的食物转盘决策小助手。不知道吃什么？按住按键滚动食物彩票，松开停在某个随机食物上。

这是本 `feature/cheerful-goodall` 分支承载的应用。

## 交互

三个按键驱动应用（按键回调跑在 button 任务里、保持非阻塞；动画循环用 LVGL timer 跑）：

- **按下任意键（UP / DOWN / OK）** —— 抽签：先随机决定本轮类型，再从该类型的随机一帧开始高速滚动。
- **松开** —— 停在最后一次显示的帧上，画面里自带菜名/店名，那就是结果。
- **长按 OK** —— 切换渲染路径（LVGL 局部刷新 / 直刷 panel），仅用于上板 A/B。

两种类型各是一段帧序列，每次抽签等概率选一种：

| 类型 | 提示条标签 | 内容 | 帧数 / 速度 |
| --- | --- | --- | --- |
| 去处盘 | `PLACE` | 麦当劳、玻璃屋、热炒、寿司……「今天午餐吃什麼呢?」 | 20 帧 / 10 fps |
| 菜品盘 | `DISH` | 沙县小吃、兰州拉面、汉堡、火锅……「今天吃什么选择器」 | 21 帧 / 20 fps |

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
