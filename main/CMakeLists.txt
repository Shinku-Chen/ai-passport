# 《想成为Galgame领域大神！！！》AI Passport 移植版:只编译本应用的源码。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
#
# 资源包:tools/galgod_pack.py 生成后放在 main/gal_data/gal_pack.bin,随固件嵌入。
# 换源素材后重建:
#   python tools/galgod_pack.py --script <band-src/common/scn> \
#       --images <upstream-src/common/img> --out main/gal_data/gal_pack.bin
set(GAL_PACK_FILE "${CMAKE_CURRENT_LIST_DIR}/gal_data/gal_pack.bin" CACHE FILEPATH
    "GalGame 资源包")
if(NOT EXISTS "${GAL_PACK_FILE}")
    message(FATAL_ERROR "找不到 ${GAL_PACK_FILE}(先运行 tools/galgod_pack.py)")
endif()

idf_component_register(
    SRCS "main.c"
         "gal_text.c"
         "gal_pack.c"
         "gal_model.c"
         "gal_image.c"
         "gal_store.c"
         "gal_ui.c"
         "gal_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包:脚本 + 背景/CG + 立绘,直接读 Flash。
    EMBED_FILES "${GAL_PACK_FILE}"
)

# 中文字体子集(assets/fonts 下由 tools/galgod_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_20.c"
)
