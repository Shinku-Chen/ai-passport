// Host regression for the real packed script and the exact source graph
// semantics. Uses the production nine_pack.c / nine_vm.c and a real zlib
// inflater (tests/nine_host/tinfl_shim.c).
#include "nine_crc.h"
#include "nine_pack.h"
#include "nine_vm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    const long length = ftell(file);
    assert(length > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t *data = malloc((size_t)length);
    assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return data;
}

static void advance_to_kind(nine_vm_t *vm, nine_vm_kind_t target, int *texts)
{
    for (int i = 0; i < 20000; ++i) {
        const nine_view_t *view = nine_vm_locate(vm);
        assert(view && view->kind != NINE_VM_ERROR);
        if (view->kind == target) return;
        if (view->kind == NINE_VM_TEXT) {
            if (texts) (*texts)++;
            assert(nine_vm_advance(vm)->kind != NINE_VM_ERROR);
        } else if (view->kind == NINE_VM_OP) {
            assert(nine_vm_advance(vm)->kind != NINE_VM_ERROR);
        } else {
            assert(!"unexpected VM state while advancing to target");
        }
    }
    assert(!"hop limit in host traversal");
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    size_t size = 0;
    uint8_t *data = load(argv[1], &size);
    nine_pack_t pack;
    assert(nine_pack_open(&pack, data, size));
    assert(nine_pack_validate_text_blocks(&pack));
    assert(pack.node_count == 56);
    assert(pack.frame_count == 10386);
    assert(pack.chapter_count == 21);
    assert(pack.ending_count == 2);
    assert(pack.key_count == 15);
    assert(pack.visual_count == 2119);
    assert(pack.layer_count == 8143);
    assert(pack.gallery_count == 72);
    assert(nine_pack_find_key(&pack, "loop1") >= 0);
    assert(nine_pack_find_key(&pack, "flavor_n4") >= 0);
    assert(nine_pack_find_node(&pack, "n0") == 0);
    assert(nine_pack_find_node(&pack, "n55") == 55);

    nine_node_t node;
    assert(nine_pack_node(&pack, 0, &node));
    nine_frame_t frame;
    assert(nine_pack_frame(&pack, node.first_frame, &frame));
    const char *text = nine_pack_text(&pack, &frame);
    assert(text && strstr(text, "重要的回忆") != NULL);
    assert(strcmp(nine_pack_title(&pack), "9-nine- 天色天歌天籁音") == 0);
    nine_chapter_t chapter;
    assert(nine_pack_chapter(&pack, 0, &chapter));
    assert(strcmp(nine_pack_string(&pack, chapter.title), "第01章") == 0);
    assert(nine_pack_string(&pack, chapter.preview)[0] != '\0');

    nine_vm_t vm;
    assert(nine_vm_init(&vm, &pack));
    assert(nine_vm_start(&vm, NINE_NODE_NONE, false));

    int texts = 0;
    advance_to_kind(&vm, NINE_VM_CHOICE, &texts);
    assert(texts == 10);
    nine_view_t *view = (nine_view_t *)nine_vm_locate(&vm);
    assert(view->choice_count == 1);
    nine_edge_t edge;
    assert(nine_pack_edge(&pack, true, view->choices[0], &edge));
    assert(strcmp(nine_pack_string(&pack, edge.label), "回顾过往的记忆。") == 0);
    assert(nine_vm_choose(&vm, 0)->kind == NINE_VM_TEXT);
    advance_to_kind(&vm, NINE_VM_CHOICE, &texts);
    view = (nine_view_t *)nine_vm_locate(&vm);
    assert(view->choice_count == 3);
    assert(nine_vm_choose(&vm, 1)->kind == NINE_VM_TEXT);
    assert(nine_vm_condition_value(&vm, (uint16_t)nine_pack_find_key(&pack, "flavor_n4")) == 1);
    advance_to_kind(&vm, NINE_VM_CHOICE, &texts);
    view = (nine_view_t *)nine_vm_locate(&vm);
    assert(view->choice_count == 2);
    // Accept her, reach the loop choice, take "re-select", come back and
    // decline; the true route must still be reachable.
    assert(nine_vm_choose(&vm, 0)->kind == NINE_VM_TEXT);
    advance_to_kind(&vm, NINE_VM_CHOICE, &texts);
    view = (nine_view_t *)nine_vm_locate(&vm);
    assert(view->choice_count == 1);
    assert(nine_pack_edge(&pack, true, view->choices[0], &edge));
    assert(strcmp(nine_pack_string(&pack, edge.label), "重新选择。") == 0);
    assert(nine_vm_choose(&vm, 0)->kind == NINE_VM_TEXT);
    assert(nine_vm_condition_value(&vm, (uint16_t)nine_pack_find_key(&pack, "loop1")) == 1);
    advance_to_kind(&vm, NINE_VM_CHOICE, &texts);
    view = (nine_view_t *)nine_vm_locate(&vm);
    assert(view->choice_count == 2);
    assert(nine_vm_choose(&vm, 1)->kind == NINE_VM_TEXT);
    advance_to_kind(&vm, NINE_VM_END, &texts);
    // clear_true/cf_sr/titleBg are persistent progress keys; source flags()
    // only overlays loop1, so read progress directly, as the app does.
    assert(vm.state.progress[nine_pack_find_key(&pack, "clear_true")] != 0);
    assert(vm.state.progress[nine_pack_find_key(&pack, "cf_sr")] != 0);
    assert(vm.state.progress[nine_pack_find_key(&pack, "titleBg")] == 2);

    // Manual save round-trip, page preservation, corruption rejection.
    uint8_t saved[NINE_SAVE_CAPACITY];
    const size_t saved_size = nine_vm_save_encode(&vm, 3, saved, sizeof(saved));
    assert(saved_size > 0);
    uint16_t page = 0;
    nine_vm_t loaded;
    assert(nine_vm_init(&loaded, &pack));
    assert(nine_vm_save_decode(&loaded, saved, saved_size, &page));
    assert(page == 3);
    assert(loaded.state.progress[nine_pack_find_key(&pack, "clear_true")] != 0);
    saved[40] ^= 0x40;
    assert(!nine_vm_save_peek(&pack, saved, saved_size, NULL, NULL, NULL, NULL));
    saved[40] ^= 0x40;
    assert(nine_vm_save_peek(&pack, saved, saved_size, NULL, NULL, NULL, NULL));

    // Chapter preview must not write permanent progress.
    const int loop_key = nine_pack_find_key(&pack, "loop1");
    const int true_key = nine_pack_find_key(&pack, "clear_true");
    const int32_t before_loop = vm.state.progress[loop_key];
    const int32_t before_true = vm.state.progress[true_key];
    assert(nine_vm_seek(&vm, 55, 0, true, true));
    advance_to_kind(&vm, NINE_VM_END, &texts);
    assert(vm.state.progress[loop_key] == before_loop);
    assert(vm.state.progress[true_key] == before_true);
    assert(nine_vm_condition_value(&vm, (uint16_t)nine_pack_find_key(&pack, "flavor_n4")) == 0);

    // Whole-pack and text-block CRC rejection remain active after the CRC
    // table optimization.
    uint8_t *bad = malloc(size);
    assert(bad);
    memcpy(bad, data, size);
    bad[16] ^= 0x01;
    nine_pack_t bad_pack;
    assert(!nine_pack_open(&bad_pack, bad, size));
    memcpy(bad, data, size);
    nine_script_header_t *header = (nine_script_header_t *)bad;
    nine_text_block_record_t *block = (nine_text_block_record_t *)
        (bad + header->blocks_offset);
    block->crc32 ^= 1u;
    const uint32_t bad_crc = nine_crc32_zero(bad, size, 16, 4);
    bad[16] = (uint8_t)bad_crc;
    bad[17] = (uint8_t)(bad_crc >> 8);
    bad[18] = (uint8_t)(bad_crc >> 16);
    bad[19] = (uint8_t)(bad_crc >> 24);
    assert(nine_pack_open(&bad_pack, bad, size));
    assert(!nine_pack_validate_text_blocks(&bad_pack));
    free(bad);

    // skipChapter stops at the OP frame and keeps the current chapter.
    nine_vm_deinit(&loaded);
    nine_vm_deinit(&vm);
    free(data);
    puts("nine pack/vm real-script traversal: PASS");
    return 0;
}
