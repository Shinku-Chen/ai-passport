// 宿主机用 bsp_battery.h:电量由测试设定。
#pragma once
int bsp_battery_soc(void);
void bsp_stub_set_soc(int percent);
