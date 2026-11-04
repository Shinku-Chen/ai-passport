# eden* 阅读器:只编译本应用的源码。
# baseline 的 demo_*.c / ui_pixel.c / gal_app.c 仍留在仓库里(框架骨架还在),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "eden_pack.c"
         "eden_script.c"
         "eden_image.c"
         "eden_store.c"
         "eden_store_nvs.c"
         "eden_ui.c"
         "eden_app.c"
         "gal/gal_text.c"
         "gal/gal_settings.c"
    INCLUDE_DIRS "." "gal"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包(图片 + 剧本 + 章节 + 鉴赏索引)由 tools/eden_pack.py 生成后直接映射进
    # Flash 读取,运行时不解析 JSON、不解压整包。
    EMBED_FILES "eden_data/eden_pack.bin"
)

# 中文字体子集(assets/fonts 下由 tools/gal_font.py 生成,字符集来自资源包与 main/ 的字面量)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_20.c"
)
