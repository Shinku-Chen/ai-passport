// Store status regression: distinguish empty / I/O error / corrupt / OK and
// prove the test double's commit contract preserves the old committed blob.
// This is a host contract double, not a claim about physical ESP-IDF NVS.
#include "gal_store.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); failures++; } \
} while (0)

int main(void)
{
    nvs_stub_reset();
    gal_store_t store;
    CHECK(gal_store_init(&store), "init");
    uint8_t oldv[64], newv[128];
    for (int i = 0; i < (int)sizeof(oldv); ++i) oldv[i] = (uint8_t)i;
    for (int i = 0; i < (int)sizeof(newv); ++i) newv[i] = (uint8_t)(0xA0 + i);
    CHECK(gal_store_save_slot(&store, 0, oldv, sizeof(oldv)), "seed slot0");

    uint8_t buf[256];
    size_t len = 0;
    CHECK(gal_store_read_slot(&store, 0, buf, sizeof(buf), &len) == GAL_STORE_OK,
          "slot0 status");
    CHECK(len == sizeof(oldv) && memcmp(buf, oldv, len) == 0, "slot0 value");
    CHECK(gal_store_read_slot(&store, 1, buf, sizeof(buf), &len) == GAL_STORE_NOT_FOUND,
          "empty slot status");
    CHECK(!gal_store_load_slot(&store, 1, buf, sizeof(buf), &len),
          "load_slot accepted empty");

    nvs_stub_fail_open(true);
    CHECK(gal_store_read_slot(&store, 0, buf, sizeof(buf), &len) == GAL_STORE_IO_ERROR,
          "open failure status");
    CHECK(!gal_store_load_slot(&store, 0, buf, sizeof(buf), &len),
          "load_slot accepted open failure");
    nvs_stub_fail_open(false);

    nvs_stub_fail_get(true);
    CHECK(gal_store_read_slot(&store, 0, buf, sizeof(buf), &len) == GAL_STORE_IO_ERROR,
          "get failure status");
    nvs_stub_fail_get(false);

    // Commit failure must leave the old committed blob visible.
    nvs_stub_fail_commit(true);
    CHECK(!gal_store_save_slot(&store, 0, newv, sizeof(newv)), "commit failure accepted");
    nvs_stub_fail_commit(false);
    len = 0;
    CHECK(gal_store_read_slot(&store, 0, buf, sizeof(buf), &len) == GAL_STORE_OK,
          "post-failure status");
    CHECK(len == sizeof(oldv) && memcmp(buf, oldv, len) == 0,
          "commit failure replaced old blob (len=%zu)", len);

    // Set failure also leaves old blob.
    nvs_stub_fail_set(true);
    CHECK(!gal_store_save_slot(&store, 0, newv, sizeof(newv)), "set failure accepted");
    nvs_stub_fail_set(false);
    len = 0;
    CHECK(gal_store_read_slot(&store, 0, buf, sizeof(buf), &len) == GAL_STORE_OK &&
          len == sizeof(oldv) && memcmp(buf, oldv, len) == 0,
          "set failure replaced old blob");

    // Zero-length committed record is corrupt, not empty.
    nvs_handle_t h;
    CHECK(nvs_open("manosaba", NVS_READWRITE, &h) == ESP_OK, "nvs open corrupt");
    CHECK(nvs_set_blob(h, "s02", "", 0) == ESP_OK, "write zero blob");
    CHECK(nvs_commit(h) == ESP_OK, "commit zero blob");
    nvs_close(h);
    CHECK(gal_store_read_slot(&store, 2, buf, sizeof(buf), &len) == GAL_STORE_CORRUPT,
          "zero-length blob not corrupt");

    // Init failure is an I/O error, not "empty".
    nvs_stub_fail_open(true);
    CHECK(!gal_store_init(&store), "init unexpectedly succeeded");
    CHECK(gal_store_read_slot(&store, 0, buf, sizeof(buf), &len) == GAL_STORE_IO_ERROR,
          "init failure status");
    nvs_stub_fail_open(false);
    CHECK(gal_store_init(&store), "reinit");

    printf("test_gal_store_status: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
