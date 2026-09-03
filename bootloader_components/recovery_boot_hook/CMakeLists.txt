file(TO_CMAKE_PATH "$ENV{IDF_PATH}" _IDF_PATH_FWD)  # Windows 反斜杠 -> CMake 转发斜杠
idf_component_register(
    SRCS "recovery_boot_hook.c"
    PRIV_REQUIRES bootloader_support
    PRIV_INCLUDE_DIRS "${_IDF_PATH_FWD}/components/bootloader_support/private_include"
)
