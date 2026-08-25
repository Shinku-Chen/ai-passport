idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "voice_app.c"
         "fonts/voice_cjk.c"
         "fonts/voice_hint.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl spiffs esp_timer
)

# 让 lv_font_conv 生成的字体用简单 include(<lvgl.h>), 而非带 lvgl/ 前缀的路径
target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_LVGL_H_INCLUDE_SIMPLE=1)
