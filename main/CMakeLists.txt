# 《ATRI -My Dear Moments-》阅读器:只编译本应用的源码。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "atri_pack.c"
         "atri_model.c"
         "atri_image.c"
         "atri_save.c"
         "atri_ui.c"
         "atri_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包:剧本 + 背景 + 叠加,由 tools/atri_pack.py 生成后直接读 Flash。
    EMBED_FILES "atri_data/atri_pack.bin"
)

# 中文字体子集(assets/fonts 下由 tools/atri_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/atri_cjk_16.c"
)
