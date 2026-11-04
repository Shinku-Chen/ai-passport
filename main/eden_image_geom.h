// main/eden_image_geom.h —— 画面贴图的纯几何校验(无 ESP/LVGL 依赖,可宿主机测试)。
//
// 补丁贴进画面缓冲时最容易出的两类错误都在这里一次算清:
//   1. 坐标落在缓冲之外 —— 之前用 (CANVAS_W - x) 直接算列数,x >= CANVAS_W 会无符号回绕
//      成一个巨大的 memcpy 长度;
//   2. 尺寸相加回绕 —— u16 的 x + w 会越过 65535 绕回小值,越界看起来"合法"。
// 因此统一用 u32 相加,并把"落在缓冲内 / 需要裁剪"都表达成显式返回值。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t x;      // 目标列(已在缓冲内)
    uint16_t y;      // 目标行(已在缓冲内)
    uint16_t cols;   // 实际可写的列数(= min(w, canvas_w - x))
    uint16_t rows;   // 实际可写的行数(= min(h, canvas_h - y))
} eden_blit_span_t;

// 计算一次贴图的可见范围。
// 返回 false 表示整块都在缓冲之外(或宽高为 0),调用方应跳过并视为坏数据;
// 返回 true 表示至少有 1 个像素要写,span 里的 cols/rows 可能小于传入的 w/h(被右/下边界裁剪)。
static inline bool eden_blit_span(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                  uint16_t canvas_w, uint16_t canvas_h,
                                  eden_blit_span_t *span)
{
    if (span == 0 || canvas_w == 0 || canvas_h == 0 || w == 0 || h == 0) {
        return false;
    }
    if (x >= canvas_w || y >= canvas_h) {
        return false;
    }
    const uint32_t cols = (uint32_t)x + w <= canvas_w ? w : (uint32_t)(canvas_w - x);
    const uint32_t rows = (uint32_t)y + h <= canvas_h ? h : (uint32_t)(canvas_h - y);
    span->x = x;
    span->y = y;
    span->cols = (uint16_t)cols;
    span->rows = (uint16_t)rows;
    return cols > 0 && rows > 0;
}
