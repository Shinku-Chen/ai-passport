# 《DRACU-RIOT!》阅读器:只编译本应用的源码。
# 界面层来自 ATRI / 千恋万花阅读器(main/dracu_ui.c、dracu_image.c、dracu_app.c),
# 剧情/资源包/存档用 dracu 数据层(main/dracu_model.c、dracu_scn.c、dracu_pack.c)。
# baseline 的 demo_*.c / ui_pixel.c 仍留在仓库里(可切回硬件自检 demo),
# 但不再参与本应用的构建,也不再注册到菜单。
idf_component_register(
    SRCS "main.c"
         "dracu_pack.c"
         "dracu_scn.c"
         "dracu_model.c"
         "dracu_inflate.c"
         "dracu_save.c"
         "dracu_image.c"
         "dracu_ui.c"
         "dracu_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg esp_driver_usb_serial_jtag
    # 资源包:图片包(背景/事件 CG/立绘/SD,JPEG + 分块调色板)与剧本包(DRACUSC1,
    # 逐块 raw deflate),分别由 tools/dracu_pack.py、tools/dracu_scn_pack.py 生成后
    # 直接映射进 Flash 读取,运行时不解压整包。
    EMBED_FILES "dracu_data/dracu_pack.bin"
                "dracu_data/dracu_scn.bin"
)

# 真机验收开关:开机直接进入某一页(页号就是源工程剧本页表的编号,1 起;
# -1 = 正常标题流程)。例如只为了看梓线开头:
#   idf.py -DDRACU_BOOT_PAGE=42333 build
set(DRACU_BOOT_PAGE "-1" CACHE STRING "开机直接进入的页号(-1 = 正常标题流程)")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "DRACU_BOOT_PAGE=${DRACU_BOOT_PAGE}")

# 中文字体子集(assets/fonts 下由 tools/dracu_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/dracu_cjk_16.c"
)
