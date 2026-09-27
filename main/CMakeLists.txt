# 《天使☆騒々 RE-BOOT!》阅读器:只编译本应用的源码。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "tsxx_pack.c"
         "tsxx_model.c"
         "tsxx_image.c"
         "tsxx_save.c"
         "tsxx_ui.c"
         "tsxx_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包:剧本 + 背景 + 立绘 + 事件图,由 tools/tsxx_pack.py 生成后直接读 Flash。
    EMBED_FILES "tsxx_data/tsxx_pack.bin"
)

# 中文字体子集(assets/fonts 下由 tools/tsxx_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/tsxx_cjk_16.c"
)
