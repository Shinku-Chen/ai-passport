/*
 * Every Chinese string the reader UI can display.
 *
 * tools/gal/gen_font.py scans this header for non-ASCII codepoints and unions
 * them with the chapter scripts, so any string added here is automatically
 * covered by the generated font subset. Adding a Chinese literal anywhere else
 * in the firmware would silently render as placeholder boxes instead.
 *
 * Unless noted, these are the original game's own terminology, kept so a reader
 * of the upstream release recognises the controls.
 */
#ifndef GAL_STRINGS_H
#define GAL_STRINGS_H

#define GAL_STR_APP_NAME       "飞鸟会长不肯认输"

/* Title screen */
#define GAL_STR_START          "开始游戏"
#define GAL_STR_CONTINUE       "继续阅读"
#define GAL_STR_CHAPTERS       "选择章节"
#define GAL_STR_SETTINGS       "设置"
#define GAL_STR_QUIT           "退出"

/* Reader */
#define GAL_STR_READER_HINT    "确定键推进"
#define GAL_STR_FAST_FORWARD   "快进"
#define GAL_STR_AUTO_ON        "自动阅读开"
#define GAL_STR_AUTO_OFF       "自动阅读关"
#define GAL_STR_HIDE_TEXT      "隐藏文字"
#define GAL_STR_SHOW_TEXT      "显示文字"

/* Title screen control legend */
#define GAL_STR_HELP_ADVANCE   "上键推进  下键滚动"
#define GAL_STR_HELP_HOLD      "长按上键快进"
#define GAL_STR_HELP_MENU      "确定键呼出菜单"

/* Reader menu panel */
#define GAL_STR_MENU           "菜单"
#define GAL_STR_SAVE           "保存进度"
#define GAL_STR_LOAD           "读取进度"
#define GAL_STR_SKIP_CHAPTER   "跳过本章"
#define GAL_STR_BACK_TO_TITLE  "返回标题"
#define GAL_STR_RESUME         "继续阅读"
#define GAL_STR_BACK           "返回"

/* Save slots */
#define GAL_STR_SLOT           "存档 "
#define GAL_STR_EMPTY_SLOT     "空存档"
#define GAL_STR_NO_SAVES       "没有存档"
#define GAL_STR_SAVED          "已保存"
#define GAL_STR_LOADED         "已读取"
#define GAL_STR_DELETED        "已删除"
#define GAL_STR_SAVE_HINT      "长按确定键删除"
#define GAL_STR_SAVE_FAILED    "保存失败"

/* Chapter list */
#define GAL_STR_CHAPTER_PREFIX "第 "
#define GAL_STR_CHAPTER_SUFFIX " 章"
#define GAL_STR_FINALE_BADGE   "结局"

/* Settings */
#define GAL_STR_TEXT_SPEED     "文字速度"
#define GAL_STR_TEXT_SIZE      "文字大小"
#define GAL_STR_RESET_DEFAULTS "恢复默认"
#define GAL_STR_PREVIEW        "文字速度与大小示例"
#define GAL_STR_ON             "开"
#define GAL_STR_OFF            "关"
#define GAL_STR_SLOW           "慢"
#define GAL_STR_FAST           "快"
#define GAL_STR_INSTANT        "瞬间"
#define GAL_STR_SMALL          "小"
#define GAL_STR_MEDIUM         "中"
#define GAL_STR_LARGE          "大"

/* Ending screen */
#define GAL_STR_ENDING         "达成结局"
#define GAL_STR_ENDING_HINT    "确定键返回标题"

/* Startup failures */
#define GAL_STR_NO_ASSETS      "未找到素材"
#define GAL_STR_NO_ASSETS_HINT "请刷入素材分区"

#endif /* GAL_STRINGS_H */
