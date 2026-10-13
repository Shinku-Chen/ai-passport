// main/gal/gal_music.h —— 音乐鉴赏的播放控制。
//
// 骨架不携带音频资源:按内容层的 notes 用方波合成一段短旋律循环播放,
// 用来把音频通路(I2S + ES8311)接起来。播放跑在独立任务里,不阻塞按键与界面。
#pragma once

#include <stdbool.h>

// 建播放任务(不碰音频硬件;第一次播放时才初始化)。
bool gal_music_init(void);

// 开始播放(或从头重放)某首曲目;越界或未初始化为空操作。
void gal_music_play(int track_index);

// 停止播放(可重复调用)。
void gal_music_stop(void);

// 正在播放的曲目下标;-1 = 没有播放。
int gal_music_current(void);
