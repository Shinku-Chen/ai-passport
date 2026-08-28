// components/dlna_audio/include/dlna_audio.h
// DLNA 流式音频播放管线:HTTP 拉流 → minimp3/esp_aac 解码 → bsp_audio(I2S)输出。
// 设计为一个小缓冲、单任务、非阻塞控制接口,适配 ESP32-C3 无 PSRAM。
//
// 控制语义:
//   dlna_audio_play_uri()   —— 保存 URI 并启动播放(异步返回,在播放任务里实际拉流)。
//   dlna_audio_pause/resume —— 暂停/恢复。
//   dlna_audio_stop()       —— 停止并清空。
//   dlna_audio_seek()       —— 按秒跳转(尽力,对 HTTP 流基本是重开,慎用)。
//
// 回调运行在播放任务上下文,里面禁止阻塞/做重活,状态更新应只写缓存或派发。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 播放器状态,供 UI/协议层显示。
typedef enum {
    DLNA_AUDIO_IDLE = 0,    // 未播放/已停止
    DLNA_AUDIO_OPENING,     // 建连/探测格式中
    DLNA_AUDIO_PLAYING,     // 正在解码输出
    DLNA_AUDIO_PAUSED,      // 已暂停
    DLNA_AUDIO_ERROR,       // 出错(建连失败/解码失败/断流)
} dlna_audio_state_t;

// 播放状态回调(运行于播放任务)。
typedef void (*dlna_audio_cb_t)(dlna_audio_state_t state, const char *uri, void *user);

// 初始化播放器。cb 可为 NULL。应早于首次 play 调用。
esp_err_t dlna_audio_init(dlna_audio_cb_t cb, void *user);

// 开始播放一个 HTTP 流。uri 会复制;返回 ESP_OK 仅表示已接手,结果经回调通知。
esp_err_t dlna_audio_play_uri(const char *uri);

// 暂停/恢复。
esp_err_t dlna_audio_pause(void);
esp_err_t dlna_audio_resume(void);

// 停止并释放当前流。
esp_err_t dlna_audio_stop(void);

// 按秒 seek(尽力)。u64 秒,Duration 上限由实现决定。
esp_err_t dlna_audio_seek(int seconds);

// 获取当前状态 / 当前音量设置(供协议上报)。
dlna_audio_state_t dlna_audio_get_state(void);
uint32_t dlna_audio_get_position_ms(void);
uint32_t dlna_audio_get_duration_ms(void);
int dlna_audio_get_volume(void);
bool dlna_audio_is_muted(void);

// 音量 0..100,内部映射到 bsp_audio_set_volume。
void dlna_audio_set_volume(int percent);
void dlna_audio_set_mute(bool mute);

// 反向(本地)触发上一曲/下一曲/播放暂停(通过回调由应用层组织)。
// 说明:应用层直接调用 custom_dlna 的 on_next/on_previous 即可,这里不重复暴露。

// 彻底去初始化(退出前调用)。
void dlna_audio_deinit(void);

#ifdef __cplusplus
}
#endif
