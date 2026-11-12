// Real script regression: normal choices from New Game must reach six heroines.
#include "senren_model.h"
#include "senren_afterstory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; \
} } while (0)

static const senren_layout_t layout = {26, 5};
static const uint32_t flag_pages[] = {109, 5504, 5912, 5926, 6390, 7337, 8418, 51359};

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void test_routes(const senren_scn_t *scn)
{
    static const struct {
        const char *ending;
        uint8_t values[8];
    } routes[] = {
        {"gameend_YoshinoEnd",  {1, 2, 1, 1, 1, 1, 1, 1}},
        {"gameend_MakoEnd",     {1, 2, 2, 1, 1, 1, 1, 1}},
        {"gameend_MurasameEnd", {1, 2, 3, 1, 2, 1, 1, 1}},
        {"gameend_RenaEnd",     {1, 1, 1, 2, 1, 1, 1, 1}},
        {"gameend_KoharuEnd",   {1, 2, 1, 1, 1, 2, 2, 1}},
        {"gameend_RokaEnd",     {1, 2, 1, 1, 1, 2, 2, 2}},
    };
    int flag_index[8];
    for (size_t i = 0; i < 8; i++) {
        flag_index[i] = -1;
        for (uint32_t j = 0; j < scn->flag_count; j++) {
            if (read_u32(scn->flag_pages + 4u * j) == flag_pages[i]) {
                flag_index[i] = (int)j;
            }
        }
        CHECK(flag_index[i] >= 0);
    }
    for (size_t route = 0; route < sizeof(routes) / sizeof(routes[0]); route++) {
        senren_player_t player = {0};
        CHECK(senren_player_start(&player, scn, 0, 0, 0, &layout));
        bool ended = false;
        unsigned choices = 0;
        for (unsigned guard = 0; guard < 300000; guard++) {
            senren_step_t step = senren_player_advance(&player, scn, &layout);
            if (step == SENREN_STEP_CHOICE) {
                uint8_t pick = 0;
                for (uint8_t option = 0; option < player.choice_count; option++) {
                    for (size_t flag = 0; flag < 8; flag++) {
                        if (player.choice_flag[option] == flag_index[flag] &&
                            player.choice_value[option] == routes[route].values[flag]) {
                            pick = option;
                        }
                    }
                }
                if (!senren_player_choose(&player, scn, pick, &layout)) {
                    CHECK(false);
                    break;
                }
                choices++;
            } else if (step == SENREN_STEP_ENDING) {
                char ending[SENREN_NAME_MAX];
                senren_scn_ending(scn, player.ending_index, ending, sizeof(ending));
                CHECK(strcmp(ending, routes[route].ending) == 0);
                printf("route %s -> %s, choices=%u, chunk=%u\n",
                       routes[route].ending, ending, choices, player.chunk);
                ended = true;
                break;
            } else if (step == SENREN_STEP_STUCK) {
                fprintf(stderr, "%s: %s\n", routes[route].ending, senren_get_error());
                CHECK(false);
                break;
            }
        }
        CHECK(ended && choices > 0);
        senren_player_release(&player);
    }
}

// Optional independent source oracle: zero-based select/option and label
// positions generated from JSON, never from the pack parser under test.
static void test_choice_targets(const senren_scn_t *scn, const char *path)
{
    FILE *file = fopen(path, "r");
    CHECK(file != NULL);
    if (!file) return;
    unsigned chunk, node, option, target_chunk, target_node, count = 0;
    int fields;
    while ((fields = fscanf(file, "%u %u %u %u %u", &chunk, &node, &option,
                            &target_chunk, &target_node)) == 5) {
        senren_player_t player = {0};
        CHECK(senren_player_start(&player, scn, 0, (uint16_t)chunk, node, &layout));
        CHECK(senren_player_advance(&player, scn, &layout) == SENREN_STEP_CHOICE);
        CHECK(senren_player_choose(&player, scn, (uint8_t)option, &layout));
        CHECK(player.chunk == target_chunk && player.node == target_node);
        senren_player_release(&player);
        count++;
    }
    CHECK(fields == EOF && count > 0);
    fclose(file);
    printf("source choice targets: %u checked\n", count);
}

// Walk one afterstory from its source AFTER_STORIES (scn, from) entry.
// flip_ordinal < 0 keeps option 0 everywhere; otherwise choose option 1 at
// that SELECT ordinal. Every branch has to reach the generic gameend_end.
static bool walk_afterstory(const senren_scn_t *scn, const senren_afterstory_t *story,
                            int flip_ordinal, unsigned *selects)
{
    senren_player_t player = {0};
    CHECK(senren_player_start(&player, scn, 0, story->chunk, story->node, &layout));
    unsigned ordinal = 0;
    for (unsigned guard = 0; guard < 400000; guard++) {
        senren_step_t step = senren_player_advance(&player, scn, &layout);
        if (step == SENREN_STEP_CHOICE) {
            if (player.choice_count < 2) {
                CHECK(false);
                senren_player_release(&player);
                return false;
            }
            const uint8_t pick = flip_ordinal == (int)ordinal ? 1 : 0;
            if (!senren_player_choose(&player, scn, pick, &layout)) {
                CHECK(false);
                senren_player_release(&player);
                return false;
            }
            ++ordinal;
        } else if (step == SENREN_STEP_ENDING) {
            char ending[SENREN_NAME_MAX];
            senren_scn_ending(scn, player.ending_index, ending, sizeof(ending));
            *selects = ordinal;
            senren_player_release(&player);
            return strcmp(ending, "gameend_end") == 0;
        } else if (step == SENREN_STEP_STUCK) {
            fprintf(stderr, "afterstory %s: %s\n", story->name, senren_get_error());
            CHECK(false);
            senren_player_release(&player);
            return false;
        }
    }
    CHECK(false);
    senren_player_release(&player);
    return false;
}

static void test_afterstories(const senren_scn_t *scn)
{
    CHECK(senren_afterstory_count() == 6);
    for (int i = 0; i < senren_afterstory_count(); ++i) {
        const senren_afterstory_t *story = senren_afterstory_get(i);
        CHECK(story != NULL);
        if (!story) continue;
        unsigned selects = 0;
        CHECK(walk_afterstory(scn, story, -1, &selects));
        CHECK(selects == story->selects);
        for (unsigned ordinal = 0; ordinal < selects; ++ordinal) {
            unsigned branch_selects = 0;
            CHECK(walk_afterstory(scn, story, (int)ordinal, &branch_selects));
            CHECK(branch_selects == selects);
        }
        printf("afterstory %s: %u selects, %u option branches -> gameend_end\n",
               story->name, selects, selects * 2);
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main/senren_data/senren_scn.bin";
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "Cannot open script pack: %s\n", path); return 2; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 2; }
    long length = ftell(file);
    rewind(file);
    uint8_t *data = length > 0 ? malloc((size_t)length) : NULL;
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        fclose(file); free(data); return 2;
    }
    fclose(file);
    senren_scn_t scn;
    if (!senren_scn_open(&scn, data, (uint32_t)length)) { free(data); return 2; }
    test_routes(&scn);
    test_afterstories(&scn);
    if (argc > 2) test_choice_targets(&scn, argv[2]);
    free(data);
    printf("senren_routes: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
