#include "lvgl.h"
#include <stdio.h>
int main(void){printf("MEM_SIZE=%u\n",(unsigned)LV_MEM_SIZE);lv_init();lv_mem_monitor_t m;lv_mem_monitor(&m);printf("total=%u free=%u\n",(unsigned)m.total_size,(unsigned)m.free_size);return 0;}
