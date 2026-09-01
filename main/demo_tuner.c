// main/demo_tuner.c —— 通用十二平均律调音器页面。
// 用板载麦克风实时采集声音,检测音高,显示音名/八度/频率与 cents 偏差。
// 两种模式(短按 OK 切换):
//   AUTO   —— 自动识别当前音名,指针指示与最近标准音的偏差。
//   MANUAL —— 短按 UP/DOWN 选目标音,指针指示与目标音的偏差(调弦直觉)。
// 音频读取是阻塞式的,放在独立 worker task 里;LVGL 操作经 bsp_lvgl_lock 保护。
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"   // bsp_lvgl_lock / bsp_lvgl_unlock
#include "tuner_engine.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "demo_tuner";

#define TUNER_SAMPLE_RATE 16000
#define TUNER_PCM_BYTES   (TUNER_WINDOW * 2)   // 一个窗口的字节数(16-bit mono)
#define TUNER_UI_MIN_MS   150                  // UI 刷新节流,避免闪屏

// 音名表(MIDI note_idx 0=C)。
static const char *const NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

typedef enum {
    TUNER_MODE_AUTO = 0,
    TUNER_MODE_MANUAL,
} tuner_mode_t;

// 手动模式目标音候选(科学音高 MIDI:C4..B4),UP/DOWN 循环。
static const int MANUAL_MIDI[] = {
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
};
#define MANUAL_COUNT (sizeof(MANUAL_MIDI) / sizeof(MANUAL_MIDI[0]))

// 刻度条几何(绝对坐标,以 s_scr 为参照)。
#define METER_X     22
#define METER_Y     128
#define METER_W     196
#define METER_H     40
#define METER_MIN_X (METER_X + 8)        // 指针最左
#define METER_MAX_X (METER_X + METER_W - 12)
#define METER_CX    ((METER_MIN_X + METER_MAX_X) / 2)
#define METER_PX_CENT 1.6f               // px per cent,±50 → ±80px

static lv_obj_t    *s_scr, *s_note_label, *s_freq_label, *s_cents_label;
static lv_obj_t    *s_mode_label, *s_target_label, *s_batt_label, *s_hint_label;
static lv_obj_t    *s_needle;
static lv_obj_t    *s_mascot;
static TaskHandle_t s_task;
static volatile bool s_running;

static volatile tuner_mode_t s_mode = TUNER_MODE_AUTO;
static volatile int s_manual_midi = 69;                // 默认 A4

// 音频任务→UI 的共享状态(单写者,不需要原子)。
static volatile int   s_note_idx = -1;
static volatile int   s_octave = -1;
static volatile float s_freq = 0.0f;
static volatile float s_cents = 0.0f;
// 诊断中间量(真机验证观测用):归一 RMS / NSDF 可信度 / 原始频率。
static volatile float s_rms = 0.0f;
static volatile float s_nsdf = 0.0f;
static volatile float s_raw_freq = 0.0f;

// 调试模式:长按 OK 切换。显示检测中间量,UP/DOWN 调麦克风增益。
static volatile bool  s_debug = false;
static volatile float s_gain_db = 30.0f;               // 当前麦克风 PGA 增益(dB)
#define TUNER_GAIN_STEP_DB 3.0f

// LVGL 对象只在持锁时操作;从音频任务调用,故内部加锁。
static void ui_set_text(lv_obj_t *obj, const char *text)
{
    if (!obj) return;
    if (!bsp_lvgl_lock(200)) return;
    lv_label_set_text(obj, text);
    bsp_lvgl_unlock();
}

// 按 midi 号格式化目标音字符串,如 "A4"。
static void format_midi(char *buf, size_t len, int midi)
{
    if (midi < 0) { snprintf(buf, len, "--"); return; }
    int idx = ((midi % 12) + 12) % 12;
    int oct = midi / 12 - 1;
    snprintf(buf, len, "%s%d", NOTE_NAMES[idx], oct);
}

// 刷新整个读数区(音名/频率/指针/cent 值)。从音频任务调用,内部加锁。
static void ui_refresh_reading(void)
{
    if (!s_scr) return;
    if (!bsp_lvgl_lock(200)) return;

    char buf[64];
    float shown_cents = 0.0f;   // 实际展示给用户的偏差(相对目标或最近音)

    if (s_debug) {
        // 调试模式:大字显示原始频率,下方显示 RMS/NSDF/增益。
        if (s_raw_freq > 0.0f) {
            lv_label_set_text_fmt(s_note_label, "%.0f", (double)s_raw_freq);
            lv_label_set_text_fmt(s_freq_label, "RMS %.3f", (double)s_rms);
            lv_label_set_text_fmt(s_cents_label, "NSDF %.2f  G%.0fdB",
                                  (double)s_nsdf, (double)s_gain_db);
        } else {
            lv_label_set_text(s_note_label, "--");
            lv_label_set_text_fmt(s_freq_label, "RMS %.3f", (double)s_rms);
            lv_label_set_text_fmt(s_cents_label, "NSDF --  G%.0fdB",
                                  (double)s_gain_db);
        }
        // 指针在 debug 下无意义,回中灰。
        lv_obj_set_x(s_needle, METER_CX);
        lv_obj_set_style_bg_color(s_needle, lv_color_hex(0x9AA7B0), 0);
        bsp_lvgl_unlock();
        return;
    }

    if (s_note_idx < 0) {
        lv_label_set_text(s_note_label, "--");
        lv_label_set_text(s_freq_label, "-- Hz");
        lv_label_set_text(s_cents_label, "0 cents");
        // 指针回中,灰。
        lv_obj_set_x(s_needle, METER_CX);
        lv_obj_set_style_bg_color(s_needle, lv_color_hex(0x9AA7B0), 0);
    } else {
        if (s_mode == TUNER_MODE_MANUAL) {
            // 偏差相对目标音(调弦直觉):数字与指针都指向目标音的偏差。
            int midi = s_manual_midi;
            int idx = ((midi % 12) + 12) % 12;
            int oct = midi / 12 - 1;
            float target_hz = TUNER_A4_HZ * powf(2.0f, (float)(midi - 69) / 12.0f);
            shown_cents = 1200.0f * log2f(s_freq / target_hz);
            snprintf(buf, sizeof(buf), "%s%d", NOTE_NAMES[idx], oct);
            lv_label_set_text(s_note_label, buf);
            lv_label_set_text_fmt(s_freq_label, "%.1f Hz", (double)s_freq);
            lv_label_set_text_fmt(s_cents_label, "%+.0f cents", (double)shown_cents);
        } else {
            // AUTO:显示最近音名 + 相对该音的偏差。
            shown_cents = s_cents;
            snprintf(buf, sizeof(buf), "%s%d", NOTE_NAMES[s_note_idx], s_octave);
            lv_label_set_text(s_note_label, buf);
            lv_label_set_text_fmt(s_freq_label, "%.1f Hz", (double)s_freq);
            lv_label_set_text_fmt(s_cents_label, "%+.0f cents", (double)shown_cents);
        }
        float cx = shown_cents < -50.0f ? -50.0f : (shown_cents > 50.0f ? 50.0f : shown_cents);
        lv_obj_set_x(s_needle, (int)(METER_CX + cx * METER_PX_CENT));
        bool in_tune = (shown_cents > -5.0f && shown_cents < 5.0f);
        lv_obj_set_style_bg_color(s_needle,
            lv_color_hex(in_tune ? 0x2ECC71 : UI_INK), 0);
    }

    bsp_lvgl_unlock();
}

// 更新电池角标。soc<0 显示 "--"。
static void ui_update_battery(void)
{
    if (!s_batt_label) return;
    if (!bsp_lvgl_lock(200)) return;
    int soc = bsp_battery_soc();
    if (soc < 0) lv_label_set_text(s_batt_label, "--");
    else         lv_label_set_text_fmt(s_batt_label, "%d%%", soc);
    bsp_lvgl_unlock();
}

// 音频采集 + 检测任务。
static void tuner_task(void *arg)
{
    (void)arg;
    int16_t *pcm = malloc(TUNER_PCM_BYTES);
    if (!pcm) {
        ESP_LOGE(TAG, "PCM 缓冲分配失败");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (bsp_audio_set_format(TUNER_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "audio format failed");
        free(pcm);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    uint32_t last_ui_ms = 0;
    uint32_t last_log_ms = 0;
    while (s_running) {
        if (bsp_audio_read(pcm, TUNER_PCM_BYTES) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        tuner_diag_t diag;
        float freq = tuner_detect_pitch_diag(pcm, TUNER_WINDOW, TUNER_SAMPLE_RATE, &diag);
        s_rms = diag.rms;
        s_nsdf = diag.nsdf_peak;
        if (freq > 0.0f) {
            tuner_note_t note = tuner_quantize(freq);
            if (note.note_idx >= 0) {
                s_note_idx = note.note_idx;
                s_octave = note.octave;
                s_freq = freq;
                s_cents = note.cents;
                s_raw_freq = diag.raw_freq;
            }
        } else {
            s_note_idx = -1;   // 无输入
            s_raw_freq = 0.0f;
        }

        // 节流刷新 UI。
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_ui_ms >= TUNER_UI_MIN_MS) {
            ui_refresh_reading();
            last_ui_ms = now;
        }

        // 节流打印检测中间量(真机验证观测用,不进屏只进日志)。
        if (s_debug && now - last_log_ms >= 500) {
            if (freq > 0.0f) {
                tuner_note_t note = tuner_quantize(freq);
                ESP_LOGI(TAG,
                         "diag: raw=%.1fHz rms=%.3f nsdf=%.2f lag=%d -> %s%d %.1fHz %+.1fcents gain=%.0fdB",
                         (double)diag.raw_freq, (double)diag.rms,
                         (double)diag.nsdf_peak, diag.lag,
                         NOTE_NAMES[note.note_idx], note.octave,
                         (double)freq, (double)note.cents, (double)s_gain_db);
            } else {
                ESP_LOGI(TAG, "diag: NO signal rms=%.3f gain=%.0fdB",
                         (double)diag.rms, (double)s_gain_db);
            }
            last_log_ms = now;
        }
    }

    free(pcm);
    vTaskDelete(NULL);
}

void demo_tuner_enter(void)
{
    s_scr = ui_pixel_screen_create("TUNER");
    s_running = true;

    // 中央大号音名 + 八度(如 "A4")。
    s_note_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_note_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_note_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_note_label, LV_ALIGN_TOP_MID, 0, 50);
    lv_label_set_text(s_note_label, "--");

    // 频率。
    s_freq_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_freq_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_freq_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_freq_label, LV_ALIGN_TOP_MID, 0, 96);
    lv_label_set_text(s_freq_label, "-- Hz");

    // cents 偏差刻度条面板。
    lv_obj_t *meter = ui_pixel_panel_create(s_scr, METER_X, METER_Y, METER_W, METER_H,
                                            UI_PAPER);
    (void)meter;
    // 中刻线(绝对坐标)。
    lv_obj_t *center = lv_obj_create(s_scr);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(center, METER_CX - 1, METER_Y + 6);
    lv_obj_set_size(center, 2, METER_H - 12);
    lv_obj_set_style_bg_color(center, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(center, 0, 0);
    // 左右 ± 标记。
    lv_obj_t *minus = ui_pixel_label(s_scr, "-50", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_pos(minus, METER_X + 2, METER_Y + 11);
    lv_obj_t *plus = ui_pixel_label(s_scr, "+50", &lv_font_montserrat_14, UI_MUTED);
    lv_obj_set_pos(plus, METER_X + METER_W - 30, METER_Y + 11);
    // 指针。
    s_needle = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_needle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_needle, METER_CX - 2, METER_Y + 6);
    lv_obj_set_size(s_needle, 4, METER_H - 12);
    lv_obj_set_style_bg_color(s_needle, lv_color_hex(0x9AA7B0), 0);
    lv_obj_set_style_border_width(s_needle, 0, 0);

    // cents 数值。
    s_cents_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_cents_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cents_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_cents_label, LV_ALIGN_TOP_MID, 0, 176);
    lv_label_set_text(s_cents_label, "0 cents");

    // 模式 + 目标音。
    s_mode_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_mode_label, LV_ALIGN_TOP_MID, 0, 206);
    lv_label_set_text(s_mode_label, "AUTO");   // 进入页面默认非调试

    s_target_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_target_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_target_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_target_label, LV_ALIGN_TOP_MID, 0, 228);
    lv_label_set_text(s_target_label, "target: --");

    // 底部提示。
    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_label_set_text(s_hint_label, "pluck. LONG OK: debug");

    // 右上角电池(避让右上云朵 x≈188,y≈8,放最右上缘)。
    s_batt_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_batt_label, LV_ALIGN_TOP_RIGHT, -6, 4);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 238);

    // 初始显示。
    s_note_idx = -1; s_octave = -1; s_freq = 0.0f; s_cents = 0.0f;
    s_rms = 0.0f; s_nsdf = 0.0f; s_raw_freq = 0.0f;
    s_debug = false;
    s_gain_db = 30.0f;
    ui_update_battery();
    ui_refresh_reading();

    if (!s_task) xTaskCreate(tuner_task, "demo_tuner", 4096, NULL, 4, &s_task);
    lv_screen_load(s_scr);
}

void demo_tuner_exit(void)
{
    s_running = false;
    if (s_task) {
        // 音频读取阻塞,任务会在下一次 read 返回后检查标志退出。
        vTaskDelay(pdMS_TO_TICKS(40));
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_note_label = s_freq_label = s_cents_label = NULL;
        s_mode_label = s_target_label = s_batt_label = s_hint_label = NULL;
        s_needle = s_mascot = NULL;
    }
}

void demo_tuner_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 长按 OK:切换调试模式。长按不会被当作单击,和短按切换模式不冲突。
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        s_debug = !s_debug;
        if (s_debug) {
            ui_set_text(s_mode_label, "DEBUG");
        } else {
            // 退出调试:恢复当前模式(AUTO/MANUAL)标签。
            ui_set_text(s_mode_label, s_mode == TUNER_MODE_AUTO ? "AUTO" : "MANUAL");
        }
        ui_refresh_reading();
        ui_pixel_mascot_jump(s_mascot);
        return;
    }

    if (ev != BSP_BTN_CLICK) return;

    // 调试模式下 UP/DOWN 调麦克风增益。
    if (s_debug) {
        if (btn == BSP_BTN_UP)   s_gain_db += TUNER_GAIN_STEP_DB;
        else if (btn == BSP_BTN_DOWN) s_gain_db -= TUNER_GAIN_STEP_DB;
        if (s_gain_db > 30.0f) s_gain_db = 30.0f;
        if (s_gain_db < 0.0f)  s_gain_db = 0.0f;
        bsp_audio_set_in_gain(s_gain_db);
        ui_refresh_reading();
        ui_pixel_mascot_jump(s_mascot);
        return;
    }

    if (btn == BSP_BTN_OK) {
        s_mode = (s_mode == TUNER_MODE_AUTO) ? TUNER_MODE_MANUAL : TUNER_MODE_AUTO;
        ui_set_text(s_mode_label, s_mode == TUNER_MODE_AUTO ? "AUTO" : "MANUAL");
    } else if ((btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) &&
               s_mode == TUNER_MODE_MANUAL) {
        int i = 0;
        for (int k = 0; k < (int)MANUAL_COUNT; k++) {
            if (MANUAL_MIDI[k] == s_manual_midi) { i = k; break; }
        }
        if (btn == BSP_BTN_UP) i = (i + 1) % (int)MANUAL_COUNT;
        else                   i = (i + (int)MANUAL_COUNT - 1) % (int)MANUAL_COUNT;
        s_manual_midi = MANUAL_MIDI[i];
    }

    // 刷新模式/目标音显示。
    if (s_mode == TUNER_MODE_MANUAL) {
        char buf[16];
        format_midi(buf, sizeof(buf), s_manual_midi);
        ui_set_text(s_target_label, buf);
    } else {
        ui_set_text(s_target_label, "target: --");
    }
    ui_refresh_reading();
    ui_pixel_mascot_jump(s_mascot);
}
