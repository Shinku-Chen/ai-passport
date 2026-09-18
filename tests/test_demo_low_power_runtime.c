#include "demo_stubs/demo_runtime.c"
#include "../main/demo_low_power.c"

int main(void) {
    test_sem_fails = true;
    assert(demo_low_power_start() == ESP_ERR_NO_MEM);
    test_sem_fails = false;
    test_create_fails = true;
    assert(demo_low_power_start() == ESP_ERR_NO_MEM);
    assert(!s_task && !s_stopped);
    test_create_fails = false;
    assert(demo_low_power_start() == ESP_OK);
    TaskHandle_t first = s_task;
    assert(demo_low_power_stop() == ESP_ERR_TIMEOUT);
    assert(s_task == first && !first->deleted && s_stop_requested);
    unsigned notifications = test_notifications;
    demo_low_power_key(BSP_BTN_OK, BSP_BTN_CLICK);
    assert(test_notifications == notifications); // No new work after stop request.
    test_run_worker(first);
    assert(s_task == first && !first->deleted && s_stopped->available);
    assert(demo_low_power_stop() == ESP_OK);
    assert(!s_task && !s_stopped && first->deleted);
    assert(demo_low_power_start() == ESP_OK);
    assert(s_task != first && !s_task->deleted);
    test_auto_ack = true;
    assert(demo_low_power_stop() == ESP_OK);
    assert(demo_low_power_stop() == ESP_OK);
    assert(test_task_deletes == 2);
    puts("low-power worker timeout/retry/re-entry tests: PASS");
    return 0;
}
