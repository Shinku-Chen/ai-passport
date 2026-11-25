// Synthetic graph test for the parts the real sorairo data does not exercise:
// conditional choices, all-conditions, first-match nexts, + effects and the
// empty-filter fallback (visibleChoices returns every option).
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
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)length);
    assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    size_t size = 0;
    uint8_t *data = load(argv[1], &size);
    nine_pack_t pack;
    assert(nine_pack_open(&pack, data, size));
    assert(pack.node_count == 5);
    assert(pack.key_count == 4);
    assert(nine_pack_find_key(&pack, "C") == 2);

    nine_vm_t vm;
    assert(nine_vm_init(&vm, &pack));
    assert(nine_vm_start(&vm, NINE_NODE_NONE, false));
    const nine_view_t *view = nine_vm_locate(&vm);
    assert(view->kind == NINE_VM_TEXT);
    assert(strstr(nine_pack_text(&pack, &(nine_frame_t){ .kind = 0,
        .text_block = 0, .text_offset = 0, .text_length = 21 }), "条件") != NULL);

    // n0 has two nexts; the first condition is false, the second (all) wins.
    assert(nine_vm_advance(&vm)->kind == NINE_VM_CHOICE);
    view = nine_vm_locate(&vm);
    assert(view->choice_count == 2);   // hidden option filtered out
    nine_edge_t edge;
    assert(nine_pack_edge(&pack, true, view->choices[0], &edge));
    assert(strcmp(nine_pack_string(&pack, edge.label), "条件成立的选项") == 0);
    assert(nine_pack_edge(&pack, true, view->choices[1], &edge));
    assert(strcmp(nine_pack_string(&pack, edge.label), "无条件选项") == 0);

    // n1 effects run before the choice: C += 2, D = 1. The chosen option adds
    // B = 2 and C += 1.
    assert(nine_vm_choose(&vm, 0)->kind == NINE_VM_TEXT);
    assert(vm.state.flag[0] == 0);
    assert(vm.state.flag[1] == 2);
    assert(vm.state.flag[2] == 0);
    assert(vm.state.flag[3] == 1);
    assert(vm.state.progress[2] == 3);
    assert(vm.state.progress[3] == 0);

    assert(nine_vm_advance(&vm)->kind == NINE_VM_CHOICE);
    view = nine_vm_locate(&vm);
    assert(view->choice_count == 3);   // all filtered -> fallback to all
    for (uint8_t i = 0; i < view->choice_count; ++i) {
        assert(nine_pack_edge(&pack, true, view->choices[i], &edge));
        assert(strcmp(nine_pack_string(&pack, edge.label), "回退选项") == 0);
    }
    assert(nine_vm_choose(&vm, 2)->kind == NINE_VM_TEXT);
    assert(nine_vm_advance(&vm)->kind == NINE_VM_END);
    assert(vm.state.flag[3] == 1);

    // Save/load keeps the conditional state and rejects corruption.
    uint8_t saved[NINE_SAVE_CAPACITY];
    const size_t saved_size = nine_vm_save_encode(&vm, 0, saved, sizeof(saved));
    assert(saved_size > 0);
    nine_vm_t loaded;
    assert(nine_vm_init(&loaded, &pack));
    uint16_t page = 0;
    assert(nine_vm_save_decode(&loaded, saved, saved_size, &page));
    assert(loaded.state.flag[1] == 2 && loaded.state.progress[2] == 3);
    saved[20] ^= 1;
    assert(!nine_vm_save_peek(&pack, saved, saved_size, NULL, NULL, NULL, NULL));

    nine_vm_deinit(&loaded);
    nine_vm_deinit(&vm);
    free(data);
    puts("nine conditional choice/effect semantics: PASS");
    return 0;
}
