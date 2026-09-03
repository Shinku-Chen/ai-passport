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
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"    // esp_get_free_heap_size
#include "esp_sleep.h"     // deep sleep + GPIO 唤醒
#include "esp_timer.h"     // 空闲计时器
#include "driver/gpio.h"   // GPIO0 唤醒配置
#include "driver/usb_serial_jtag.h"   // 控制台(UART0 因 GPIO21 冲突禁用, 走 USB-Serial/JTAG)
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

// 目录页
static lv_obj_t     *s_dir_scr;
static lv_obj_t     *s_dir_batt;                  // 顶栏右侧电量百分比
static lv_timer_t   *s_batt_timer;                // 周期性刷新电量(CW2017 SOC 会随充电/耗电变化)
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

static play_job_t    *s_job;           // 当前活跃播放 job(单播放, 指向最新一个)
static SemaphoreHandle_t s_job_sem;    // 常驻播放 worker 的唤醒信号(有新 job 时 give)
static TaskHandle_t      s_player;     // 常驻播放任务句柄(xTaskCreateStatic 创建, 静态栈)
#define PLAYER_STACK_BYTES 16384       // Opus SILK 单帧解码栈需求大, 用静态栈避免每次播放重复 16KB 堆分配
static StackType_t       s_player_stack[PLAYER_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t      s_player_tcb;

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

// 常驻播放任务: 启动后一直等待播放请求。每个 job 自带 stop 标志, 播放循环逐帧检查;
// 有新 job(OK 播放)时 play_file 先把当前 job 置 stop, worker 循环退出后再播新 job,
// 从而"OK 停止当前+播当前"可靠。用静态栈, 避免每次播放都从堆上申请 16KB。
static void player_worker(void *arg) {
    (void)arg;
    while (1) {
        xSemaphoreTake(s_job_sem, portMAX_DELAY);   // 等一个播放请求
        play_job_t *job = s_job;
        if (!job) continue;

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

        ESP_LOGI(TAG, "播放: %s", f->name);
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
    }
}

// 请求播放一个文件: 先停当前(若有), 再让常驻 worker 播新的。worker 用静态栈, 不每次
// 从堆上申请 16KB —— 堆不足/碎片化曾导致"只停不播"(xTaskCreate 16KB 栈失败)。
static void play_file(const voice_file_t *f) {
    if (s_job) s_job->stop = true;              // 停掉正在播的
    if (!f) return;
    if (!s_fs_mounted) { ESP_LOGE(TAG, "SPIFFS 未挂载"); return; }
    if (!s_job_sem || !s_player) { ESP_LOGE(TAG, "播放器未就绪"); return; }
    play_job_t *job = malloc(sizeof(*job));
    if (!job) { ESP_LOGE(TAG, "播放 job 分配失败"); return; }
    job->f = f;
    job->stop = false;
    s_job = job;
    if (xSemaphoreGive(s_job_sem) != pdTRUE) {  // 唤醒常驻 worker 播新 job
        ESP_LOGW(TAG, "唤醒播放 worker 失败");
    }
}

static void stop_playback(void) {
    if (s_job) s_job->stop = true;   // 请求当前播放停止(worker 循环退出后自清 s_job)
}

// ---------------------------------------------------------------------------
// 空闲休眠: 5 分钟无按键操作 → 深度休眠; 按键(GPIO0)经分压拉到 0.3~0.6V,
// 远低于 I/O 低电平阈值(约 0.825V) → 低电平唤醒。深睡唤醒=重启, App 回到目录页。
// ---------------------------------------------------------------------------
#define IDLE_SLEEP_US  (5u * 60u * 1000000u)

static esp_timer_handle_t s_sleep_timer;

static void do_deep_sleep(void) {
    ESP_LOGI(TAG, "5 分钟无操作, 进入深度休眠(按键唤醒)");

    // —— 省电前序: 深睡时 GPIO 保持原输出状态, 若背光脚(GPIO21)仍为 HIGH 会一直耗电。
    // 先把背光归零/停掉, 别让 LCD 背光与屏在深睡期间继续吃电。
    bsp_display_backlight(0);

    // GPIO0 已有外部 10k 上拉(空闲 3.3V), 无需软件上拉; 配置为纯输入, 低电平唤醒。
    // 显式关掉内部上拉(避免内部上拉叠加在外部 10kΩ 上造成额外泄漏)。
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    // ⚠ 接口第一参是【引脚位掩码】不是引脚号: 传 GPIO_NUM_0(=0) 会被当成空掩码
    // (esp_deep_sleep_enable_gpio_wakeup 对 mask==0 直接返回 INVALID_ARG), 结果深睡
    // 后没有任何唤醒源, 永远无法用按键唤醒。必须用 1ULL<<GPIO_NUM_0。
    esp_err_t wak = esp_deep_sleep_enable_gpio_wakeup(1ULL << GPIO_NUM_0, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (wak != ESP_OK) {
        ESP_LOGE(TAG, "GPIO0 低电平唤醒配置失败: %s", esp_err_to_name(wak));
    }
    esp_deep_sleep_start();   // 不返回; 唤醒=重启
}

static void sleep_timer_cb(void *arg) {
    do_deep_sleep();
}

// 任意按键事件重排 5 分钟空闲计时; 计时到即休眠。
static void reset_idle_sleep(void) {
    esp_timer_stop(s_sleep_timer);
    esp_timer_start_once(s_sleep_timer, IDLE_SLEEP_US);
}

// ---------------------------------------------------------------------------
// FAP_SCREENSHOT_V1 串口截图协议(社区发布用)
// publisher 助手经 USB-Serial/JTAG(控制台, 115200, USB 虚拟串口)发一行
// "FAP_SCREENSHOT_V1\n";本固件把当前 LVGL 屏幕按 RGB565LE 行序回传。社区发布
// 工具用这份真实画面做封面, 并校验 .fap-capture.json 收据;不实现该协议则无法
// 用官方 publisher 发布/更新(见其 references/serial-screenshot.md)。
// 只读: 不改设置、不换屏、不暴露任何凭证。
// ---------------------------------------------------------------------------
#define FAP_CMD_STR "FAP_SCREENSHOT_V1"

// 确保底层 USB-Serial/JTAG 驱动已安装。控制台 VFS 走 LL 函数, 不自装此驱动;
// 不装则 read_bytes/write_bytes 会解引用 NULL 而崩溃。若已装(REPL 场景)返回错, 忽略即可。
static void fap_driver_init(void) {
    static bool inited = false;
    if (inited) return;
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 4096;
    cfg.rx_buffer_size = 1024;
    usb_serial_jtag_driver_install(&cfg);
    inited = true;
}

// 抓当前屏并回传协议头 + RGB565LE 载荷。截图缓冲由 LVGL 内存池分配。
// 大载荷分块写(每块 2KB, 各自带超时), 规避单次 write_bytes 被 TX 环形缓冲 +
// 超时限制导致只发了一部分的问题。
static void fap_send_screenshot(void) {
    if (!bsp_lvgl_lock(1500)) return;
    lv_draw_buf_t *db = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    bsp_lvgl_unlock();
    if (!db) { ESP_LOGW(TAG, "FAP: lv_snapshot_take 失败"); return; }

    uint32_t w = db->header.w;
    uint32_t h = db->header.h;
    uint32_t stride = db->header.stride ? db->header.stride : (w * 2);
    uint32_t byte_len = stride * h;   // RGB565LE 行序, 规格要求 width*height*2
    char hdr[96];
    int hlen = snprintf(hdr, sizeof(hdr), FAP_CMD_STR " %u %u RGB565LE %u\n",
                        (unsigned)w, (unsigned)h, (unsigned)byte_len);
    if (usb_serial_jtag_write_bytes(hdr, (size_t)hlen, pdMS_TO_TICKS(2000)) != (size_t)hlen) {
        lv_draw_buf_destroy(db);
        ESP_LOGW(TAG, "FAP: 写协议头失败");
        return;
    }
    const uint8_t *p = (const uint8_t *)db->data;
    uint32_t rem = byte_len;
    while (rem > 0) {
        uint32_t chunk = rem > 2048 ? 2048 : rem;
        int n = (int)usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(5000));
        if (n <= 0) break;
        p += n;
        rem -= (uint32_t)n;
    }
    bool ok = (rem == 0);
    lv_draw_buf_destroy(db);
    ESP_LOGI(TAG, "FAP: 已回传 %ux%u %u 字节%s", (unsigned)w, (unsigned)h, (unsigned)byte_len,
             ok ? "" : "(未发完)");
}

// 控制台串口逐字节累积一行, 匹配到 "FAP_SCREENSHOT_V1" 即抓屏回传。
static void fap_screenshot_task(void *arg) {
    (void)arg;
    fap_driver_init();
    uint8_t line[64];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        int got = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50));
        if (got != 1) { n = 0; continue; }   // 超时/无数据: 清残句, 避免误触发
        if (c == '\n' || c == '\r') {
            line[n] = '\0';
            if (strcmp((const char *)line, FAP_CMD_STR) == 0) fap_send_screenshot();
            n = 0;
            continue;
        }
        if (n < sizeof(line) - 1) line[n++] = c;
    }
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

// 周期性刷新电量: 顶栏%只在开机读一次, 会随电池消耗/充电而失真; 这里每 30s
// 重读一次 CW2017 的 SOC, 让顶栏与设置行的电量跟随真实电量。整机 5 分钟无操作即
// 深睡, 会话很短, 30s 足以及时更新且不额外占用。
static void refresh_settings(void);   // 前向声明(refresh_batt_periodic 会用到)
static void refresh_batt_periodic(lv_timer_t *t) {
    (void)t;
    if (s_view == VIEW_DIR) refresh_dir_batt();
    if (s_view == VIEW_SETTINGS && s_set_batt_img) refresh_settings();
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
    reset_idle_sleep();   // 任意按键事件都重置 5 分钟空闲休眠计时

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

    // 周期性刷新电量(顶栏 + 设置行): 开机只读一次的 SOC 会随电池状态失真。
    s_batt_timer = lv_timer_create(refresh_batt_periodic, 30000, NULL);

    // 社区发布/截图协议: 在控制台(USB-Serial/JTAG)监听 FAP_SCREENSHOT_V1 并回传屏幕。
    // ⚠ 栈要够大: 抓屏用 lv_snapshot_take 做全屏渲染(类似 LVGL 任务), 2KB 会溢栈崩。
    xTaskCreate(fap_screenshot_task, "fap_shot", 16384, NULL, 3, NULL);

    // 常驻播放任务(静态栈) + 唤醒信号量: 只在启动时创建一次。之前每次 OK 播放都
    // xTaskCreate 一个 16KB 栈任务, 会因堆不足/碎片而失败, 导致"OK 只停不播"。
    if (!s_job_sem) s_job_sem = xSemaphoreCreateBinary();
    if (s_job_sem && !s_player) {
        s_player = xTaskCreateStatic(player_worker, "voice_player",
                                     PLAYER_STACK_BYTES / sizeof(StackType_t),
                                     NULL, 5, s_player_stack, &s_player_tcb);
        if (!s_player) ESP_LOGE(TAG, "常驻播放任务创建失败");
    }

    // 空闲休眠: 5 分钟无按键操作 → 深度休眠; 按键(GPIO0)低电平唤醒。
    const esp_timer_create_args_t sleep_args = { .callback = sleep_timer_cb, .name = "idle_sleep" };
    if (esp_timer_create(&sleep_args, &s_sleep_timer) == ESP_OK) reset_idle_sleep();
}

void voice_app_stop(void) {
    stop_playback();
    if (s_batt_timer) { lv_timer_delete(s_batt_timer); s_batt_timer = NULL; }
    if (bsp_lvgl_lock(1000)) {
        if (s_dir_scr) { lv_obj_delete(s_dir_scr); s_dir_scr = NULL; }
        if (s_list_scr) { lv_obj_delete(s_list_scr); s_list_scr = NULL; }
        if (s_set_scr) { lv_obj_delete(s_set_scr); s_set_scr = NULL; }
        bsp_lvgl_unlock();
    }
}
