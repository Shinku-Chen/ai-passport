// tests/test_cafe_store.c —— NVS 设置/槽位/解锁 往返与坏 blob 拒绝。
#include "cafe_store.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

int main(void)
{
    CHECK(cafe_store_init());
    cafe_settings_t cfg;
    cafe_store_defaults(&cfg);
    cfg.text_speed = 3;
    cfg.font_large = true;
    cfg.panel_opacity = 45;
    cfg.title_bg = 7;
    CHECK(cafe_store_save_settings(&cfg));
    cafe_settings_t got;
    memset(&got, 0, sizeof(got));
    CHECK(cafe_store_load_settings(&got));
    CHECK(got.text_speed == 3 && got.font_large && got.panel_opacity == 45 && got.title_bg == 7);

    cafe_save_t s;
    memset(&s, 0, sizeof(s));
    s.occupied = true;
    s.node = 7;
    s.index = 0;
    s.progress = (1u << CAFE_PROG_KANNA);
    s.progression = 0;
    s.last_complete = 0xFF;
    s.flags[3] = 2;
    snprintf(s.chapter, sizeof(s.chapter), "chapter 1-1");
    CHECK(cafe_store_save_slot(0, &s));
    cafe_save_t out;
    memset(&out, 0, sizeof(out));
    CHECK(cafe_store_load_slot(0, &out));
    CHECK(out.node == 7 && out.index == 0 && out.progress == s.progress && out.flags[3] == 2);
    CHECK(strcmp(out.chapter, s.chapter) == 0);

    cafe_unlock_t u;
    memset(&u, 0, sizeof(u));
    u.progress = (1u << CAFE_PROG_KANNA) | (1u << CAFE_PROG_KANNA_AFTER);
    u.cg_seen[0] = 0x81;
    u.last_cleared = CAFE_PROG_KANNA;
    CHECK(cafe_store_save_unlock(&u));
    cafe_unlock_t u2;
    memset(&u2, 0, sizeof(u2));
    CHECK(cafe_store_load_unlock(&u2));
    CHECK(u2.progress == u.progress && u2.cg_seen[0] == 0x81 && u2.last_cleared == CAFE_PROG_KANNA);

    // 坏 blob：正确 magic/version/size 但 CRC 错误，读取必须失败且不污染输出。
    nvs_handle_t h;
    CHECK(nvs_open("cafe", NVS_READWRITE, &h) == ESP_OK);
    unsigned char junk[204];
    memset(junk, 0, sizeof(junk));
    junk[0] = 'C'; junk[1] = 'A'; junk[2] = 'F'; junk[3] = 'E';
    junk[4] = 1; junk[5] = 0;
    junk[6] = (unsigned char)(sizeof(junk) & 0xFF);
    junk[7] = (unsigned char)(sizeof(junk) >> 8);
    junk[100] = 0x5A;
    CHECK(nvs_set_blob(h, "s0", junk, sizeof(junk)) == ESP_OK);
    CHECK(nvs_commit(h) == ESP_OK);
    nvs_close(h);
    cafe_save_t untouched;
    memset(&untouched, 0xAA, sizeof(untouched));
    CHECK(!cafe_store_load_slot(0, &untouched));
    CHECK(untouched.node == 0xAAAA && untouched.flags[0] == 0xAA);

    // 长度/魔数错误同样拒绝。
    unsigned char tiny[8] = { 0 };
    CHECK(nvs_open("cafe", NVS_READWRITE, &h) == ESP_OK);
    CHECK(nvs_set_blob(h, "prog", tiny, sizeof(tiny)) == ESP_OK);
    nvs_close(h);
    cafe_save_t auto_out;
    memset(&auto_out, 0x55, sizeof(auto_out));
    CHECK(!cafe_store_load_auto(&auto_out));
    CHECK(auto_out.node == 0x5555);

    printf("cafe store PASS\n");
    return 0;
}
