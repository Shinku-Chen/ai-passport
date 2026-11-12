#pragma once
#include "atri_ui.h"
typedef struct {
    atri_page_t page;
    int list_count[ATRI_LIST_COUNT];
    char list_labels[ATRI_LIST_COUNT][ATRI_UI_MAX_ROWS][64];
    char list_values[ATRI_LIST_COUNT][ATRI_UI_MAX_ROWS][64];
    int choice_count;
    int choice_selected;
    char notice[64];
    char ending_name[64];
} fake_ui_state_t;
extern fake_ui_state_t g_fake_ui;

