// tests/test_anemoi_pack.c —— 资源包解析层：真实包 + 畸形包的边界行为。
#include "anemoi_pack.h"

#include "anemoi_test_util.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int check_real_pack(const uint8_t *blob, size_t size)
{
    anem_pack_t pack;
    ANEM_CHECK(anem_pack_open(&pack, blob, size));
    ANEM_CHECK(anem_pack_op_count(&pack) > 50000);
    ANEM_CHECK(anem_pack_scene_count(&pack) == 71);
    ANEM_CHECK(anem_pack_image_count(&pack) > 1000);
    ANEM_CHECK(anem_pack_chapter_count(&pack) == 264);
    ANEM_CHECK(anem_pack_gallery_count(&pack) > 0);

    // 文本：第 0 条与越界
    const char *text = anem_pack_text(&pack, 0);
    ANEM_CHECK(text != NULL && strlen(text) > 0);
    ANEM_CHECK(anem_pack_text(&pack, 0xFFFF) == NULL);
    ANEM_CHECK(anem_pack_text(&pack, 60000) == NULL);

    // 场景区间：流程场景 2 与路线场景 3 都在包内
    uint32_t first = 0, count = 0;
    ANEM_CHECK(anem_pack_scene_range(&pack, 2, &first, &count));
    ANEM_CHECK(count > 1000);
    ANEM_CHECK(anem_pack_scene_range(&pack, 3, &first, &count));
    ANEM_CHECK(count > 400);
    ANEM_CHECK(!anem_pack_scene_range(&pack, 9999, &first, &count));

    // entry -> op 映射（流程场景 entry 100 必须解析到包内指令）
    const uint32_t op = anem_pack_entry_op(&pack, 2, 100);
    ANEM_CHECK(op != ANEM_NONE32);
    anem_op_t record;
    ANEM_CHECK(anem_pack_op(&pack, op, &record));

    // 姿势查找：真实姿势 + 回退 + 不存在
    anem_pose_view_t pose;
    ANEM_CHECK(anem_pack_pose(&pack, "rk11_17", 1, 1, &pose));
    ANEM_CHECK(pose.base_w > 0 && pose.base_h > 0);
    ANEM_CHECK(pose.body != ANEM_NONE16);
    ANEM_CHECK(!anem_pack_pose(&pack, "zz99_99", 1, 1, &pose));
    ANEM_CHECK(!anem_pack_pose(&pack, "", 1, 1, &pose));

    // 背景名 -> 图像
    ANEM_CHECK(anem_pack_image_by_name(&pack, "bg_black") != ANEM_NONE16);
    ANEM_CHECK(anem_pack_image_by_name(&pack, "definitely_missing") == ANEM_NONE16);

    // 章节名 / meta
    anem_chap_t chap;
    ANEM_CHECK(anem_pack_chapter_for(&pack, 2, &chap));
    ANEM_CHECK(anem_pack_text(&pack, chap.source) != NULL);
    ANEM_CHECK(anem_pack_scene_by_source(&pack, "001_シナリオフロー") == 2);
    const char *value = NULL;
    uint32_t len = 0;
    ANEM_CHECK(anem_pack_meta(&pack, "scope", &value, &len));
    ANEM_CHECK(len == 6 && memcmp(value, "common", 6) == 0);
    ANEM_CHECK(anem_pack_meta(&pack, "nope", &value, &len) == false);

    // 图像条目与载荷边界
    anem_image_t image;
    ANEM_CHECK(anem_pack_image(&pack, 0, &image));
    ANEM_CHECK(anem_pack_blob(&pack, image.off, image.len) != NULL);
    {
        uint32_t blob_size = 0;
        ANEM_CHECK(anem_pack_section(&pack, ANEM_SEC_BLOB, NULL, NULL, &blob_size));
        ANEM_CHECK(anem_pack_blob(&pack, blob_size, 1) == NULL);
        ANEM_CHECK(anem_pack_blob(&pack, 0, blob_size + 1) == NULL);
    }
    return 0;
}

static int check_corrupt(uint8_t *blob, size_t size)
{
    anem_pack_t pack;
    // magic
    uint8_t saved = blob[0];
    blob[0] = 'X';
    ANEM_CHECK(!anem_pack_open(&pack, blob, size));
    blob[0] = saved;
    // version
    blob[8] = 9;
    ANEM_CHECK(!anem_pack_open(&pack, blob, size));
    blob[8] = 1;
    // total size
    const uint32_t total = (uint32_t)blob[16] | ((uint32_t)blob[17] << 8) |
                           ((uint32_t)blob[18] << 16) | ((uint32_t)blob[19] << 24);
    blob[16] = (uint8_t)(total - 1);
    ANEM_CHECK(!anem_pack_open(&pack, blob, size));
    blob[16] = (uint8_t)total;
    // CRC
    blob[20] ^= 0xFF;
    ANEM_CHECK(!anem_pack_open(&pack, blob, size));
    blob[20] ^= 0xFF;
    // 段偏移越界（SECTION 0 的 offset）
    const uint32_t off_saved = (uint32_t)blob[32] | ((uint32_t)blob[33] << 8) |
                               ((uint32_t)blob[34] << 16) | ((uint32_t)blob[35] << 24);
    const uint32_t bad_off = (uint32_t)size + 8;
    blob[32] = (uint8_t)bad_off;
    blob[33] = (uint8_t)(bad_off >> 8);
    blob[34] = (uint8_t)(bad_off >> 16);
    blob[35] = (uint8_t)(bad_off >> 24);
    ANEM_CHECK(!anem_pack_open(&pack, blob, size));
    blob[32] = (uint8_t)off_saved;
    blob[33] = (uint8_t)(off_saved >> 8);
    blob[34] = (uint8_t)(off_saved >> 16);
    blob[35] = (uint8_t)(off_saved >> 24);
    // 截断
    ANEM_CHECK(!anem_pack_open(&pack, blob, size - 16));
    ANEM_CHECK(anem_pack_open(&pack, blob, size));
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/anemoi_data/anemoi_pack.bin";
    size_t size = 0;
    unsigned char *blob = anem_test_read(path, &size);
    if (blob == NULL) {
        fprintf(stderr, "SKIP: 读不到资源包 %s（先运行 tools/anemoi_pack.py）\n", path);
        return 77;
    }
    int rc = check_real_pack(blob, size);
    if (rc == 0) {
        rc = check_corrupt(blob, size);
    }
    free(blob);
    if (rc == 0) {
        printf("test_anemoi_pack: PASS\n");
    }
    return rc;
}
