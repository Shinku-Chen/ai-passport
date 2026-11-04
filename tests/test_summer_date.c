// tests/test_summer_date.c — strings[106] display dates and the real chapter 137
// fallback (scene 177 inherits 8月30日 from scene 176).
#include "summer_model.h"

#include <stdio.h>
#include <stdlib.h>
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

#define MON "\xE6\x9C\x88"
#define DAY "\xE6\x97\xA5"

static const char d_full_8_30[] = "\xEF\xBC\x98" MON "\xEF\xBC\x93\xEF\xBC\x90" DAY;
static const char d_full_12_31[] = "\xEF\xBC\x91\xEF\xBC\x92" MON
                                    "\xEF\xBC\x93\xEF\xBC\x91" DAY;
static const char d_ascii_8_30[] = "8" MON "30" DAY;
static const char d_ascii_8_3[] = "8" MON "3" DAY;
static const char d_ascii_12_31[] = "12" MON "31" DAY;
static const char d_bad_tail[] = "8" MON "30" DAY "x";
static const char d_short_utf8[] = "\xE6\x97";
static const char d_truncated[] = "8" MON "30";
static const char d_over_month[] = "123" MON "1" DAY;
static const char d_over_day[] = "8" MON "123" DAY;
static const char d_zero_month[] = "0" MON "1" DAY;
static const char d_zero_day[] = "8" MON "0" DAY;

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return p;
}

static void test_converter(void)
{
    char out[32];
    CHECK(summer_date_to_ascii(d_full_8_30, out, sizeof(out)), "full 8/30 rejected");
    CHECK(strcmp(out, d_ascii_8_30) == 0, "full 8/30 -> [%s]", out);
    CHECK(summer_date_to_ascii(d_full_12_31, out, sizeof(out)), "full 12/31 rejected");
    CHECK(strcmp(out, d_ascii_12_31) == 0, "full 12/31 -> [%s]", out);
    CHECK(summer_date_to_ascii(d_ascii_8_30, out, sizeof(out)), "ascii 8/30 rejected");
    CHECK(strcmp(out, d_ascii_8_30) == 0, "ascii 8/30 -> [%s]", out);
    CHECK(summer_date_to_ascii(d_ascii_8_3, out, sizeof(out)), "single day rejected");
    CHECK(strcmp(out, d_ascii_8_3) == 0, "single day -> [%s]", out);
    CHECK(!summer_date_to_ascii("", out, sizeof(out)), "empty date accepted");
    CHECK(!summer_date_to_ascii(d_bad_tail, out, sizeof(out)), "trailing byte accepted");
    CHECK(!summer_date_to_ascii(d_short_utf8, out, sizeof(out)), "short UTF-8 accepted");
    CHECK(!summer_date_to_ascii(d_truncated, out, sizeof(out)), "missing 日 accepted");
    CHECK(!summer_date_to_ascii(d_over_month, out, sizeof(out)), "3-digit month accepted");
    CHECK(!summer_date_to_ascii(d_over_day, out, sizeof(out)), "3-digit day accepted");
    CHECK(!summer_date_to_ascii(d_zero_month, out, sizeof(out)), "month 0 accepted");
    CHECK(!summer_date_to_ascii(d_zero_day, out, sizeof(out)), "day 0 accepted");

    // A small destination must fail without writing past cap; the final byte
    // is still NUL when the function returns false.
    char small[8];
    memset(small, 'X', sizeof(small));
    CHECK(!summer_date_to_ascii(d_full_12_31, small, sizeof(small)), "cap truncation accepted");
    CHECK(small[sizeof(small) - 1] == 0, "cap truncation left no NUL at end");
    char enough[10];
    CHECK(summer_date_to_ascii(d_ascii_8_30, enough, sizeof(enough)), "8/30 fits 10 bytes");
    CHECK(strcmp(enough, d_ascii_8_30) == 0, "10-byte 8/30 -> [%s]", enough);
}

static void test_chapter137(const summer_pack_t *pack)
{
    summer_vm_t vm;
    CHECK(summer_vm_start_chapter(&vm, pack, 137), "chapter 137 start");
    int saw = 0;
    for (int guard = 0; guard < 200000; ++guard) {
        if (vm.scene_id == 177 && !saw) {
            char label[SUMMER_TEXT_CAP];
            snprintf(label, sizeof(label), "%s", summer_vm_scene_label(&vm, pack));
            CHECK(strcmp(vm.date, d_full_8_30) == 0, "scene 177 date [%s]", vm.date);
            CHECK(strcmp(label, d_ascii_8_30) == 0, "scene 177 label [%s]", label);
            saw = 1;
        }
        const summer_step_t st = summer_vm_step(&vm, pack, 256);
        if (st.kind == SUMMER_STEP_TEXT) {
            if (!summer_vm_advance(&vm, pack)) break;
        } else if (st.kind == SUMMER_STEP_CHOICE) {
            int items[32];
            const int n = summer_vm_visible_choices(&vm, pack, st.group, items, 32);
            if (n <= 0 || !summer_vm_choose(&vm, pack, st.group, items[0])) break;
        } else if (st.kind == SUMMER_STEP_MAP) {
            break;
        } else if (st.kind == SUMMER_STEP_END || st.kind == SUMMER_STEP_ERROR) {
            break;
        }
    }
    CHECK(saw, "chapter 137 never reached scene 177");
}

// Walk all chapter entries and count inherited-date scene visits.  The source
// review hit 10 scenes / 117 visits; every one must convert the raw date, never
// fall back to the hard-coded 7月29日 label.
static void test_datewalk(const summer_pack_t *pack)
{
    int dated_scenes = 0, dated_visits = 0;
    int inherited_scenes = 0, inherited_visits = 0;
    static bool seen[1024];
    for (uint32_t i = 0; i < pack->chapter_count; ++i) {
        summer_vm_t vm;
        if (!summer_vm_start_chapter(&vm, pack, pack->chapters[i].scene)) continue;
        uint16_t last_scene = 0;
        for (int guard = 0; guard < 20000; ++guard) {
            if (vm.scene_id != last_scene) {
                last_scene = vm.scene_id;
                const summer_scene_t *scene = summer_scene_by_id(pack, last_scene);
                char own[24] = "";
                const bool own_date = scene && scene->date != 0 &&
                    summer_pack_text_into((summer_pack_t *)pack, scene->date, own, sizeof(own)) &&
                    own[0];
                if (scene && scene->phase == 0 && vm.date[0]) {
                    char label[SUMMER_TEXT_CAP];
                    snprintf(label, sizeof(label), "%s", summer_vm_scene_label(&vm, pack));
                    char expected[32];
                    if (!summer_date_to_ascii(vm.date, expected, sizeof(expected))) {
                        CHECK(false, "date for scene %u not canonical [%s]",
                              last_scene, vm.date);
                    } else {
                        CHECK(strcmp(label, expected) == 0,
                              "scene %u label [%s] expected [%s]",
                              last_scene, label, expected);
                    }
                    dated_visits++;
                    if (!own_date) inherited_visits++;
                    if (last_scene < sizeof(seen) && !seen[last_scene]) {
                        seen[last_scene] = true;
                        dated_scenes++;
                        if (!own_date) inherited_scenes++;
                    }
                }
            }
            const summer_step_t st = summer_vm_step(&vm, pack, 256);
            if (st.kind == SUMMER_STEP_TEXT) {
                if (!summer_vm_advance(&vm, pack)) break;
            } else if (st.kind == SUMMER_STEP_CHOICE) {
                int items[32];
                const int n = summer_vm_visible_choices(&vm, pack, st.group, items, 32);
                if (n <= 0 || !summer_vm_choose(&vm, pack, st.group, items[0])) break;
            } else if (st.kind == SUMMER_STEP_MAP) {
                break;
            } else if (st.kind == SUMMER_STEP_END || st.kind == SUMMER_STEP_ERROR) {
                break;
            }
        }
    }
    printf("datewalk: dated_scenes=%d dated_visits=%d inherited_scenes=%d inherited_visits=%d",
           dated_scenes, dated_visits, inherited_scenes, inherited_visits);
    putchar(10);
    CHECK(dated_scenes > 0 && dated_visits > 0, "datewalk found no dated scenes");
    CHECK(inherited_scenes > 0 && inherited_visits > 0, "datewalk found no inherited dates");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/summer_data/summer_pack.bin";
    size_t size = 0;
    uint8_t *blob = read_all(path, &size);
    if (!blob) {
        fprintf(stderr, "SKIP: cannot read %s\n", path);
        return 77;
    }
    summer_pack_t pack;
    CHECK(summer_pack_open(&pack, blob, (uint32_t)size, NULL), "pack");
    test_converter();
    test_chapter137(&pack);
    test_datewalk(&pack);
    free(blob);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_summer_date: PASS\n");
    return 0;
}
