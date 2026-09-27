# 《魔女的夜宴》阅读器:只编译本应用的源码。
# 界面层沿用同一套阅读器骨架(main/sanoba_ui.c / sanoba_image.c / sanoba_app.c),
# 剧情/资源包/存档用 sanoba 数据层(sanoba_model.c / sanoba_pack.c / sanoba_save.c)。
idf_component_register(
    SRCS "main.c"
         "sanoba_pack.c"
         "sanoba_model.c"
         "sanoba_inflate.c"
         "sanoba_save.c"
         "sanoba_image.c"
         "sanoba_ui.c"
         "sanoba_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包:图片包(背景 + SD 横条,JPEG)与剧本包(SANOSCN1,逐块 raw deflate),
    # 分别由 tools/sanoba_pack.py、tools/sanoba_scn_pack.py 生成后直接映射进 Flash
    # 读取,运行时不解压整包。
    EMBED_FILES "sanoba_data/sanoba_pack.bin"
                "sanoba_data/sanoba_scn.bin"
)

# 真机验收开关:开机直接进入某个场景(下标 0 起,对应工具里的场景表;默认 -1 = 正常标题流程)。
# 例如只看第 20 个场景:
#   idf.py -DSANOBA_BOOT_SCENARIO=19 build
set(SANOBA_BOOT_SCENARIO "-1" CACHE STRING "开机直接进入的场景下标(-1 = 正常标题流程)")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SANOBA_BOOT_SCENARIO=${SANOBA_BOOT_SCENARIO}")

# 中文字体子集(assets/fonts 下由 tools/sanoba_font.py 生成;字形顺序与剧本包
# SEC_CHAR 一致,所以不会缺字)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/sanoba_cjk_16.c"
)
