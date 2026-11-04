# main/CMakeLists.txt —— 《男娘恋爱物语》横屏阅读器。
#
# 只编译阅读器本身（nn_* + 排版纯逻辑 gal_text）；框架骨架的 gal_app/gal_ui/
# demo_* 不再参与构建，启动直接进入作品标题页。
idf_component_register(
    SRCS "main.c"
         "nn_pack.c"
         "nn_script.c"
         "nn_image.c"
         "nn_canvas.c"
         "nn_save.c"
         "nn_save_codec.c"
         "nn_ui.c"
         "nn_app.c"
         "nn_jpeg/tjpgd.c"
         "gal/gal_text.c"
    INCLUDE_DIRS "." "nn_jpeg" "gal"
    REQUIRES bsp lvgl esp_timer nvs_flash esp_hw_support
)

# 资源包直接编进应用镜像（.rodata，由指令 cache 映射）：
# 不占分区、不需要 mmap 窗口，运行时是普通只读指针。
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../assets/nn/nn_pack.bin" BINARY)

# 中文字库子集（tools/gal_font.py 生成；源字体不入库）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/gal_cjk_20.c"
)
