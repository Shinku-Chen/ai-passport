#include "lvgl.h"
#include <stdio.h>
static unsigned used(void){lv_mem_monitor_t m;lv_mem_monitor(&m);return (unsigned)(m.total_size-m.free_size);}
int main(void){lv_init();lv_display_t*d=lv_display_create(320,240);(void)d;lv_obj_t*s=lv_screen_active();unsigned a=used();
for(int i=0;i<5;i++){lv_obj_t*r=lv_obj_create(s);lv_obj_set_size(r,300,28);lv_obj_t*l=lv_label_create(r);lv_label_set_text(l,"row label text 1234567890");lv_obj_t*v=lv_label_create(r);lv_label_set_text(v,"value");}
unsigned b=used();printf("5 rows delta=%u per=%.1f total=%u\n",b-a,(b-a)/5.0,b);return 0;}
