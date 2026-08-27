// main/dlna_player.h —— DLNA 播放器对外接口。
// 播放管道把"从 URL 拉流 → 解码 → 写 I2S"串起来,由两个独立任务驱动:
//   · http_pull 任务(prio 8):用 esp_http_client 流式读压缩字节,只在环形缓冲有空间时读。
//   · audio_pipeline 任务(prio 12):从环形缓冲取压缩字节,经 music_decoder 解一帧,再 bsp_audio_write。
// 阻塞写本身就是天然流控,无需信号量(见架构评审)。
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 播放状态机(供 UI 显示)。
typedef enum {
    DLNA_PLAYER_IDLE = 0,   // 无流,空闲
    DLNA_PLAYER_CONNECTING, // 正在连接/解析 URI
    DLNA_PLAYER_PLAYING,    // 正在播放
    DLNA_PLAYER_PAUSED,     // 已暂停
    DLNA_PLAYER_STOPPED,    // 手动停止
    DLNA_PLAYER_ERROR,      // 出错(如格式不支持)
} dlna_player_state_t;

// 初始化播放管道所需的全局资源(环形缓冲、解码器、任务句柄)。
// 在 DLNA 服务收到 SetAVTransportURI 前调用一次。
esp_err_t dlna_player_init(void);

// 开始播放一个 HTTP 音频流 URL。典型的 DLNA"投屏"流(http://...)。
// 会启动 http_pull 与 audio_pipeline 两个任务(幂等:已启动则复用)。
esp_err_t dlna_player_play(const char *url);

// 暂停/继续(相当于 DLNA Pause / Play)。
esp_err_t dlna_player_pause(void);
esp_err_t dlna_player_resume(void);

// 停止播放并释放流(相当于 DLNA Stop)。
void dlna_player_stop(void);

// 查询当前播放状态(线程安全)。
dlna_player_state_t dlna_player_get_state(void);

// 查询当前正在播放的 URL 的截断副本到 out(供 UI 显示曲名/来源)。
// 返回实际写入字符数(不含 '\0')。out 容量 max。
int dlna_player_get_title(char *out, size_t max);

// 设置播放音量(0..100),转发到 bsp_audio_set_volume,并记录供 UI 读取。
void dlna_player_set_volume(int percent);
int  dlna_player_get_volume(void);

#ifdef __cplusplus
}
#endif
