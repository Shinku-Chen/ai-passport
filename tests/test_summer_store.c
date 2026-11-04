// tests/test_summer_store.c —— NVS 存档/设置/自动档，含写入失败与不可用降级。
#include "summer_store.h"

#include "nvs.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

int main(void)
{
    stub_nvs_reset();
    summer_store_t store;
    CHECK(summer_store_init(&store), "init");
    CHECK(store.available, "available");
    CHECK(store.text_speed == 30, "default speed %u", store.text_speed);
    store.text_speed = 0;
    store.auto_speed = 2;
    store.panel_opacity = 4;
    CHECK(summer_store_save_settings(&store), "save settings");

    summer_store_t again;
    CHECK(summer_store_init(&again), "reinit");
    CHECK(again.text_speed == 0, "speed reload %u", again.text_speed);
    CHECK(again.auto_speed == 2, "auto reload %u", again.auto_speed);
    CHECK(again.panel_opacity == 4, "opacity reload %u", again.panel_opacity);

    again.progress.cleared = 0x0005;
    again.progress.restart_stage = 3;
    again.progress.globals[0] = 7;
    again.progress.dirty = true;
    CHECK(summer_store_flush_progress(&again), "progress flush");
    CHECK(!again.progress.dirty, "progress dirty cleared");
    summer_store_t third;
    CHECK(summer_store_init(&third), "third init");
    CHECK(third.progress.cleared == 0x0005, "cleared reload %u", third.progress.cleared);
    CHECK(third.progress.restart_stage == 3, "stage reload");
    CHECK(third.progress.globals[0] == 7, "globals reload");

    const uint8_t payload[4] = { 1, 2, 3, 4 };
    CHECK(summer_store_write_slot(&store, 0, payload, sizeof(payload)), "slot write");
    uint8_t out[8];
    size_t len = 0;
    CHECK(summer_store_read_slot(&store, 0, out, sizeof(out), &len), "slot read");
    CHECK(len == 4 && memcmp(out, payload, 4) == 0, "slot payload");
    CHECK(summer_store_write_auto(&store, payload, sizeof(payload)), "auto write");
    CHECK(summer_store_read_auto(&store, out, sizeof(out), &len), "auto read");
    CHECK(len == 4 && memcmp(out, payload, 4) == 0, "auto payload");

    // Commit failure must not report success or create the key.
    stub_nvs_set_fail_commit(1);
    CHECK(!summer_store_write_slot(&store, 1, payload, sizeof(payload)), "commit fail accepted");
    CHECK(!summer_store_slot_exists(&store, 1), "commit fail created slot");
    stub_nvs_set_fail_commit(0);

    // nvs_set_blob failure.
    stub_nvs_set_fail_set(1);
    CHECK(!summer_store_write_slot(&store, 2, payload, sizeof(payload)), "set fail accepted");
    CHECK(!summer_store_write_auto(&store, payload, sizeof(payload)), "auto set fail accepted");
    stub_nvs_set_fail_set(0);

    // NVS unavailable: degrade, never erase; writes fail, reads fail.
    stub_nvs_set_available(0);
    summer_store_t down;
    CHECK(!summer_store_init(&down), "unavailable init reported true");
    CHECK(!down.available, "unavailable flag");
    CHECK(!summer_store_write_slot(&down, 0, payload, sizeof(payload)), "unavailable write");
    CHECK(!summer_store_read_slot(&down, 0, out, sizeof(out), &len), "unavailable read");
    CHECK(!summer_store_write_auto(&down, payload, sizeof(payload)), "unavailable auto");
    stub_nvs_set_available(1);

    // Restored fixture: the earlier slots/settings/progress still exist.
    summer_store_t restored;
    CHECK(summer_store_init(&restored), "restored init");
    CHECK(restored.available, "restored available");
    CHECK(summer_store_read_slot(&restored, 0, out, sizeof(out), &len), "restored slot");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_store: PASS\n");
    return 0;
}
