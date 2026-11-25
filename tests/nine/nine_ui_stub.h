#pragma once
#include <stdbool.h>
extern char nine_ui_text[2048];
extern char nine_ui_speaker[128];
extern char nine_ui_notice[128];
extern char nine_ui_ending[3][512];
extern int nine_ui_page;
extern int nine_ui_choice_count;
extern char nine_ui_choice[16][128];
void nine_ui_stub_reset(void);
