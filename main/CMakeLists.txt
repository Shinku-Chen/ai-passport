# 《星空列车与白的旅行》阅读器:只编译本应用的源码。
# 界面层来自 ATRI 阅读器(main/atri_*.c),剧情/资源包/存档用 starry 数据层。
# 旧的手绘逐条带渲染器(starry_render.c / starry_gfx.c / starry_font.c / starry_ui.c /
# starry_app.c)仍留在仓库里(可回退),但不再参与本应用的构建。
idf_component_register(
    SRCS "main.c"
         "starry_pack.c"
         "starry_model.c"
         "starry_save.c"
         "atri_image.c"
         "atri_ui.c"
         "atri_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包:剧本 + 背景 + 立绘(RGB565+4bpp),由 tools/starry_pack.py 生成后直接读 Flash。
    EMBED_FILES "starry_data/starry_pack.bin"
)

# 真机验收开关:开机直接进入某一章(下标见 tools/starry_pack.py, -1 = 正常标题流程)。
# 例如只为了看最后一章:
#   idf.py -DSTARRY_BOOT_CHAPTER=38 build
set(STARRY_BOOT_CHAPTER "-1" CACHE STRING "开机直接进入的章节下标(-1 = 正常标题流程)")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "STARRY_BOOT_CHAPTER=${STARRY_BOOT_CHAPTER}")

# 中文字体子集(assets/fonts 下由 tools/starry_lvgl_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/starry_cjk_16.c"
)
