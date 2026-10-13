<p align="right">
  <strong>简体中文</strong> · <a href="galgame-content-authoring.md">English</a>
</p>

# GalGame 作品内容填写（简明版）

面向作品作者与 AI 代理：**只改一个文件**，把作品内容填进框架，19 个逻辑屏、
三键操作、剧情锁、存档与进度开箱即用。

> 用法：把本手册、框架分支路径（`feature/galgame-framework`，内容层在
> `main/gal/gal_content.c`）与作品素材一起交给 AI，即可生成完整的内容层与固件。
> 框架内部细节见完整手册 [galgame-framework.zh_CN.md](galgame-framework.zh_CN.md)。

## 1. 只改这一个文件

```
main/gal/gal_content.c     ← 填内容
main/gal/gal_content.h     ← 改作品信息宏;需要时加剧情标记枚举
```

其余模块（界面、状态机、存档、剧情锁、音频）不用动。

当前骨架的“美术与音乐”是程序绘制的渐变与方波合成，因此**纯文字作品可以直接上机**；
要换成真实图片 / 音频见第 6 节。

## 2. 八张表

| 数组 / 宏 | 作用 | 例子 |
| --- | --- | --- |
| `GAL_CONTENT_TITLE` 等 4 个宏 | 作品名、副标题、关于页 logo 副标、版本 | `"示例作品"` |
| `gal_content_lines[]` | 对白。字段：说话人（空串=旁白）、正文、需要的剧情标记 | `{ "主角", "……嗯。", GAL_UNLOCK_NONE }` |
| `gal_content_events[]` | **事件流**：按顺序演。字段：类型、下标、到达时写入的标记 | `{ GAL_EV_LINE, 0, GAL_UNLOCK_NONE }` |
| `gal_content_choices[]` | 选项组：提示句 + 选项数组 | 见下方示例 |
| `gal_content_transitions[]` | 章节过场：归属章节、小标、标题、副标题 | `{ "第 2 章", "CHAPTER 02", "再等一班车", "第二章 · 站台" }` |
| `gal_content_endings[]` | 结局：小标、标题、正文 | `{ "NORMAL END", "再等一班车", "…" }` |
| `gal_content_cgs[]` | CG：编号、名称、解锁事件 | `{ "CG 01", "站台", 1, false }` |
| `gal_content_chapters[]` | 章节：名称、简介、完成度文本、解锁事件、需要的标记 | 见示例 |
| `gal_content_recalls[]` | CG 回想：场景名、章节、需要的 CG、重演起点事件 | 见示例 |
| `gal_content_tracks[]` | 音乐：曲名、时长文本、需要的 CG、音符序列 | 见示例 |
| `gal_content_*_menu[]` | 标题页菜单 6 项、游戏内菜单 6 项（文字可改，项数保持） | `"开始新游戏"…` |
| `gal_content_about_rows[]`、`GAL_CONTENT_ABOUT` | 关于页信息与正文 | `{ "版本", "v0.1.0" }` |

每个数组后面都要有对应的 `const int gal_content_xxx_count = (int)(sizeof(...) / sizeof(...[0]));`
（示例里已经写好，照抄即可）。

## 3. 最小可用示例（可直接替换整个文件）

```c
// main/gal/gal_content.c —— 示例作品内容层
#include "gal_content.h"

#define GAL_CONTENT_TITLE    "示例作品"
#define GAL_CONTENT_SUBTITLE "一段示例旅程"
#define GAL_CONTENT_LOGO_SUB "SAMPLE TITLE"
#define GAL_CONTENT_VERSION  "v0.1.0"

// 剧情标记:按作品需要增删
enum {
    GAL_FLAG_KIND = 0,   // 选择了「温柔一点」
    GAL_FLAG_CLEARED,    // 走过一次结局
};

// ---------------------------------------------------------------- 对白
const gal_line_t gal_content_lines[] = {
    { "", "雨停之后,车站只剩我们两个人。", GAL_UNLOCK_NONE },
    { "主角", "你真的要走吗?", GAL_UNLOCK_NONE },
    { "同行者", "……嗯。", GAL_UNLOCK_NONE },
    { "同行者", "但今天,可以慢一点。", GAL_FLAG_KIND },   // 条件台词:只在选了「温柔一点」后出现
    { "主角", "那就再等一班车。", GAL_UNLOCK_NONE },
};
const int gal_content_line_count =
    (int)(sizeof(gal_content_lines) / sizeof(gal_content_lines[0]));

// ---------------------------------------------------------------- 选项
static const gal_option_t choice0_options[] = {
    { "温柔一点", GAL_FLAG_KIND, GAL_UNLOCK_NONE },      // 选中后写入标记
    { "什么也不说", GAL_UNLOCK_NONE, GAL_UNLOCK_NONE },
};
const gal_choice_t gal_content_choices[] = {
    { "列车进站的声音从远处传来。", 2, choice0_options },
};
const int gal_content_choice_count =
    (int)(sizeof(gal_content_choices) / sizeof(gal_content_choices[0]));

// ---------------------------------------------------------------- 事件流(演出顺序)
const gal_event_t gal_content_events[] = {
    { GAL_EV_LINE, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 1, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 2, GAL_UNLOCK_NONE },
    { GAL_EV_CHOICE, 0, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 3, GAL_UNLOCK_NONE },
    { GAL_EV_LINE, 4, GAL_UNLOCK_NONE },
    { GAL_EV_TRANSITION, 0, GAL_UNLOCK_NONE },
    { GAL_EV_ENDING, 0, GAL_FLAG_CLEARED },              // 到达结局即写入通关标记
};
const int gal_content_event_count =
    (int)(sizeof(gal_content_events) / sizeof(gal_content_events[0]));

// ---------------------------------------------------------------- 章节过场
const gal_transition_t gal_content_transitions[] = {
    { "第 2 章", "CHAPTER 02", "再等一班车", "第二章 · 站台" },
};
const int gal_content_transition_count =
    (int)(sizeof(gal_content_transitions) / sizeof(gal_content_transitions[0]));

// ---------------------------------------------------------------- 结局
const gal_ending_t gal_content_endings[] = {
    { "NORMAL END", "再等一班车", "车来了。\n这一次,两个人都没有上车。" },
};
const int gal_content_ending_count =
    (int)(sizeof(gal_content_endings) / sizeof(gal_content_endings[0]));

// ---------------------------------------------------------------- CG
const gal_cg_t gal_content_cgs[] = {
    { "CG 01", "空月台", 1, false },
    { "CG 02", "再等一班车", 6, false },
};
const int gal_content_cg_count = (int)(sizeof(gal_content_cgs) / sizeof(gal_content_cgs[0]));

// ---------------------------------------------------------------- 章节 / 路线
const gal_chapter_t gal_content_chapters[] = {
    { "序章 · 空月台", "雨停之后的站台。", "完成度　1 / 2",
      GAL_UNLOCK_NONE, GAL_UNLOCK_NONE, false },
    { "隐藏线 · 夜车", "走过一次结局后解锁。", "完成度　0 / 2",
      GAL_UNLOCK_NONE, GAL_FLAG_CLEARED, false },
};
const int gal_content_chapter_count =
    (int)(sizeof(gal_content_chapters) / sizeof(gal_content_chapters[0]));

// ---------------------------------------------------------------- CG 回想
const gal_recall_t gal_content_recalls[] = {
    { "空月台", "序章", 0, 0 },      // 需要 CG 01 已解锁,从事件 0 重演
};
const int gal_content_recall_count =
    (int)(sizeof(gal_content_recalls) / sizeof(gal_content_recalls[0]));

// ---------------------------------------------------------------- 音乐
// notes 是音高表下标(C4 起的自然音阶,0=Do1=Re…,见 gal_music.c),0xFF = 休止。
static const uint8_t track0_notes[] = { 0, 4, 7, 4, GAL_NOTE_REST };
const gal_track_t gal_content_tracks[] = {
    { "空月台", "01:30", 0, track0_notes, (uint8_t)sizeof(track0_notes) },
};
const int gal_content_track_count =
    (int)(sizeof(gal_content_tracks) / sizeof(gal_content_tracks[0]));

// ---------------------------------------------------------------- 关于
const gal_about_row_t gal_content_about_rows[] = {
    { "版本", GAL_CONTENT_VERSION },
    { "作者", "作者名" },
    { "美术 / 音乐", "素材说明" },
    { "授权说明", "按作品填写" },
};
const int gal_content_about_row_count =
    (int)(sizeof(gal_content_about_rows) / sizeof(gal_content_about_rows[0]));

// ---------------------------------------------------------------- 菜单
const char *const gal_content_title_menu[] = {
    "开始新游戏", "继续游戏", "读取存档", "设置", "鉴赏室", "关于",
};
const int gal_content_title_menu_count =
    (int)(sizeof(gal_content_title_menu) / sizeof(gal_content_title_menu[0]));

const char *const gal_content_quick_menu[] = {
    "返回剧情", "保存进度", "读取进度", "历史文本", "设置", "返回标题",
};
const int gal_content_quick_menu_count =
    (int)(sizeof(gal_content_quick_menu) / sizeof(gal_content_quick_menu[0]));

// ---------------------------------------------------------------- 鉴赏室分类
const char *const gal_content_gallery_menu[] = {
    "CG 鉴赏", "CG 回想", "剧情鉴赏", "音乐鉴赏",
};
const int gal_content_gallery_menu_count =
    (int)(sizeof(gal_content_gallery_menu) / sizeof(gal_content_gallery_menu[0]));
```

另外在 `gal_content.h` 里把 `GAL_CONTENT_ABOUT` 换成本作简介，并按需要保留 / 增删
`GAL_FLAG_*` 枚举项。

## 4. 剧情锁（可选）

三步就够：

1. **定义标记**：在 `gal_content.h` 的 `GAL_FLAG_*` 里加一项。
2. **写入标记**：选项的 `flag_set`，或事件的 `flag_set`（过场、结局常用）。
3. **要求标记**：台词的 `require_flag`（不满足则这句**自动跳过**，用来做分支）、
   选项的 `require_flag`（不满足则置灰）、章节的 `require_flag`（不满足显示「锁定」）。

CG 与章节还可以用 `unlock_event`：到达该事件下标即解锁（例如 `6` = 事件流第 7 个事件）。
解锁记录与标记跨存档长期保存，「继续游戏」与鉴赏室都读它。

## 5. 填完之后三步

```bash
# 1) 内容变了要重新生成中文字体子集(字符集自动从 main/ 的字面量收集)
python3 tools/gal_font.py --font <源字体>.otf --out-dir assets/fonts

# 2) 门禁:仓库检查 + 宿主机测试 + 字体覆盖(缺字会直接失败)
CC=<cc 包装> ./tools/validate.sh --static      # macOS 需要 CC 包装,见完整手册
./tools/validate.sh --firmware                 # 需要已激活 ESP-IDF 5.5.3

# 3) 烧录到设备
idf.py -p <串口> flash monitor
```

## 6. 接入真实素材（进阶）

| 想要的效果 | 改哪里 |
| --- | --- |
| 真实背景 / CG 图片 | `gal_ui.c`：把 `bg`、CG 缩略图 tile、CG 全屏页的渐变换成 `lv_image`，资源放 `assets/`（LVGL 图片或 RGB565 原始数据） |
| 真实 BGM / 语音 | `gal_music.c`：把方波循环换成解码后的 PCM，仍走 `bsp_audio_write()`，用 `bsp_audio_set_format()` 设采样格式 |
| 从 PC 版批量搬剧本 | 先写一个转换脚本，把剧本文本转成 `gal_content.c` 的三张表（对白 / 事件流 / 选项）；标记与章节照第 4 节映射 |
| 横屏素材 | 用 BSP 的横屏接口，按 320×240 裁切（布局参数见表，见完整手册第 5 节） |

## 7. 交付前检查清单

- [ ] 四张核心表齐全：对白、事件流、选项、结局；每张表都有对应的 `count` 变量
- [ ] 事件流里每个下标都存在（`GAL_EV_LINE 3` 就必须有 `gal_content_lines[3]`）
- [ ] 章节过场至少一个（否则「章节 / 路线」与存档章节名没有内容）
- [ ] CG / 章节 / 回想 / 曲目的解锁事件落在真实存在的事件下标上
- [ ] 关于页信息与作品一致；菜单文字已本地化
- [ ] 重新生成字体后 `./tools/validate.sh --static` 通过（字体覆盖 0 缺字）
- [ ] 真机走一遍：启动页 → 对白 → 选项 → 过场 → 结局 → 回标题 → 继续游戏
