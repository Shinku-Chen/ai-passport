// tests/test_gal_store.c —— 存档/设置的持久化测试(内存 NVS + 故障注入)。
//
// 覆盖:默认值、存档往返(位置/变量/解锁/预览)、版本与 CRC 拒绝、commit 失败必须
// 判为保存失败、擦除、任意槽位探测、以及"在选项处存档后恢复仍停在选项"。
#include "gal_store.h"

#include <stdio.h>
#include <string.h>

void nvs_stub_reset(void);
void nvs_stub_fail_open(int on);
void nvs_stub_fail_set(int on);
void nvs_stub_fail_commit(int on);
int nvs_stub_count(void);
int nvs_stub_corrupt(const char *ns, const char *key, size_t offset);
int nvs_stub_put(const char *key, const void *data, size_t len);

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static void fill(gal_save_t *s, uint16_t pc, int16_t v0, int16_t v1)
{
    memset(s, 0, sizeof(*s));
    s->pc = pc;
    s->chapter = 3;
    s->bg = 5;
    s->cg = GAL_NONE;
    s->spr[0] = 7;
    s->spr[1] = GAL_NONE;
    s->spr[2] = 9;
    s->vars[0] = v0;
    s->vars[1] = v1;
    gal_unlock_cg_set(&s->unlock, 33);
    gal_unlock_chapter_set(&s->unlock, 17);
    gal_unlock_ending_set(&s->unlock, 2);
    snprintf(s->preview, sizeof(s->preview), "第三幕 · 抉择：测试存档");
}

int main(void)
{
    nvs_stub_reset();
    CHECK(gal_store_init(), "store init");

    // 1) 默认设置合理
    gal_settings_t st;
    gal_settings_defaults(&st);
    CHECK(st.text_speed == 1 && st.font_large == 0 && st.auto_delay == 1 && st.panel_opa == 2,
          "defaults %u/%u/%u/%u", st.text_speed, st.font_large, st.auto_delay, st.panel_opa);
    CHECK(!gal_store_load_settings(&st), "no settings yet");
    gal_settings_t custom = st;
    custom.text_speed = 2;
    custom.font_large = 1;
    custom.panel_opa = 3;
    CHECK(gal_store_save_settings(&custom), "save settings");
    gal_settings_t back;
    CHECK(gal_store_load_settings(&back), "load settings");
    CHECK(back.text_speed == 2 && back.font_large == 1 && back.panel_opa == 3, "settings roundtrip");

    // 2) 存档往返
    gal_save_t save;
    fill(&save, 1234, 7, -3);
    CHECK(!gal_store_load_slot(0, &save), "empty slot must not load");
    CHECK(gal_store_save_slot(0, &save), "save slot 0");
    gal_save_t got;
    CHECK(gal_store_load_slot(0, &got), "load slot 0");
    CHECK(got.pc == 1234, "pc %u", got.pc);
    CHECK(got.bg == 5 && got.cg == GAL_NONE && got.spr[0] == 7 && got.spr[2] == 9, "visuals");
    CHECK(got.vars[0] == 7 && got.vars[1] == -3, "vars %d/%d", got.vars[0], got.vars[1]);
    CHECK(gal_unlock_cg_get(&got.unlock, 33), "cg 33 unlocked");
    CHECK(gal_unlock_chapter_get(&got.unlock, 17), "chapter 17 unlocked");
    CHECK(gal_unlock_ending_get(&got.unlock, 2), "ending 2 unlocked");
    CHECK(strcmp(got.preview, save.preview) == 0, "preview '%s'", got.preview);
    CHECK(gal_store_any_slot(), "any_slot");

    // 3) CRC 损坏必须拒绝
    CHECK(nvs_stub_corrupt("galgod", "s0", 8), "corrupt");
    CHECK(!gal_store_load_slot(0, &got), "corrupted save must be rejected");

    // 4) 版本不符必须拒绝
    fill(&save, 77, 0, 0);
    CHECK(gal_store_save_slot(1, &save), "save slot 1");
    {
        gal_save_t tmp;
        CHECK(gal_store_load_slot(1, &tmp), "load slot 1");
        tmp.version = 99;
        CHECK(nvs_stub_put("s1", &tmp, sizeof(tmp)), "write old-version blob");
        CHECK(!gal_store_load_slot(1, &tmp), "old version must be rejected");
        // 恢复一个可用的槽 1,供后面的"commit 失败不覆盖"用例使用
        fill(&save, 77, 0, 0);
        CHECK(gal_store_save_slot(1, &save), "restore slot 1");
    }

    // 5) commit 失败 = 保存失败
    nvs_stub_fail_commit(1);
    fill(&save, 999, 1, 1);
    CHECK(!gal_store_save_slot(1, &save), "commit failure must fail the save");
    nvs_stub_fail_commit(0);
    // 之前的槽 1 内容仍在(commit 失败不应被当成成功)
    CHECK(gal_store_load_slot(1, &got) && got.pc == 77, "slot 1 keeps old data");

    // 6) 打开失败 = 读写失败但不崩
    nvs_stub_fail_open(1);
    CHECK(!gal_store_save_slot(2, &save), "open failure -> save fails");
    CHECK(!gal_store_load_slot(2, &got), "open failure -> load fails");
    nvs_stub_fail_open(0);

    // 7) 擦除
    CHECK(gal_store_erase_slot(0), "erase slot 0");
    CHECK(!gal_store_load_slot(0, &got), "erased slot must not load");

    // 8) 自动存档槽独立
    fill(&save, 42, 0, 0);
    CHECK(gal_store_save_slot(GAL_SLOT_AUTO, &save), "save auto");
    CHECK(gal_store_load_slot(GAL_SLOT_AUTO, &got) && got.pc == 42, "auto roundtrip");
    CHECK(gal_store_load_slot(1, &got) && got.pc == 77, "manual slot untouched");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("store ok (%d entries)\n", nvs_stub_count());
    return 0;
}
