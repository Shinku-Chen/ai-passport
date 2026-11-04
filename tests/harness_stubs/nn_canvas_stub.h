// tests/harness_stubs/nn_canvas_stub.h —— 分带画布桩的测试钩子。
#pragma once

// 每条带被刷黑 / 被画图的次数（断言“三带都画过”用）。
extern int stub_canvas_black_calls[4];
extern int stub_canvas_draw_calls[4];
extern int stub_canvas_cg_full_calls;

// 模拟另一种分带（回退到 320x60 x 4 带）。
void stub_canvas_set_bands(int band_h, int count);
