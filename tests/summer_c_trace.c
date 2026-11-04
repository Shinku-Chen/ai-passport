// tests/summer_c_trace.c — production C VM trace for tests/summer_source_trace.mjs.
//
// Usage: summer_c_trace <pack> <mode> <choice:first|last> <map:first|last> [scene]
//   story|alka|pocket|chapter|labels
#include "summer_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void esc(const char *s)
{
    for (; s && *s; ++s) {
        if (*s == '\\') fputs("\\\\", stdout);
        else if (*s == '\t') fputs("\\t", stdout);
        else if (*s == '\n') fputs("\\n", stdout);
        else if (*s == '\r') continue;
        else fputc(*s, stdout);
    }
}

static void progress_apply(summer_vm_t *vm, const summer_pack_t *pack,
                           const summer_progress_t *progress)
{
    summer_progress_apply(vm, pack, progress);
}

static void emit_line(const char *tag, unsigned scene, unsigned op, const char *a,
                      const char *b, const char *c, const char *d)
{
    printf("%s\t%u\t%u", tag, scene, op);
    if (a) { fputc('\t', stdout); esc(a); }
    if (b) { fputc('\t', stdout); esc(b); }
    if (c) { fputc('\t', stdout); esc(c); }
    if (d) { fputc('\t', stdout); esc(d); }
    fputc('\n', stdout);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <pack> <mode> <choice> <map> [scene]\n", argv[0]);
        return 2;
    }
    size_t size = 0;
    uint8_t *blob = read_all(argv[1], &size);
    if (!blob) { fprintf(stderr, "cannot read pack\n"); return 2; }
    summer_pack_t pack;
    if (!summer_pack_open(&pack, blob, (uint32_t)size, NULL)) {
        fprintf(stderr, "pack rejected\n");
        return 2;
    }
    const char *mode = argv[2];
    const int choice_last = strcmp(argv[3], "last") == 0;
    const int map_last = strcmp(argv[4], "last") == 0;
    const uint16_t scene_arg = argc > 5 ? (uint16_t)atoi(argv[5]) : 0;
    const int labels = strcmp(mode, "labels") == 0;

    summer_progress_t progress;
    summer_progress_init(&progress);
    if (strcmp(mode, "alka") == 0 || strcmp(mode, "pocket") == 0) {
        progress.cleared = (uint16_t)((1u << (4 - 1)) | (1u << (5 - 1)) | (1u << (6 - 1)) |
                                      (1u << (7 - 1)) | (1u << (13 - 1)) |
                                      (1u << (8 - 1)) | (1u << (9 - 1)));
        progress.restart_stage = 3;
    }

    if (labels) {
        for (uint32_t i = 0; i < pack.chapter_count; ++i) {
            summer_vm_t vm;
            if (!summer_vm_start_chapter(&vm, &pack, pack.chapters[i].scene)) continue;
            summer_step_t st = { 0 };
            for (int n = 0; n < 8; ++n) {
                st = summer_vm_step(&vm, &pack, 256);
                if (st.kind == SUMMER_STEP_TEXT || st.kind == SUMMER_STEP_CHOICE ||
                    st.kind == SUMMER_STEP_MAP || st.kind == SUMMER_STEP_END) break;
            }
            char label[SUMMER_TEXT_CAP];
            snprintf(label, sizeof(label), "%s", summer_vm_scene_label(&vm, &pack));
            char speaker[SUMMER_TEXT_CAP] = "";
            char text[SUMMER_TEXT_CAP] = "";
            if (st.kind == SUMMER_STEP_TEXT) {
                const uint32_t sp_off = summer_speaker_text(&pack, st.speaker);
                if (sp_off) summer_pack_text_into(&pack, sp_off, speaker, sizeof(speaker));
                summer_pack_text_into(&pack, st.text, text, sizeof(text));
            }
            printf("L\t%u\t", pack.chapters[i].scene);
            esc(label);
            fputc('\t', stdout);
            esc(speaker);
            fputc('\t', stdout);
            esc(text);
            fputc('\t', stdout);
            esc(vm.date);
            fputc('\n', stdout);
        }
        free(blob);
        return 0;
    }

    summer_vm_t vm;
    summer_vm_init(&vm);
    if (strcmp(mode, "chapter") == 0) {
        if (!summer_vm_start_chapter(&vm, &pack, scene_arg)) return 2;
    } else if (strcmp(mode, "alka") == 0) {
        if (!summer_vm_start_flow(&vm, &pack, SUMMER_TITLE_ALKA)) return 2;
    } else if (strcmp(mode, "pocket") == 0) {
        if (!summer_vm_start_flow(&vm, &pack, SUMMER_TITLE_POCKET)) return 2;
    } else {
        if (!summer_vm_start_story(&vm, &pack)) return 2;
    }
    const int story = vm.mode == SUMMER_MODE_STORY;
    if (story) progress_apply(&vm, &pack, &progress);

    char last_date[24] = "";
    for (long guard = 0; guard < 500000; ++guard) {
        if (story) progress_apply(&vm, &pack, &progress);
        const summer_step_t st = summer_vm_step(&vm, &pack, 256);
        if (st.kind == SUMMER_STEP_YIELD) continue;
        if (st.kind == SUMMER_STEP_ERROR) {
            printf("ERR\t%u\t%u\t%u\n", vm.scene_id, vm.op_index, st.error);
            return 3;
        }
        if (st.kind == SUMMER_STEP_TEXT) {
            char speaker[SUMMER_TEXT_CAP] = "";
            char text[SUMMER_TEXT_CAP] = "";
            char label[SUMMER_TEXT_CAP] = "";
            const uint32_t sp_off = summer_speaker_text(&pack, st.speaker);
            if (sp_off) summer_pack_text_into(&pack, sp_off, speaker, sizeof(speaker));
            summer_pack_text_into(&pack, st.text, text, sizeof(text));
            snprintf(label, sizeof(label), "%s", summer_vm_scene_label(&vm, &pack));
            if (strcmp(last_date, vm.date) != 0) {
                snprintf(last_date, sizeof(last_date), "%s", vm.date);
                char label_date[SUMMER_TEXT_CAP] = "";
                snprintf(label_date, sizeof(label_date), "%s", summer_vm_scene_label(&vm, &pack));
                printf("D	%u	", vm.scene_id);
                esc(vm.date);
                fputc('	', stdout);
                esc(label_date);
                fputc('\n', stdout);
            }
            emit_line("T", vm.scene_id, vm.op_index, speaker, text, label, NULL);
            if (!summer_vm_advance(&vm, &pack)) return 3;
            continue;
        }
        if (st.kind == SUMMER_STEP_CHOICE) {
            int items[32];
            const int n = summer_vm_visible_choices(&vm, &pack, st.group, items, 32);
            printf("C\t%u\t%u\t%d\n", vm.scene_id, vm.op_index, n);
            for (int i = 0; i < n; ++i) {
                const summer_choice_t *item = summer_choice_item(&pack, st.group, items[i]);
                char text[SUMMER_TEXT_CAP] = "";
                if (item) summer_pack_text_into(&pack, item->text, text, sizeof(text));
                printf("I\t%d\t", item ? item->value : 0);
                esc(text);
                fputc('\n', stdout);
            }
            const int index = choice_last ? n - 1 : 0;
            if (n <= 0 || !summer_vm_choose(&vm, &pack, st.group, items[index])) return 3;
            continue;
        }
        if (st.kind == SUMMER_STEP_MAP) {
            summer_map_choice_t map[40];
            const int n = summer_vm_preview_map(&vm, &pack, st.group, map, 40);
            printf("M\t%u\t%u\t%d\n", vm.scene_id, vm.op_index, n);
            for (int i = 0; i < n; ++i) {
                printf("I\t%d\t%u\t", map[i].value, map[i].dest_scene);
                esc(map[i].label);
                fputc('\n', stdout);
            }
            if (n <= 0) {
                printf("E\t%u\t%u\tMEXHAUST\n", vm.scene_id, vm.op_index);
                free(blob);
                return 0;
            }
            const int index = map_last ? n - 1 : 0;
            if (!summer_vm_choose_map(&vm, &pack, st.group, map[index].item_index)) return 3;
            continue;
        }
        if (st.kind == SUMMER_STEP_END) {
            printf("E\t%u\t%u\t%d\n", vm.scene_id, vm.op_index,
                   summer_vm_ending(&vm, &pack));
            free(blob);
            return 0;
        }
        if (st.kind == SUMMER_STEP_SCENE) continue;
    }
    fprintf(stderr, "trace limit\n");
    free(blob);
    return 3;
}
