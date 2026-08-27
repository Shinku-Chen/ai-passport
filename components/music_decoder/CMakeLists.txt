idf_component_register(
    SRCS "minimp3_wrapper.c"
    INCLUDE_DIRS "." "include"
    # minimp3 是纯 C 解码器,无其他依赖
)
