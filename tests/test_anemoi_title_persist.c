// Tracked host regression: app END -> progress flush -> reload -> title image.
// This is a state-path test with minimal script variables, not a natural-route
// completion claim.
#include "anemoi_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char s_storage[48][4608];
static size_t s_length[48];
static char s_keys[48][32];
static int s_count;
static int s_fail_write;

static bool fake_read(const char *key, void *out, size_t size, size_t *out_size)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_keys[i], key) == 0 && s_length[i] == size) {
            memcpy(out, s_storage[i], size);
            if (out_size) *out_size = size;
            return true;
        }
    }
    return false;
}

static bool fake_write(const char *key, const void *data, size_t size)
{
    if (s_fail_write) return false;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_keys[i], key) == 0) {
            memcpy(s_storage[i], data, size);
            s_length[i] = size;
            return true;
        }
    }
    if (s_count >= 48) return false;
    snprintf(s_keys[s_count], sizeof(s_keys[s_count]), "%s", key);
    memcpy(s_storage[s_count], data, size);
    s_length[s_count] = size;
    s_count++;
    return true;
}

static const anem_store_io_t s_io = { .read = fake_read, .write = fake_write };

static void reset_store(void)
{
    memset(s_storage, 0, sizeof(s_storage));
    memset(s_length, 0, sizeof(s_length));
    memset(s_keys, 0, sizeof(s_keys));
    s_count = 0;
    s_fail_write = 0;
}

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return b;
}

static int var_index(const anem_app_t *app, const char *name)
{
    for (uint16_t i = 0; i < app->script.st.var_count; i++) {
        const char *got = anem_var_name(&app->pack, i);
        if (got && strcmp(got, name) == 0) return (int)i;
    }
    return -1;
}

static uint32_t end_op(const anem_pack_t *pack)
{
    for (uint32_t i = 0; i < anem_pack_op_count(pack); i++) {
        anem_op_t op;
        if (anem_pack_op(pack, i, &op) && op.kind == ANEM_OP_END) return i;
    }
    return UINT32_MAX;
}

static int set_level(anem_app_t *app, int raw)
{
    const int idx = var_index(app, "g:30");
    if (idx < 0) return -1;
    app->script.st.vars[idx].num = raw;
    app->script.st.vars[idx].kind = ANEM_VAL_NUM;
    app->script.st.vars[idx].text = 0;
    app->script.st.vars[idx].len = 0;
    return 0;
}

static int init_app(anem_app_t *app, unsigned char *blob, size_t size)
{
    if (!anem_app_init(app, blob, size)) return -1;
    if (!anem_store_workspace_init()) return -1;
    anem_store_bind(&s_io);
    anem_app_refresh_store(app, 1000);
    app->progress_ready = true;
    app->progress_dirty = true;
    return 0;
}

static int end_to_title(anem_app_t *app, int raw_level, int old_level,
                        int want_level, uint16_t want_image)
{
    const uint32_t end = end_op(&app->pack);
    if (end == UINT32_MAX || end == 0) return -1;
    if (set_level(app, raw_level) != 0) return -1;
    app->progress.level = old_level;
    app->progress_dirty = true;
    // Put the script immediately before END and make the reader page ready.
    app->script.st.cursor = end - 1;
    app->screen = ANEM_SCR_READ;
    app->typed_chars = 1;
    app->line_chars = 1;
    app->pages = 1;
    app->page = 0;
    anem_app_key(app, ANEM_KEY_UP, ANEM_EV_CLICK, 1100);
    if (app->screen != ANEM_SCR_END) return -1;
    anem_progress_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    if (!anem_store_load_progress(&loaded)) return -1;
    if (loaded.level != want_level || app->progress.level != want_level) return -1;
    // Simulate a fresh app reload before the END OK -> title transition.
    app->progress = loaded;
    app->progress_ready = true;
    app->progress_dirty = false;
    app->sel[ANEM_SCR_END] = 0;
    anem_app_key(app, ANEM_KEY_OK, ANEM_EV_PRESS, 1200);
    anem_app_key(app, ANEM_KEY_OK, ANEM_EV_RELEASE, 1240);
    anem_app_key(app, ANEM_KEY_OK, ANEM_EV_CLICK, 1250);
    return app->title_level == want_level && app->title_image == want_image ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: test_anemoi_title_persist pack.bin");
        return 2;
    }
    size_t size = 0;
    unsigned char *blob = read_file(argv[1], &size);
    if (!blob) return 2;
    int fail = 0;

    // level 0 / normal.
    reset_store();
    anem_app_t app;
    if (init_app(&app, blob, size) != 0) return 2;
    if (end_to_title(&app, 0, 0, 0, app.title_image_normal) != 0) {
        puts("FAIL level0 normal END->title");
        fail++;
    }

    // raw level 1 maps to source level 2 / eternicle.
    reset_store();
    if (init_app(&app, blob, size) != 0) return 2;
    if (end_to_title(&app, 1, 0, 2, app.title_image_eternicle) != 0) {
        puts("FAIL raw level1->2 eternicle END->title");
        fail++;
    }

    // level 2 / eternicle.
    reset_store();
    if (init_app(&app, blob, size) != 0) return 2;
    if (end_to_title(&app, 2, 0, 2, app.title_image_eternicle) != 0) {
        puts("FAIL level2 eternicle END->title");
        fail++;
    }

    // level 4 / anemoi, with the pre-existing stale level 0 that triggered
    // the production bug.
    reset_store();
    if (init_app(&app, blob, size) != 0) return 2;
    if (end_to_title(&app, 3, 0, 4, app.title_image_anemoi) != 0) {
        puts("FAIL level4 anemoi END->title");
        fail++;
    }

    // Existing legal history must survive a NewGame/Continue-shaped script
    // with no level variables.
    reset_store();
    if (init_app(&app, blob, size) != 0) return 2;
    if (end_to_title(&app, 0, 4, 4, app.title_image_anemoi) != 0) {
        puts("FAIL existing level4 history overwritten by level0 script");
        fail++;
    }

    // Write failure: no persistence, dirty remains set, existing record is not
    // corrupted; a retry succeeds and persists the END-earned level.
    reset_store();
    if (init_app(&app, blob, size) != 0) return 2;
    if (set_level(&app, 2) != 0) return 2;
    app.progress.level = 0;
    app.progress_dirty = true;
    if (!anem_app_flush_progress(&app)) {
        puts("FAIL initial level2 flush");
        fail++;
    } else {
        anem_progress_t old;
        memset(&old, 0, sizeof(old));
        if (!anem_store_load_progress(&old) || old.level != 2) {
            puts("FAIL initial level2 reload");
            fail++;
        }
        if (set_level(&app, 3) != 0) return 2;
        app.progress.level = 0;
        app.progress_dirty = true;
        s_fail_write = 1;
        if (anem_app_flush_progress(&app) || !app.progress_dirty) {
            puts("FAIL write failure contract");
            fail++;
        }
        anem_progress_t kept;
        memset(&kept, 0, sizeof(kept));
        if (!anem_store_load_progress(&kept) || kept.level != 2) {
            puts("FAIL failed write polluted stored progress");
            fail++;
        }
        s_fail_write = 0;
        if (!anem_app_flush_progress(&app) || app.progress_dirty) {
            puts("FAIL write retry");
            fail++;
        }
        anem_progress_t retried;
        memset(&retried, 0, sizeof(retried));
        if (!anem_store_load_progress(&retried) || retried.level != 4) {
            puts("FAIL write retry reload");
            fail++;
        }
    }

    free(blob);
    if (fail == 0) {
        puts("test_anemoi_title_persist: PASS (host END/flush/reload state path; not a natural route claim)");
        return 0;
    }
    printf("test_anemoi_title_persist: FAIL (%d)", fail);
    puts("");
    return 1;
}
