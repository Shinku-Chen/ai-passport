// main/gal/maitetsu_media_embed.c —— 固件的内嵌媒体包入口。
//
// 与 maitetsu_media.c 分开，host 测试可以只编译适配层并用文件内容初始化
// （maitetsu_media_init_from），不需要链接 5.8 MB 的二进制符号。
#include "maitetsu_media.h"

extern const uint8_t maitetsu_media_bin_start[] asm("_binary_maitetsu_media_bin_start");
extern const uint8_t maitetsu_media_bin_end[] asm("_binary_maitetsu_media_bin_end");

bool maitetsu_media_init(void)
{
    return maitetsu_media_init_from(maitetsu_media_bin_start,
                                    (size_t)(maitetsu_media_bin_end - maitetsu_media_bin_start));
}
