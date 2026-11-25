// tests/maitetsu_stubs/sdkconfig.h —— host 测试用的 esp_jpeg TJpgDec 配置。
#pragma once

#define CONFIG_JD_SZBUF 512
#define CONFIG_JD_FORMAT 1          // RGB565
#define CONFIG_JD_USE_SCALE 0
#define CONFIG_JD_TBLCLIP 1
#define CONFIG_JD_FASTDECODE 1
#define CONFIG_JD_DEFAULT_HUFFMAN 0
