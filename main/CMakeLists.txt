# GalGame 框架（AI Passport）：只编译框架应用的源码。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里（可切回硬件自检 demo），
# 但不再参与本应用的构建，也不再注册到菜单。
#
# 资源包默认走仓库里提交的 community 变体（不含补丁/R18 内容）；自用的 release
# 变体（含补丁）用命令行覆盖它：
#   -D GAL_PACK_FILE=<用 tools/saya_pack.py --patch 生成的 pack>
# 生成命令见 tools/saya_pack.py 的文件头。这类产物放在 build/（已被 .gitignore
# 忽略）下，禁止提交，也不要用它构建发布到社区的固件。
#
# 字体不需要覆盖：assets/fonts 里的子集是 community + release 两个变体字符的并集，
# 两个变体共用同一份字体，设备上不会出现缺字方块。
set(GAL_PACK_FILE "${CMAKE_CURRENT_LIST_DIR}/gal/saya_data/saya_pack.bin" CACHE FILEPATH
    "GalGame 资源包（默认：仓库内的 community 变体）")
if(NOT EXISTS "${GAL_PACK_FILE}")
    message(FATAL_ERROR "找不到 ${GAL_PACK_FILE}（GAL_PACK_FILE 设置是否正确？）")
endif()

idf_component_register(
    SRCS "main.c"
         "gal/saya_pack.c"
         "gal/saya_model.c"
         "gal/saya_image.c"
         "gal/saya_save.c"
         "gal/saya_ui.c"
         "gal/saya_app.c"
    INCLUDE_DIRS "." "gal"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包：脚本 + 背景 + 立绘，由 tools/saya_pack.py 生成后直接读 Flash。
    EMBED_FILES "${GAL_PACK_FILE}"
)

# 中文字体子集（assets/fonts 下由 tools/saya_font.py 生成，两个变体共用）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/saya_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/saya_cjk_20.c"
)
