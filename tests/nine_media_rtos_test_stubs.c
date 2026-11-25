// Controlled non-NINE_MEDIA_HOST RTOS/BSP boundary. The real production
// audio_worker is compiled; this file only supplies deterministic task,
// semaphore, timer and BSP capture primitives.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp_audio.h"
#include "esp_timer.h"
#include "nine_media.h"
#include "nine_image.h"

#include <assert.h>
#include <setjmp.h>
#include <string.h>

struct test_task { TaskFunction_t entry; void *arg; };
struct test_sem { bool available; };
static struct test_task s_task;
static struct test_sem s_sem;
static bool s_task_created;
static bool s_in_worker;
static unsigned s_notifications;
static jmp_buf s_park;

unsigned test_audio_format_calls;
uint32_t test_audio_hz;
uint8_t test_audio_bits;
uint8_t test_audio_channels;
unsigned test_audio_writes;
uint64_t test_audio_samples;
uint64_t test_audio_bytes;
bool test_audio_fail_format;
bool test_audio_fail_write;
unsigned test_audio_stop_after_writes;
unsigned test_audio_stop_calls;
unsigned test_audio_take_calls;
unsigned test_audio_task_priority;

static void run_worker(void)
{
    if (s_in_worker || !s_task_created) return;
    s_in_worker = true;
    if (setjmp(s_park) == 0) s_task.entry(s_task.arg);
    s_in_worker = false;
}

BaseType_t xTaskCreate(TaskFunction_t entry, const char *name, unsigned stack,
                       void *arg, unsigned priority, TaskHandle_t *task)
{
    (void)name; (void)stack;
    assert(task && entry);
    test_audio_task_priority = priority;
    s_task.entry = entry;
    s_task.arg = arg;
    s_task_created = true;
    *task = &s_task;
    return pdPASS;
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == &s_task && s_task_created);
    ++s_notifications;
    run_worker();
    return pdTRUE;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout)
{
    (void)clear; (void)timeout;
    ++test_audio_take_calls;
    if (!s_notifications) longjmp(s_park, 1);
    --s_notifications;
    return 1;
}

void vTaskDelete(TaskHandle_t task)
{
    assert(task == NULL && s_in_worker);
    longjmp(s_park, 1);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    memset(&s_sem, 0, sizeof(s_sem));
    return &s_sem;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    assert(sem == &s_sem);
    sem->available = true;
    return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout)
{
    (void)timeout;
    assert(sem == &s_sem);
    if (!sem->available) return pdFALSE;
    sem->available = false;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    assert(sem == &s_sem);
    memset(sem, 0, sizeof(*sem));
}

int64_t esp_timer_get_time(void)
{
    static int64_t now;
    now += 1000;
    return now;
}

esp_err_t bsp_audio_init(void) { return ESP_OK; }
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch)
{
    ++test_audio_format_calls;
    test_audio_hz = hz;
    test_audio_bits = bits;
    test_audio_channels = ch;
    return test_audio_fail_format ? ESP_FAIL : ESP_OK;
}
esp_err_t bsp_audio_write(const void *pcm, size_t bytes)
{
    (void)pcm;
    if (test_audio_fail_write) return ESP_FAIL;
    ++test_audio_writes;
    test_audio_bytes += bytes;
    test_audio_samples += bytes / sizeof(int16_t);
    if (test_audio_stop_after_writes &&
        test_audio_writes >= test_audio_stop_after_writes) {
        ++test_audio_stop_calls;
        nine_media_stop();
    }
    return ESP_OK;
}
void bsp_audio_set_volume(uint8_t percent) { (void)percent; }

uint32_t nine_image_op_count(void) { return 1; }
bool nine_image_op_frame(uint32_t index, uint32_t *image, uint32_t *timestamp,
                         uint32_t *duration)
{
    if (index) return false;
    if (image) *image = 0;
    if (timestamp) *timestamp = 0;
    if (duration) *duration = 1000;
    return true;
}
bool nine_image_show_asset(uint32_t image) { (void)image; return true; }
bool nine_image_show_asset_op(uint32_t image) { (void)image; return true; }
bool nine_image_show_asset_op_ex(uint32_t image, nine_image_op_result_t *result)
{
    (void)image;
    if (result) { result->crc_checks=1; result->jpeg_decodes=1; result->ok=true; }
    return true;
}
