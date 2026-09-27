# limelight lemonade jam 阅读器:只编译本应用的源码。
# 界面层来自 ATRI 阅读器(main/limelight_ui.c、limelight_image.c、limelight_app.c),
# 剧本/素材/存档由 limelight 数据层驱动(limelight_script.c / limelight_assets.c /
# limelight_model.c / limelight_save.c)。
idf_component_register(
    SRCS "main.c"
         "limelight_script.c"
         "limelight_assets.c"
         "limelight_model.c"
         "limelight_save.c"
         "limelight_image_math.c"
         "limelight_image.c"
         "limelight_ui.c"
         "limelight_app.c"
         "limelight_inflate.c"
    INCLUDE_DIRS "."
    # esp_driver_usb_serial_jtag:控制台走驱动快路径(社区抓屏协议要求秒级传完 150KB)
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support esp_jpeg esp_driver_usb_serial_jtag
    # 资源包:素材(JPEG + 立绘遮罩 + 名字表)与剧本(分块 deflate),由 tools/ 下的
    # limelight_material_pack.py / limelight_script_pack.py 生成后直接读 Flash。
    EMBED_FILES "limelight_data/limelight_pack.bin"
                "limelight_data/limelight_script.bin"
)

# 真机验收开关:开机直接进入某一句对白(-1 = 正常走标题流程)。
# 例如只为了看某一章:LIMELIGHT_BOOT_ID=14848
set(LIMELIGHT_BOOT_ID "-1" CACHE STRING "开机直接进入的对白 id(-1 = 正常标题流程)")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "LIMELIGHT_BOOT_ID=${LIMELIGHT_BOOT_ID}")

# 中文字体子集(assets/fonts 下由 tools/limelight_lvgl_font.py 生成)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/limelight_cjk_16.c"
)
