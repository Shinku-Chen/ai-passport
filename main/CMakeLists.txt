# 《Summer Pockets REFLECTION BLUE》AI Passport 阅读器：只编译本应用的源码。
#
# 资源包：tools/summer_pack.py 从 RW5 源快照生成 main/summer_data/summer_pack.bin，
# 整包嵌入 Flash；固件运行时直接读，不经文件系统/JSON。
set(SUMMER_PACK_FILE "${CMAKE_CURRENT_LIST_DIR}/summer_data/summer_pack.bin" CACHE FILEPATH
    "Summer Pockets 资源包")
if(NOT EXISTS "${SUMMER_PACK_FILE}")
    message(FATAL_ERROR "找不到 ${SUMMER_PACK_FILE}（先运行 tools/summer_pack.py）")
endif()

idf_component_register(
    SRCS "main.c"
         "summer_pack.c"
         "summer_model.c"
         "summer_text.c"
         "summer_store.c"
         "summer_image.c"
         "summer_ui.c"
         "summer_app.c"
         "vendor/tjpgd/tjpgd.c"
    INCLUDE_DIRS "." "vendor/tjpgd"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support
    EMBED_FILES "${SUMMER_PACK_FILE}"
)

# 16px 完整中文字库（tools/summer_font.py 生成）。20px 变体保留在 assets 供覆盖
# 自检，但本固件不链接：Flash 预算优先留给 81,040 句剧情、CG 与表情。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
)
