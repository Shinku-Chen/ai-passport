// main/voice_app.c —— 音效钥匙扣应用。
// 产品形态: 开机即进本应用, 无主菜单。三层视图状态机:
//   目录页(顶层)   -> UP/DOWN 选择目录, OK 进入列表, OK 长按弹设置
//   列表页         -> UP/DOWN 选择音效, OK 播放, OK 长按返回目录
//   设置菜单       -> 显示电量, 调节音量, OK 长按退出
//
// UI 采用"现代列表式"：顶栏标题 + 列表行(中文) + 底部操作提示。
// 中文用 LVGL label + 中文字库(voice_ttf)渲染; 底栏提示用小号字库(voice_hint)。
// 音频从 SPIFFS 数据分区读取 IMA-ADPCM 4bit 压缩 PCM, 在独立任务解码后
// 送 bsp_audio_write() 播放。遵循硬件指南: 阻塞的 codec I/O 不放按键回调/LVGL 任务。
#include "voice_app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"   // bsp_lvgl_lock / bsp_lvgl_unlock
#include "voice_index.h"   // 编译期素材索引(VOICE_DIRS)
#include "fonts/voice_cjk.h" // 中文 UI 字库(黑体 TTF 子集, lv_label 渲染)
#include "fonts/voice_hint.h" // 底栏提示 12px 小号字库
#include "esp_spiffs.h"    // SPIFFS 挂载(VFS), 注册后走标准 POSIX fopen/opendir
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char *TAG = "voice_app";

// ---- 数据分区与驱动配置 ----
#define VOICE_FS_PARTITION  "voicefs"
#define VOICE_FS_MOUNT      "/voices"

// ---- 播放 ----
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHUNK_SAMPLES 512
#define AUDIO_CHUNK_BYTES  (AUDIO_CHUNK_SAMPLES / 2)

// ---- UI 布局(240x286) ----
#define UI_W 240
#define UI_H 286
#define UI_TOP_H 16          // 顶栏(正好 16px 标题高; 为列表区让出最大高度)
#define UI_BOTTOM_H 12       // 底部提示文字区(小字贴底; 无独立条)
#define UI_LIST_TOP (UI_TOP_H + 2)
#define UI_LIST_H   (UI_H - UI_TOP_H - UI_BOTTOM_H)
#define UI_ROW_H    16       // 列表行高(正好 16px 字高, 最多行)
#define UI_ROW_W    232      // 列表行宽
#define UI_ROW_X    4
#define UI_ROW_INDENT 10     // 行内文字左边距

// 每目录最多音频数(全量单目录最多 149, 预留 160)
#define MAX_FILES 160
// 每屏可见的最大行数
#define MAX_ROWS ((UI_LIST_H) / (UI_ROW_H))

// 视图类型
typedef enum { VIEW_DIR = 0, VIEW_LIST, VIEW_SETTINGS, VIEW_COUNT } view_t;

// ---- 全局状态(单例应用) ----
static view_t        s_view;
static bool          s_fs_mounted;

// 目录页
static lv_obj_t     *s_dir_scr;
static int           s_dir_sel;
static int           s_dir_top;                   // 可见窗口起点(滚动)
static lv_obj_t     *s_dir_rows[MAX_ROWS];       // 背景条(选中高亮)
static lv_obj_t     *s_dir_txts[MAX_ROWS];       // 行文字 image
static unsigned      s_dir_count;                 // 目录总数(实际)
static unsigned      s_dir_window;                // 可见窗口行数

// 列表页
static lv_obj_t     *s_list_scr;
static int           s_list_sel;
static int           s_list_dir;
static int           s_list_top;                  // 可见窗口起点(滚动)
static lv_obj_t     *s_list_rows[MAX_ROWS];
static lv_obj_t     *s_list_txts[MAX_ROWS];
static unsigned      s_list_count;                // 目录内音频总数(实际)
static unsigned      s_list_window;               // 可见窗口行数

// 设置页
static lv_obj_t     *s_set_scr;
static lv_obj_t     *s_set_batt_img, *s_set_vol_img;
static lv_obj_t     *s_set_batt_row, *s_set_vol_row;   // 设置行背景(选中高亮)
static int           s_set_sel;          // 0=电量(只读) 1=音量(可调)
static bool          s_set_vol_active;   // 音量调节态(OK选中音量后 UP/DOWN 调音量)
static uint8_t       s_vol;

// 播放任务
static TaskHandle_t  s_player_task;
static volatile bool s_player_stop;
static bool          s_playing;
static int           s_playing_index;   // 当前播放的列表项索引(用于"设置中/播放中"态高亮)

// ---- IMA-ADPCM 解码表(与 tools/encode_voice.py 编码端对称) ----
static const int16_t ADPCM_STEPS[89] = {
      7,   8,   9,  10,  11,  12,  13,  14,  16,  17,  19,  21,  23,  25,  28,  31,
     34,  37,  41,  45,  50,  55,  60,  66,  73,  80,  88,  97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
   3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
  15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t ADPCM_INDEX_TABLE[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };
#define ADPCM_STEP_MAX 88

// ---------------------------------------------------------------------------
// SPIFFS 挂载
// ---------------------------------------------------------------------------
static bool fs_mount(void) {
    if (s_fs_mounted) return true;
    esp_vfs_spiffs_conf_t cfg = {
        .base_path = VOICE_FS_MOUNT,
        .partition_label = VOICE_FS_PARTITION,
        .max_files = 8,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS 挂载失败: %s (分区 %s, 挂载点 %s)",
                 esp_err_to_name(err), VOICE_FS_PARTITION, VOICE_FS_MOUNT);
        return false;
    }
    s_fs_mounted = true;
    ESP_LOGI(TAG, "SPIFFS 挂在 %s (分区 %s)", VOICE_FS_MOUNT, VOICE_FS_PARTITION);
    return true;
}

// ---------------------------------------------------------------------------
// IMA-ADPCM 4bit 解码(跨块保持状态)
// ---------------------------------------------------------------------------
typedef struct { int predictor; int step_index; } adpcm_state_t;

static void adpcm_decode_block(const uint8_t *src, int nbytes,
                               int16_t *dst, adpcm_state_t *st) {
    int oi = 0;
    for (int bi = 0; bi < nbytes; bi++) {
        uint8_t b = src[bi];
        for (int nib = 0; nib < 2; nib++) {
            int code = (nib == 0) ? (b & 0x0F) : ((b >> 4) & 0x0F);
            int step = ADPCM_STEPS[st->step_index];
            int diffq = ((code & 0x07) * 2 + 1) * step >> 3;
            if (code & 0x08) diffq = -diffq;
            st->predictor += diffq;
            if (st->predictor < -32768) st->predictor = -32768;
            if (st->predictor >  32767) st->predictor =  32767;
            st->step_index += ADPCM_INDEX_TABLE[code];
            if (st->step_index < 0) st->step_index = 0;
            if (st->step_index > ADPCM_STEP_MAX) st->step_index = ADPCM_STEP_MAX;
            dst[oi++] = (int16_t)st->predictor;
        }
    }
}

// ---------------------------------------------------------------------------
// 播放任务: 读文件 -> 分块解码 -> bsp_audio_write
// ---------------------------------------------------------------------------
static void stop_playback(void);

static void player_task(void *arg) {
    const voice_file_t *f = (const voice_file_t *)arg;
    char fullpath[128];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", VOICE_FS_MOUNT, f->path);

    FILE *fp = fopen(fullpath, "rb");
    if (!fp) { ESP_LOGE(TAG, "无法打开 %s", fullpath); return; }

    if (bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "audio format 失败");
        fclose(fp);
        return;
    }

    uint8_t cbuf[AUDIO_CHUNK_BYTES];
    int16_t sbuf[AUDIO_CHUNK_SAMPLES];
    adpcm_state_t st = { .predictor = 0, .step_index = 0 };
    s_player_stop = false;

    while (!s_player_stop) {
        size_t rb = fread(cbuf, 1, sizeof(cbuf), fp);
        if (rb == 0) break;
        adpcm_decode_block(cbuf, (int)rb, sbuf, &st);
        if (s_player_stop) break;
        // rb 字节 ADPCM -> rb*2 个 int16 采样 -> rb*4 字节
        bsp_audio_write(sbuf, (size_t)rb * 4u);
    }

    fclose(fp);
    s_playing = false;
    s_playing_index = -1;
    ESP_LOGI(TAG, "播放结束: %s", f->name);
    vTaskDelete(NULL);
}

static void play_file(const voice_file_t *f) {
    stop_playback();
    if (!f) return;
    if (!s_fs_mounted) { ESP_LOGE(TAG, "SPIFFS 未挂载"); return; }
    if (xTaskCreate(player_task, "voice_player", 4096, (void *)f, 5, &s_player_task) != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败");
        return;
    }
    s_playing = true;
    s_playing_index = s_list_sel;   // 记录当前播放项, 供"设置中/播放中"态高亮
}

static void stop_playback(void) {
    s_player_stop = true;
    if (s_player_task) s_player_task = NULL;
}

// ---------------------------------------------------------------------------
// UI: 顶栏 / 底栏 / 列表行(点阵文字)
// ---------------------------------------------------------------------------
// 配色(用 LVGL9 的宏, 编译期常量)
#define COL_BG      (lv_color_hex(0x17203A))
#define COL_PANEL   (lv_color_hex(0x1E2A47))
#define COL_TEXT    (lv_color_hex(0xEDF1FF))
#define COL_HILITE  (lv_color_hex(0x2E4C9B))
#define COL_MUTED   (lv_color_hex(0x9AA7C9))
#define COL_PLAY    (lv_color_hex(0x1F7A4D))   // 播放中(设置中)状态色

// 建整个屏: 背景 + 顶栏标题 + 底栏提示
static lv_obj_t *make_screen(const char *title, const char *hint) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 顶栏: 标题(lv_label + 中文字库)
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_size(top, UI_W, UI_TOP_H);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, COL_HILITE, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_t *tt = lv_label_create(top);
    lv_label_set_text(tt, title);
    lv_obj_set_style_text_font(tt, &voice_ttf, 0);
    lv_obj_set_style_text_color(tt, COL_TEXT, 0);
    lv_obj_set_width(tt, UI_W - 16);
    lv_obj_set_style_pad_all(tt, 0, 0);
    // 顶栏标题过长则滚到最右显示完整(放慢速度)
    lv_obj_set_style_anim_duration(tt, 2000, 0);
    lv_label_set_long_mode(tt, LV_LABEL_LONG_SCROLL);
    lv_obj_align(tt, LV_ALIGN_LEFT_MID, 8, 0);

    // 底栏: 无独立条, 提示文字直接用屏幕背景贴底部一行小字(不占可见条)
    if (hint) {
        lv_obj_t *hh = lv_label_create(scr);
        lv_label_set_text(hh, hint);
        lv_obj_set_style_text_font(hh, &voice_hint, 0);   // 12px 小号, 保持完整显示
        lv_obj_set_style_text_color(hh, COL_MUTED, 0);
        lv_obj_set_style_pad_all(hh, 0, 0);
        lv_obj_align(hh, LV_ALIGN_BOTTOM_LEFT, 8, -1);    // 贴屏幕底部
    }

    return scr;
}

// 列表行三态: 未选中 / 已选中(光标) / 设置中(播放中)
typedef enum { ROW_UNSEL = 0, ROW_SEL, ROW_PLAYING } row_state_t;

// 某位置创建一个列表行(背景条 + 点阵文字)。返回背景条, 文字 image 存 *txt.
static lv_obj_t *make_row(lv_obj_t *scr, int idx, const char *text, row_state_t st, lv_obj_t **txt) {
    int y = UI_LIST_TOP + idx * UI_ROW_H;
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_pos(row, UI_ROW_X, y);
    lv_obj_set_size(row, UI_ROW_W, UI_ROW_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_color(row, st == ROW_SEL ? COL_HILITE : COL_PANEL, 0);
    // 行左侧一个彩色小指示条
    lv_obj_t *ind = lv_obj_create(row);
    lv_obj_set_pos(ind, 0, 0);
    lv_obj_set_size(ind, 3, UI_ROW_H);
    lv_obj_set_style_bg_color(ind, st == ROW_UNSEL ? COL_MUTED : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(ind, 0, 0);
    // 行内文字(lv_label + 中文字库)
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &voice_ttf, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, UI_ROW_INDENT, 0);
    lv_obj_set_width(lbl, UI_ROW_W - UI_ROW_INDENT);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    // 默认静止(只显示左段); 光标选中行在刷新时用 SCROLL 滚到最右显示完整
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    // 放慢横向滚动速度(值越低越慢)
    lv_obj_set_style_anim_duration(lbl, 2000, 0);
    *txt = lbl;
    return row;
}

// 更新某行状态(背景条颜色): 未选中(深色) / 已选中(高亮); 播放中不再用绿色, 与其他未选中相同
static void set_row_state(lv_obj_t *row, row_state_t st) {
    if (row) lv_obj_set_style_bg_color(row, st == ROW_SEL ? COL_HILITE : COL_PANEL, 0);
}

// ---------------------------------------------------------------------------
// 目录页
// ---------------------------------------------------------------------------
// 目录页: 建 `window` 个可见行(滚动窗口), 每次刷新用 lv_label_set_text 更新文本与选中
static void dir_build(void) {
    // 进入新页前删除旧屏, 避免 LVGL 对象/RAM 跨屏累积
    if (s_list_scr) { lv_obj_delete(s_list_scr); s_list_scr = NULL; }
    if (s_set_scr) { lv_obj_delete(s_set_scr); s_set_scr = NULL; }
    s_dir_scr = make_screen("音效钥匙扣", "上下选择 OK进入 长按设置");
    s_dir_count = VOICE_DIR_TOTAL;
    if (s_dir_sel >= (int)s_dir_count) s_dir_sel = 0;
    s_dir_top = 0;
    s_dir_window = s_dir_count < MAX_ROWS ? s_dir_count : MAX_ROWS;
    for (unsigned i = 0; i < s_dir_window; i++) {
        s_dir_rows[i] = make_row(s_dir_scr, (int)i, VOICE_DIRS[i].name,
                                 ((int)i == s_dir_sel) ? ROW_SEL : ROW_UNSEL, &s_dir_txts[i]);
    }
    lv_screen_load(s_dir_scr);
}

// 重绘目录页可见窗口(文本+选中)。选中项滚出窗口时移动窗口起点。
static void refresh_dir_selection(void) {
    if (s_dir_sel < s_dir_top) s_dir_top = s_dir_sel;
    if (s_dir_sel >= (int)(s_dir_top + s_dir_window)) s_dir_top = s_dir_sel - (int)s_dir_window + 1;
    if (s_dir_top < 0) s_dir_top = 0;
    for (unsigned i = 0; i < s_dir_window; i++) {
        int idx = s_dir_top + (int)i;
        if (idx >= 0 && idx < (int)s_dir_count) {
            lv_label_set_text(s_dir_txts[i], VOICE_DIRS[idx].name);
            bool sel = (idx == s_dir_sel);
            lv_label_set_long_mode(s_dir_txts[i], sel ? LV_LABEL_LONG_SCROLL : LV_LABEL_LONG_CLIP);
            set_row_state(s_dir_rows[i], sel ? ROW_SEL : ROW_UNSEL);
        }
    }
}

// ---------------------------------------------------------------------------
// 列表页: 同上, 滚动窗口浏览全部音频
// ---------------------------------------------------------------------------
static void list_build(int diridx) {
    s_list_dir = diridx;
    // 进入新页前删除旧屏, 避免 LVGL 对象/RAM 跨屏累积
    if (s_dir_scr) { lv_obj_delete(s_dir_scr); s_dir_scr = NULL; }
    if (s_set_scr) { lv_obj_delete(s_set_scr); s_set_scr = NULL; }
    const voice_dir_t *d = &VOICE_DIRS[diridx];
    int n = (int)d->count; if (n > MAX_FILES) n = MAX_FILES;
    if (s_list_sel >= n) s_list_sel = 0;

    s_list_scr = make_screen(d->name, "OK播放 长按返回");
    s_list_count = (unsigned)n;
    s_list_top = 0;
    s_list_window = s_list_count < MAX_ROWS ? s_list_count : MAX_ROWS;
    for (unsigned i = 0; i < s_list_window; i++) {
        s_list_rows[i] = make_row(s_list_scr, (int)i, d->files[i].name,
                                  ((int)i == s_list_sel) ? ROW_SEL : ROW_UNSEL, &s_list_txts[i]);
    }
    lv_screen_load(s_list_scr);
}

static void refresh_list_selection(void) {
    if (s_list_sel < s_list_top) s_list_top = s_list_sel;
    if (s_list_sel >= (int)(s_list_top + s_list_window)) s_list_top = s_list_sel - (int)s_list_window + 1;
    if (s_list_top < 0) s_list_top = 0;
    const voice_dir_t *d = &VOICE_DIRS[s_list_dir];
    for (unsigned i = 0; i < s_list_window; i++) {
        int idx = s_list_top + (int)i;
        if (idx >= 0 && idx < (int)s_list_count && idx < (int)d->count) {
            lv_label_set_text(s_list_txts[i], d->files[idx].name);
            // 只有光标选中行滚动(到最右停), 其余静止; 选中高亮
            bool sel = (idx == s_list_sel);
            lv_label_set_long_mode(s_list_txts[i], sel ? LV_LABEL_LONG_SCROLL : LV_LABEL_LONG_CLIP);
            set_row_state(s_list_rows[i], sel ? ROW_SEL : ROW_UNSEL);
        }
    }
}

// ---------------------------------------------------------------------------
// 设置页
// ---------------------------------------------------------------------------
static void refresh_settings(void);   // 前向声明(settings_build 会调用)

static void settings_build(void) {
    // 进入新页前删除旧屏, 避免 LVGL 对象/RAM 跨屏累积
    if (s_dir_scr) { lv_obj_delete(s_dir_scr); s_dir_scr = NULL; }
    if (s_list_scr) { lv_obj_delete(s_list_scr); s_list_scr = NULL; }
    s_set_scr = make_screen("设置", "上下选中 OK调音量 长按返回");
    s_set_sel = 1;              // 默认选中音量
    s_set_vol_active = false;

    // 电量行(只读)
    s_set_batt_row = lv_obj_create(s_set_scr);
    lv_obj_set_pos(s_set_batt_row, 4, 56);
    lv_obj_set_size(s_set_batt_row, 232, 24);
    lv_obj_remove_flag(s_set_batt_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_set_batt_row, 0, 0);
    lv_obj_set_style_border_width(s_set_batt_row, 0, 0);
    lv_obj_set_style_pad_all(s_set_batt_row, 0, 0);
    lv_obj_set_style_bg_color(s_set_batt_row, (s_set_sel == 0) ? COL_HILITE : COL_PANEL, 0);
    s_set_batt_img = lv_label_create(s_set_batt_row);
    lv_label_set_text(s_set_batt_img, "电量 --");
    lv_obj_set_style_text_font(s_set_batt_img, &voice_ttf, 0);
    lv_obj_set_style_text_color(s_set_batt_img, COL_TEXT, 0);
    lv_obj_align(s_set_batt_img, LV_ALIGN_LEFT_MID, UI_ROW_INDENT, 0);

    // 音量行(可调)
    s_set_vol_row = lv_obj_create(s_set_scr);
    lv_obj_set_pos(s_set_vol_row, 4, 84);
    lv_obj_set_size(s_set_vol_row, 232, 24);
    lv_obj_remove_flag(s_set_vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_set_vol_row, 0, 0);
    lv_obj_set_style_border_width(s_set_vol_row, 0, 0);
    lv_obj_set_style_pad_all(s_set_vol_row, 0, 0);
    lv_obj_set_style_bg_color(s_set_vol_row, (s_set_sel == 1) ? COL_HILITE : COL_PANEL, 0);
    s_set_vol_img = lv_label_create(s_set_vol_row);
    lv_label_set_text(s_set_vol_img, "音量 --");
    lv_obj_set_style_text_font(s_set_vol_img, &voice_ttf, 0);
    lv_obj_set_style_text_color(s_set_vol_img, COL_TEXT, 0);
    lv_obj_align(s_set_vol_img, LV_ALIGN_LEFT_MID, UI_ROW_INDENT, 0);

    // 立即刷新为真实电量/音量(否则显示占位文本 "--")
    refresh_settings();
    lv_screen_load(s_set_scr);
}

// 设置项选中态高亮
static void refresh_settings_selection(void) {
    lv_obj_set_style_bg_color(s_set_batt_row, (s_set_sel == 0) ? COL_HILITE : COL_PANEL, 0);
    lv_obj_set_style_bg_color(s_set_vol_row, (s_set_sel == 1) ? COL_HILITE : COL_PANEL, 0);
}

static void refresh_settings(void) {
    int soc = bsp_battery_soc();
    int mv  = bsp_battery_mv();
    char buf[64];
    snprintf(buf, sizeof(buf), "电量 %d%%  %dmV", soc < 0 ? 0 : soc, mv < 0 ? 0 : mv);
    lv_label_set_text(s_set_batt_img, buf);
    snprintf(buf, sizeof(buf), "音量 %d%%", (int)s_vol);
    lv_label_set_text(s_set_vol_img, buf);
    refresh_settings_selection();
}

// ---------------------------------------------------------------------------
// 按键处理
// ---------------------------------------------------------------------------
void voice_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;

    switch (s_view) {
    case VIEW_DIR:
        // 短按逐行 + 长按连续滚动; 到顶/底钳位, 不回绕。
        if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG_HOLD)) {
            if (s_dir_sel > 0) { s_dir_sel--; refresh_dir_selection(); }
        } else if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG_HOLD)) {
            if (s_dir_sel < (int)VOICE_DIR_TOTAL - 1) { s_dir_sel++; refresh_dir_selection(); }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            s_list_sel = 0;
            list_build(s_dir_sel);
            s_view = VIEW_LIST;
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            settings_build();
            s_view = VIEW_SETTINGS;
        }
        break;

    case VIEW_LIST: {
        const voice_dir_t *d = &VOICE_DIRS[s_list_dir];
        int n = (int)d->count; if (n > MAX_FILES) n = MAX_FILES;
        if (n > 0) {
            // 短按逐行 + 长按(HOLD)连续滚动。到顶/底钳位, 不回绕。
            // 播放中按上/下/返回会打断播放; OK 短按重新开始当前选中项。
            if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG_HOLD)) {
                if (s_playing) { stop_playback(); }   // 打断正在播的
                if (s_list_sel > 0) { s_list_sel--; refresh_list_selection(); }
            } else if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG_HOLD)) {
                if (s_playing) { stop_playback(); }   // 打断正在播的
                if (s_list_sel < n - 1) { s_list_sel++; refresh_list_selection(); }
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                play_file(&d->files[s_list_sel]);     // 内部先停旧播, 重新开始
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
                stop_playback();
                dir_build();
                s_view = VIEW_DIR;
            }
        }
        break;
    }

    case VIEW_SETTINGS:
        // 上下选中(电量/音量); OK 选中音量后上下调节; OK 长按退出。
        if (s_set_vol_active) {
            // 音量调节态: UP/DOWN 调音量
            if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
                if (s_vol < 100) s_vol = (uint8_t)(s_vol + 5);
                bsp_audio_set_volume(s_vol);
                refresh_settings();
            } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
                if (s_vol >= 5) s_vol = (uint8_t)(s_vol - 5);
                bsp_audio_set_volume(s_vol);
                refresh_settings();
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                s_set_vol_active = false;      // 退出调节态, 回选中态
                refresh_settings_selection();
            } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
                dir_build();
                s_view = VIEW_DIR;
            }
            break;
        }
        // 选中态: 上下切换 电量/音量
        if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
            if (s_set_sel > 0) { s_set_sel--; refresh_settings_selection(); }
        } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
            if (s_set_sel < 1) { s_set_sel++; refresh_settings_selection(); }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            if (s_set_sel == 1) s_set_vol_active = true;   // 选中的是音量 -> 进入调节
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            dir_build();
            s_view = VIEW_DIR;
        }
        break;

    default:
        break;
    }

    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------------------
// 应用生命周期
// ---------------------------------------------------------------------------
void voice_app_start(void) {
    if (!fs_mount()) ESP_LOGE(TAG, "SPIFFS 挂载失败, 音效应用无法工作");

    s_dir_sel = 0; s_list_sel = 0; s_list_dir = 0;
    s_vol = 80;
    s_view = VIEW_DIR;

    bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1);
    bsp_audio_set_volume(s_vol);   // 默认音量(否则 codec 可能无声)

    if (bsp_lvgl_lock(1000)) { dir_build(); bsp_lvgl_unlock(); }
    ESP_LOGI(TAG, "音效钥匙扣启动, 目录数=%d", VOICE_DIR_TOTAL);
}

void voice_app_stop(void) {
    stop_playback();
    if (bsp_lvgl_lock(1000)) {
        if (s_dir_scr) { lv_obj_delete(s_dir_scr); s_dir_scr = NULL; }
        if (s_list_scr) { lv_obj_delete(s_list_scr); s_list_scr = NULL; }
        if (s_set_scr) { lv_obj_delete(s_set_scr); s_set_scr = NULL; }
        bsp_lvgl_unlock();
    }
}
