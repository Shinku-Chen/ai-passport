# 《千恋＊万花》阅读器:只编译本应用的源码。
# 界面层来自 ATRI 阅读器(main/atri_*.c),剧情/资源包/存档用 senren 数据层。
# 旧的手绘逐条带渲染器(senren_render.c / senren_gfx.c / senren_font.c / senren_ui.c /
# senren_app.c)不在此分支,本应用只依赖下面的源文件。
idf_component_register(
    SRCS "main.c"
         "senren_pack.c"
         "senren_model.c"
         "senren_inflate.c"
         "senren_save.c"
         "atri_image.c"
         "atri_ui.c"
         "atri_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg
    # 资源包:图片包(背景/立绘/事件 CG,JPEG + 调色板 PNG)与剧本包(SENRSCN1,
    # 逐块 raw deflate),分别由 tools/senren_pack.py、tools/senren_scn_pack.py 生成后
    # 直接映射进 Flash 读取,运行时不解压整包。
    EMBED_FILES "senren_data/senren_pack.bin"
                "senren_data/senren_scn.bin"
)

# 真机验收开关:开机直接进入某一章(章号与画面上的「第 N 章」一致,1 起;0 = 剧本开头,
# -1 = 正常标题流程)。例如只为了看第八章:
#   idf.py -DSENREN_BOOT_CHAPTER=8 build
set(SENREN_BOOT_CHAPTER "-1" CACHE STRING "开机直接进入的章号(-1 = 正常标题流程)")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SENREN_BOOT_CHAPTER=${SENREN_BOOT_CHAPTER}")

# 中文字体子集(assets/fonts 下由 tools/senren_lvgl_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/senren_cjk_16.c"
)
