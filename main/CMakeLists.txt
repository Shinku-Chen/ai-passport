idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "ui_eatwhat_math.c"
         "ui_autopower_math.c"
         "ui_eatwhat_render.c"
         "autopower.c"
         "demo_eat_what.c"
    INCLUDE_DIRS "."
    EMBED_FILES "eat_what_g1.bin"
                "eat_what_g2.bin"
    # 只保留显示/按键/显示能力所需组件;Wi-Fi/NimBLE/音频等模块已移除。
    # esp_lcd 供 ui_eatwhat_render.c 的 esp_lcd_panel_draw_bitmap 直刷 panel。
    REQUIRES bsp esp_event esp_hw_support esp_lcd esp_timer nvs_flash
)
