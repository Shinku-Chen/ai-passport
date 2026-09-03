cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 音效钥匙扣产品固件不引入自定义 bootloader 组件。
# (upstream 的 recovery_boot_hook 跳转到 0x700000 Recovery, 但本产品分区表
#  没有 recovery 分区 —— 那个偏移是 voicefs 音频数据, 跳转会造成崩溃。
#  故移除 bootloader_components/ 及其 BOOTLOADER_EXTRA_COMPONENT_DIRS 引用。)

project(FoloToy-AI-Passport)
