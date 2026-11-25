#include "lvgl.h"
#include <stdio.h>
int main(void){lv_init();lv_display_t*d=lv_display_create(320,240);(void)d;lv_obj_t*scr=lv_screen_active();printf("screen=%p children=%d\n",(void*)scr,(int)lv_obj_get_child_count(scr));lv_obj_t*l=lv_label_create(scr);lv_label_set_text(l,"LVGL host ok");lv_mem_monitor_t m;lv_mem_monitor(&m);printf("used=%u free=%u largest=%u\n",(unsigned)m.total_size,(unsigned)m.free_size,(unsigned)m.free_biggest_size);return 0;}
