#include "bsp_battery.h"
static int s_soc = -1;
int bsp_battery_soc(void) { return s_soc; }
void bsp_stub_set_soc(int percent) { s_soc = percent; }
