# 《沙耶之歌》阅读器:只编译本应用的源码。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "saya_pack.c"
         "saya_model.c"
         "saya_image.c"
         "saya_save.c"
         "saya_ui.c"
         "saya_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包:脚本 + 背景 + 立绘,由 tools/saya_pack.py 生成后直接读 Flash。
    EMBED_FILES "saya_data/saya_pack.bin"
)

# 中文字体子集(assets/fonts 下由 tools/saya_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/saya_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/saya_cjk_20.c"
)
