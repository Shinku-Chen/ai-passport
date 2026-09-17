// main/c4_sound.h —— 四子棋音效:代码合成的短音,不占 flash 素材,无二进制资源。
//
// 所有声音都在独立的低优先级任务里通过 ES8311 播放;按键回调和输入任务只投递
// 请求(非阻塞),所以慢速 I2S 写入永远不会卡住交互或 LVGL。
#pragma once

#include <stdbool.h>

typedef enum {
    C4_SOUND_START = 0,   // 开始新对局
    C4_SOUND_MOVE,        // 光标换列
    C4_SOUND_DROP,        // 落子
    C4_SOUND_WIN,         // 玩家取胜
    C4_SOUND_LOSE,        // 电脑取胜
    C4_SOUND_DRAW,        // 平局
} c4_sound_id_t;

// 初始化音频与播放任务。失败只打日志并让 c4_sound_play() 变成空操作:
// 游戏不依赖声音,没有 codec 也必须能玩。
bool c4_sound_init(void);

// 投递一个音效(非阻塞,队列满则丢弃)。
void c4_sound_play(c4_sound_id_t id);

// deep sleep 前调用:禁止后续播放,并等到当前音效写完(ES8311 暂停前必须没有 PCM
// 在飞)。成功后调用方可以直接进入休眠,不需要配对释放。
bool c4_sound_hold_for_sleep(void);
