// main/voice_app.c —— 音效钥匙扣应用。
// 产品形态: 开机即进本应用, 无主菜单。三层视图状态机:
//   目录页(顶层)   -> UP/DOWN 选择目录, OK 进入列表, OK 长按弹设置
//   列表页         -> UP/DOWN 选择音效(不打断播放), OK 播放, OK 长按返回目录
//   设置菜单       -> 显示电量, UP/DOWN 调节音量, OK 短按/长按退出
//
// UI 采用"现代列表式"：顶栏标题 + 列表行(中文) + 底部操作提示。
// 中文用 LVGL label + 中文字库(voice_ttf)渲染; 底栏提示用小号字库(voice_hint)。
// 音频从 SPIFFS 数据分区读取 Opus 8kbps 裸包流, 在独立任务用 libopus 解码后
// 送 bsp_audio_write() 播放。遵循硬件指南: 阻塞的 codec I/O 不放按键回调/LVGL 任务。
// 裸包流格式见 tools/encode_opus.py: 每个包 = 2字节小端长度 + Opus 帧。
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
#include "esp_system.h"    // esp_get_free_heap_size
#include "opus.h"          // libopus 解码器(components/opus)
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
// Opus 单包最大可含 60ms 音频; @16kHz = 960 采样。用上界避免任意帧长溢出。
#define OPUS_FRAME_SAMPLES 960     // 最大采样数(60ms 帧 @16kHz)
#define OPUS_MAX_PACKET 1500       // 单包最大字节(8kbps 帧很小, 1.5KB 安全余量)

// ---- UI 布局(240x320, 屏幕真实高度 BSP_LCD_H=320) ----
#define UI_W 240
#define UI_H 320
#define UI_TOP_H 24          // 顶栏(标题+右侧电量, 加高)
#define UI_BOTTOM_H 0        // 底部提示区已删除, 保留0占位
#define UI_LIST_TOP UI_TOP_H // 列表起点=顶栏底(不加偏移, 撑满到屏底)
#define UI_LIST_H   (UI_H - UI_TOP_H)   // 列表区延伸到屏幕最底 (320-24=296)
#define UI_ROW_H    21       // 296 / 21 = 14行余2px, 几乎不留空白; 行高>=字高避免垂直滚
                             // 24 + 14*21 = 318, 距屏底 320 仅 2px(整区撑满, 无空白行)
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
static lv_obj_t     *s_hint_obj;   // 底栏提示条(抓屏用)

// 目录页
static lv_obj_t     *s_dir_scr;
static lv_obj_t     *s_dir_batt;                  // 顶栏右侧电量百分比
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
static lv_obj_t     *s_set_batt_row, *s_set_vol_row;   // 设置行背景(音量行高亮, 电量行只读)
static uint8_t       s_vol;

// 播放任务: 每次播放创建一个独立 job(自带停止标志)。
// 之前用共享全局 s_player_stop, 新任务启动时把它重置回 false, 旧任务看到 false
// 就不停, 导致"停止当前播放"失效(旧播放持续 + 新播放叠加)。改成每任务独立标志。
typedef struct {
    const voice_file_t *f;
    volatile bool stop;      // 本 job 的停止标志, 由 stop_playback() 置 true
} play_job_t;

static play_job_t    *s_job;      // 当前活跃播放 job(单播放, 指向最新一个)

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
// 播放任务: 读裸 Opus 包流 -> 逐包 opus_decode -> bsp_audio_write
// 裸流格式(见 tools/encode_opus.py): 每个 Opus 包 = 2字节小端长度 + 帧数据。
// ---------------------------------------------------------------------------
static void stop_playback(void);

static void player_task(void *arg) {
    play_job_t *job = (play_job_t *)arg;
    const voice_file_t *f = job->f;
    char fullpath[128];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", VOICE_FS_MOUNT, f->path);

    FILE *fp = NULL;
    OpusDecoder *dec = NULL;
    int dec_err = 0;

    fp = fopen(fullpath, "rb");
    if (!fp) { ESP_LOGE(TAG, "无法打开 %s", fullpath); goto done; }

    if (bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "audio format 失败");
        goto done;
    }

    // 创建 Opus 解码器(16kHz 单声道, 无状态跨包复用)
    dec = opus_decoder_create(AUDIO_SAMPLE_RATE, 1, &dec_err);
    if (!dec) {
        ESP_LOGE(TAG, "opus_decoder_create 失败: %d", dec_err);
        goto done;
    }

    uint8_t hdr[2];
    uint8_t pkt[OPUS_MAX_PACKET];
    int16_t pcm[OPUS_FRAME_SAMPLES];

    // 用本 job 自己的 stop 标志: 旧任务只被 stop_playback() 置位, 不受新任务影响。
    // 新任务启动时不会重置旧任务的标志, 因此"OK 停止当前播放"能真正生效。
    while (!job->stop) {
        // 读 2 字节包长度
        if (fread(hdr, 1, 2, fp) != 2) break;
        uint16_t plen = (uint16_t)(hdr[0] | (hdr[1] << 8));
        if (plen == 0 || plen > OPUS_MAX_PACKET) {
            ESP_LOGW(TAG, "非法包长 %u", plen);
            break;
        }
        if (fread(pkt, 1, plen, fp) != plen) break;
        if (job->stop) break;

        // 解码一帧。pcm 输出为 int16, 每帧 <= OPUS_FRAME_SAMPLES 采样
        int nsamp = opus_decode(dec, pkt, plen, pcm, OPUS_FRAME_SAMPLES, 0);
        if (nsamp < 0) {
            ESP_LOGW(TAG, "opus_decode 错误 %d", nsamp);
            break;
        }
        bsp_audio_write(pcm, (size_t)nsamp * 2u);   // int16 采样 -> 字节 = nsamp*2
    }

    ESP_LOGI(TAG, "播放结束: %s", f->name);

done:
    if (dec) opus_decoder_destroy(dec);
    if (fp) fclose(fp);
    // 仅当自己仍是最新 job 才清 s_job(已被新 job 顶替则不动), 并释放本 job。
    if (s_job == job) s_job = NULL;
    free(job);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 调试抓屏: 把当前屏幕 RGB565 底部区域存进 SPIFFS, 供 host 读取判读布局。
// 仅用于验证"列表最后一行/底部提示条"落位; 抓完一次即自删, 不影响功能。
// 文件格式: 自定义头 "VSNP" + w(u16 LE) + h(u16 LE) + 原始 RGB565(h 整行)。
// ---------------------------------------------------------------------------
#define SNAP_MAGIC  0x504E5356       // "VSNP"
#define SNAP_TOP_Y  0                // 从第 0 行开始(整屏高度内取数据)
static void snap_dump(lv_obj_t *scr) {
    if (!scr || !s_fs_mounted) { ESP_LOGW(TAG, "snap: 无目标屏或 FS 未挂载"); return; }
    lv_draw_buf_t *db = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    if (!db) { ESP_LOGW(TAG, "snap: lv_snapshot_take 失败(内存不足?)"); return; }
    uint32_t w = db->header.w;
    uint32_t h = db->header.h;
    const uint8_t *px = db->data;
    uint32_t stride = db->header.stride;
    if (stride == 0) stride = w * 2;              // RGB565 每行字节兜底
    // 只导出底部 96 行(最后几行 + 提示条), 减小写文件量
    uint32_t dy = h > 96 ? (h - 96) : 0;
    uint32_t rows = h > 96 ? 96 : h;
    char path[128];
    snprintf(path, sizeof(path), "%s/snap.rgb", VOICE_FS_MOUNT);
    FILE *fp = fopen(path, "wb");
    if (!fp) { ESP_LOGW(TAG, "snap: 无法写 %s", path); lv_draw_buf_destroy(db); return; }
    struct { uint32_t magic; uint16_t w, h, stride; uint16_t dy, rows; } __attribute__((packed)) hd;
    hd.magic = SNAP_MAGIC; hd.w = (uint16_t)w; hd.h = (uint16_t)h;
    hd.stride = (uint16_t)stride; hd.dy = (uint16_t)dy; hd.rows = (uint16_t)rows;
    fwrite(&hd, 1, sizeof(hd), fp);
    fwrite(px + (size_t)dy * stride, 1, (size_t)rows * stride, fp);
    fclose(fp);
    ESP_LOGI(TAG, "snap: 已存 %s (w=%u h=%u stride=%u dy=%u rows=%u)",
             path, (unsigned)w, (unsigned)h, (unsigned)stride, (unsigned)dy, (unsigned)rows);
    lv_draw_buf_destroy(db);
}

// ---------------------------------------------------------------------------
// 启动后延时抓一次屏(等待 dir 页绘制完成)。整屏 137KB 在无 PSRAM 上常超堆,
// 故优先抓底栏提示条(小, 必成功)以确认其"贴屏底 y + 高度"; 同时打印空闲堆与
// 提示条/最后一行位置, 供 host 判读列表底与提示条之间有无空隙。
static void snap_timer_cb(lv_timer_t *t) {
    if (!bsp_lvgl_lock(500)) { lv_timer_delete(t); return; }
    ESP_LOGI(TAG, "snap: free_heap=%lu", (unsigned long)esp_get_free_heap_size());
    lv_obj_t *scr = lv_screen_active();
    // 记录提示条绝对位置(abs y 由相对父(screen)偏移决定)
    int hy = s_hint_obj ? lv_obj_get_y(s_hint_obj) : -1;
    int hh_ = s_hint_obj ? lv_obj_get_height(s_hint_obj) : -1;
    ESP_LOGI(TAG, "snap: hint y=%d h=%d -> bottom=%d (屏高 UI_H=%d)",
             hy, hh_, s_hint_obj ? (hy + hh_) : -1, UI_H);
    // 抓整个屏幕(底部96行) —— 需 CONFIG_LV_MEM_SIZE_KILOBYTES 足够容纳全屏
    if (scr) snap_dump(scr);
    bsp_lvgl_unlock();
    lv_timer_delete(t);
}

static void play_file(const voice_file_t *f) {
    stop_playback();                 // 先让旧播放(若有)停止
    if (!f) return;
    if (!s_fs_mounted) { ESP_LOGE(TAG, "SPIFFS 未挂载"); return; }
    // 每次播放一个独立 job: 自带 stop 标志, 互不干扰。旧任务被置 stop 后自然退出并
    // 自清 s_job; 新任务用新 job 的标志, 不会重置旧任务, 从而"OK 停止当前+播当前"可靠。
    play_job_t *job = malloc(sizeof(*job));
    if (!job) { ESP_LOGE(TAG, "播放 job 分配失败"); return; }
    job->f = f;
    job->stop = false;
    s_job = job;
    // VAR_ARRAYS 用任务栈做 scratch, SILK 单帧解码栈需求大, 需 16KB。
    if (xTaskCreate(player_task, "voice_player", 16384, job, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败");
        if (s_job == job) s_job = NULL;
        free(job);
        return;
    }
}

static void stop_playback(void) {
    if (s_job) s_job->stop = true;   // 请求当前播放停止(任务退出后自清 s_job)
}

// ---------------------------------------------------------------------------
// UI: 顶栏 / 底栏 / 列表行(点阵文字)
// ---------------------------------------------------------------------------
// 配色(用 LVGL9 的宏, 编译期常量)
#define COL_BG      (lv_color_hex(0x17203A))
#define COL_PANEL   (lv_color_hex(0x1E2A47))
#define COL_TEXT    (lv_color_hex(0xEDF1FF))
#define COL_HILITE  (lv_color_hex(0x2E4C9B))   // 选中行高亮(蓝)
#define COL_TOP     (lv_color_hex(0x1B2444))   // 顶栏专属色(比选中蓝更深/独立, 不与选中行同色)
#define COL_MUTED   (lv_color_hex(0x9AA7C9))
#define COL_PLAY    (lv_color_hex(0x1F7A4D))   // 播放中(设置中)状态色

// 建整个屏: 背景 + 顶栏标题 + 底栏提示
static lv_obj_t *make_screen(const char *title, const char *hint) {
    (void)hint;   // 底栏提示条已删除, 参数保留仅为兼容调用处, 不再渲染
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
    lv_obj_set_style_bg_color(top, COL_TOP, 0);   // 顶栏用专属色, 不与选中行同色
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_t *tt = lv_label_create(top);
    lv_label_set_text(tt, title);
    lv_obj_set_style_text_font(tt, &voice_ttf, 0);
    lv_obj_set_style_text_color(tt, COL_TEXT, 0);
    lv_obj_set_style_pad_all(tt, 0, 0);
    // 顶栏标题确定性定位: 左上, 宽 = 顶栏宽, 高度 = 顶栏高(垂直居中由内容)
    lv_obj_set_pos(tt, 8, 0);
    // 宽 = 屏宽 - 左右边距(8+8) - 右侧电量区46px, 避免标题与电量重叠
    lv_obj_set_size(tt, UI_W - 16 - 46, UI_TOP_H);
    // 顶栏标题过长则水平循环滚到最右显示完整(放慢速度); CIRCULAR=纯水平, 不垂直滚
    lv_obj_set_style_anim_duration(tt, 2000, 0);
    lv_label_set_long_mode(tt, LV_LABEL_LONG_SCROLL_CIRCULAR);

    // 删除了底部提示条: 列表区延伸到屏幕最底, 不再渲染 hint 小字。

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
    // 行内文字(lv_label + 中文字库): 确定性 set_pos(左上), 垂直居中用行内 y
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &voice_ttf, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    lv_obj_set_pos(lbl, UI_ROW_INDENT, 1);            // 行内左上, 顶部留1px
    lv_obj_set_size(lbl, UI_ROW_W - UI_ROW_INDENT, UI_ROW_H);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
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
// 顶栏右侧电量百分比刷新(仅目录页/首页显示)
// ---------------------------------------------------------------------------
static void refresh_dir_batt(void) {
    if (!s_dir_batt) return;
    int soc = bsp_battery_soc();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", soc < 0 ? 0 : soc);
    lv_label_set_text(s_dir_batt, buf);
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
    // 顶栏右侧电量百分比(仅首页显示): 右对齐垂直居中, 右缘留8px
    s_dir_batt = lv_label_create(s_dir_scr);
    lv_label_set_text(s_dir_batt, "--%");
    lv_obj_set_style_text_font(s_dir_batt, &voice_ttf, 0);
    lv_obj_set_style_text_color(s_dir_batt, COL_TEXT, 0);
    lv_obj_set_style_pad_all(s_dir_batt, 0, 0);
    lv_obj_align(s_dir_batt, LV_ALIGN_TOP_RIGHT, -8, (UI_TOP_H - 18) / 2);
    refresh_dir_batt();
    s_dir_count = VOICE_DIR_TOTAL;
    if (s_dir_sel >= (int)s_dir_count) s_dir_sel = 0;
    s_dir_top = 0;
    s_dir_window = s_dir_count < MAX_ROWS ? s_dir_count : MAX_ROWS;
    for (unsigned i = 0; i < s_dir_window; i++) {
        s_dir_rows[i] = make_row(s_dir_scr, (int)i, VOICE_DIRS[i].name,
                                 ((int)i == s_dir_sel) ? ROW_SEL : ROW_UNSEL, &s_dir_txts[i]);
    }
    // 诊断: 打印实际列表几何(起点/行高/行数/最后行底 vs 屏高), 定位底部空白
    ESP_LOGI(TAG, "GEOM dir: UI_TOP=%d ROW_H=%d win=%u last_bot=%d UI_H=%d gap=%d",
             (int)UI_TOP_H, (int)UI_ROW_H, s_dir_window,
             (int)(UI_LIST_TOP + s_dir_window * UI_ROW_H), (int)UI_H,
             (int)(UI_H - (UI_LIST_TOP + s_dir_window * UI_ROW_H)));
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
            // SCROLL_CIRCULAR: 纯水平循环滚动(绝不垂直滚), 消除"单行文字上下滚动"。
            // 防抖: 仅当 long_mode 变化才 set_long_mode, 避免长按翻页重复重置滚动偏移。
            lv_label_long_mode_t want = sel ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
            if (lv_label_get_long_mode(s_dir_txts[i]) != want)
                lv_label_set_long_mode(s_dir_txts[i], want);
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
    // 诊断: 列表页实际几何(定位底部空白)
    ESP_LOGI(TAG, "GEOM list: UI_TOP=%d ROW_H=%d win=%u n=%d last_bot=%d UI_H=%d gap=%d",
             (int)UI_TOP_H, (int)UI_ROW_H, s_list_window, n,
             (int)(UI_LIST_TOP + s_list_window * UI_ROW_H), (int)UI_H,
             (int)(UI_H - (UI_LIST_TOP + s_list_window * UI_ROW_H)));
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
            // 只有光标选中行滚动(到最右停), 其余静止; 选中高亮。
            // SCROLL_CIRCULAR: 纯水平循环滚动(绝不垂直滚), 消除单行文字上下滚动。
            // 防抖: 仅当 long_mode 变化才 set_long_mode(内部会把滚动偏移重置为0)。
            bool sel = (idx == s_list_sel);
            lv_label_long_mode_t want = sel ? LV_LABEL_LONG_SCROLL_CIRCULAR : LV_LABEL_LONG_CLIP;
            if (lv_label_get_long_mode(s_list_txts[i]) != want)
                lv_label_set_long_mode(s_list_txts[i], want);
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
    s_set_scr = make_screen("设置", "上下调音量 短按/长按返回");

    // 电量行(只读)
    s_set_batt_row = lv_obj_create(s_set_scr);
    lv_obj_set_pos(s_set_batt_row, 4, 56);
    lv_obj_set_size(s_set_batt_row, 232, 24);
    lv_obj_remove_flag(s_set_batt_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_set_batt_row, 0, 0);
    lv_obj_set_style_border_width(s_set_batt_row, 0, 0);
    lv_obj_set_style_pad_all(s_set_batt_row, 0, 0);
    lv_obj_set_style_bg_color(s_set_batt_row, COL_PANEL, 0);   // 电量只读, 不高亮
    s_set_batt_img = lv_label_create(s_set_batt_row);
    lv_label_set_text(s_set_batt_img, "电量 --");
    lv_obj_set_style_text_font(s_set_batt_img, &voice_ttf, 0);
    lv_obj_set_style_text_color(s_set_batt_img, COL_TEXT, 0);
    lv_obj_align(s_set_batt_img, LV_ALIGN_LEFT_MID, UI_ROW_INDENT, 0);

    // 音量行(可调, UP/DOWN 直接调节)
    s_set_vol_row = lv_obj_create(s_set_scr);
    lv_obj_set_pos(s_set_vol_row, 4, 84);
    lv_obj_set_size(s_set_vol_row, 232, 24);
    lv_obj_remove_flag(s_set_vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_set_vol_row, 0, 0);
    lv_obj_set_style_border_width(s_set_vol_row, 0, 0);
    lv_obj_set_style_pad_all(s_set_vol_row, 0, 0);
    lv_obj_set_style_bg_color(s_set_vol_row, COL_HILITE, 0);   // 活动项(可调)高亮
    s_set_vol_img = lv_label_create(s_set_vol_row);
    lv_label_set_text(s_set_vol_img, "音量 --");
    lv_obj_set_style_text_font(s_set_vol_img, &voice_ttf, 0);
    lv_obj_set_style_text_color(s_set_vol_img, COL_TEXT, 0);
    lv_obj_align(s_set_vol_img, LV_ALIGN_LEFT_MID, UI_ROW_INDENT, 0);

    // 立即刷新为真实电量/音量(否则显示占位文本 "--")
    refresh_settings();
    lv_screen_load(s_set_scr);
}

// 刷新电量/音量显示(音量行始终高亮为可调项)
static void refresh_settings(void) {
    int soc = bsp_battery_soc();
    int mv  = bsp_battery_mv();
    char buf[64];
    snprintf(buf, sizeof(buf), "电量 %d%%  %dmV", soc < 0 ? 0 : soc, mv < 0 ? 0 : mv);
    lv_label_set_text(s_set_batt_img, buf);
    snprintf(buf, sizeof(buf), "音量 %d%%", (int)s_vol);
    lv_label_set_text(s_set_vol_img, buf);
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
            // 上下选择不打断播放; OK 短按停止当前播放并播当前选中项。
            if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG_HOLD)) {
                if (s_list_sel > 0) { s_list_sel--; refresh_list_selection(); }
            } else if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG_HOLD)) {
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
        // 设置页: UP/DOWN 直接调音量(HOLD 连续调); OK 短按/长按 退出返回目录。
        if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG_HOLD)) {
            if (s_vol < 100) s_vol = (uint8_t)(s_vol + 5);
            bsp_audio_set_volume(s_vol);
            refresh_settings();
        } else if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG_HOLD)) {
            if (s_vol >= 5) s_vol = (uint8_t)(s_vol - 5);
            bsp_audio_set_volume(s_vol);
            refresh_settings();
        } else if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
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

    // 调试: 启动后 1.2s 抓屏存 SPIFFS, 供 host 读回判读底部布局(一次即删)
    lv_timer_create(snap_timer_cb, 1200, NULL);

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
