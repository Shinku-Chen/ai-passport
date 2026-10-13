# GalGame 框架骨架:只编译框架应用的源码。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "gal/gal_text.c"
         "gal/gal_content.c"
         "gal/gal_settings.c"
         "gal/gal_save.c"
         "gal/gal_ui.c"
         "gal/gal_app.c"
    INCLUDE_DIRS "." "gal"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support
)

# 中文字体子集(assets/fonts 下由 tools/gal_font.py 生成;源字体不入库)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_20.c"
)
