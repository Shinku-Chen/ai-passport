<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# DRACU-RIOT! —— AI Passport 视觉小说阅读器

把小米手环同人移植版《DRACU-RIOT!》带到 AI Passport 的**竖屏**离线视觉小说阅读器：
**五条女主线、各自的后日谈与结局**，共 **62 个章节节点**，**244 张事件 CG、85 张背景**
全部打进固件。阅读引擎用 C + LVGL 重写，用设备三键操作。

## 三键操作

列表页 **UP/DOWN** 移动光标，**OK** 确认，**长按 OK** 返回。阅读时 **UP/DOWN** 逐句翻，
**长按 UP** 快进，**长按 DOWN** 切换自动阅读，**OK** 打开菜单（存档、读档、章节列表、回标题）。
选项用 UP/DOWN + OK。

## 功能

- 开机直接进标题页，从序幕一路读到结局，不中途切出去。
- 五条女主线，各有专属结局与后日谈，由选择历史驱动分叉。
- 五个手动存档位 + 一个换场景自动写入的自动档。
- 历史、章节列表、CG 鉴赏、可调文字速度；自动阅读期间屏幕不休眠。
- 空闲依次调暗、熄屏、深睡，任意键唤醒。

## 构建与刷写

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash monitor
```

## 发布与限制

发布版本 **v0.1.0-dracu-riot**。已知限制：仅竖屏 240×320，无语音与音乐，单一固定中文字库子集。

经验文档：
[docs/reference/shinku-chen/dracu-riot](https://github.com/Shinku-Chen/ai-passport/tree/docs/dracu-riot-archive-experience/docs/reference/shinku-chen/dracu-riot)。

## 来源与许可

剧本与美术来自《DRACU-RIOT!》的小米手环同人移植版；原作版权归 Yuzusoft。
本项目为个人非商业移植，请支持原作。
