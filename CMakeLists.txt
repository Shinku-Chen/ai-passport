cmake_minimum_required(VERSION 3.16)

# 正式发布版本号：必须在 include(project.cmake)/project() **之前**设置，
# 否则会被 git describe（3c2e5013-dirty）覆盖。esp_app_desc.version = 1.0.0。
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(FoloToy-AI-Passport)
