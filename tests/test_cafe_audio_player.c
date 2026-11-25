// Controlled FreeRTOS/HAL model of the production audio task/player loop.
#include "../main/cafe/cafe_audio.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#c); return 1; } } while(0)
static unsigned char *payload[2];
static size_t sizes[2], written;
static int write_error, write_calls, wait_calls, force_timeout;

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(struct FakeSem)); }
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->token = 1; return 1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    if (s->token) { s->token = 0; return 1; }
    if (t && !force_timeout) { wait_calls++; s_busy = false; return 1; }
    return 0;
}
BaseType_t xTaskCreate(void (*f)(void*), const char *n, uint32_t st, void *a, int p, TaskHandle_t *h)
{ (void)f; (void)n; (void)st; (void)a; (void)p; *h = (void*)1; return 1; }
void vTaskDelete(TaskHandle_t h) { (void)h; }
esp_err_t bsp_audio_init(void) { return ESP_OK; }
esp_err_t bsp_audio_set_format(uint32_t h, uint8_t b, uint8_t c) { return h==8000 && b==16 && c==1 ? ESP_OK : ESP_FAIL; }
void bsp_audio_set_volume(uint8_t v) { (void)v; }
esp_err_t bsp_audio_write(const void *p, size_t n) { (void)p; write_calls++; if (write_error) return ESP_FAIL; written += n; return ESP_OK; }
bool cafe_audio_track_get(uint8_t id, cafe_audio_track_t *t)
{
    if (id > 1) return false;
    memset(t, 0, sizeof(*t)); t->id=id; t->data_off=id; t->block_align=1024;
    t->samples=id?743959:1027198; t->sample_rate=id?8000:4000;
    return true;
}
const uint8_t *cafe_audio_data(const cafe_audio_track_t *t, uint32_t *n)
{ *n=(uint32_t)sizes[t->data_off]; return payload[t->data_off]; }
bool cafe_op_get(cafe_op_t *op) { memset(op,0,sizeof(*op)); op->audio_guard_ms=92400; return true; }
static int load(int id, const char *p)
{ FILE *f=fopen(p,"rb"); if(!f)return 0; fseek(f,0,SEEK_END); long n=ftell(f); rewind(f); payload[id]=malloc((size_t)n); sizes[id]=(size_t)n; int ok=payload[id]&&fread(payload[id],1,(size_t)n,f)==(size_t)n; fclose(f); return ok; }

static int run(uint8_t id, uint32_t seek, size_t expect)
{
    s_request=id; s_generation=7; s_failed=false; written=0; write_calls=0;
    bool ok=play_track_at(id,false,7,seek);
    printf("play id=%u seek=%u ok=%d pcm=%zu expected=%zu writes=%d\n",id,seek,ok,written,expect,write_calls);
    CHECK(ok); CHECK(written==expect); return 0;
}

int main(int ac, char **av)
{
    (void)ac;
    CHECK(load(0,av[1]) && load(1,av[2]));
    CHECK(cafe_audio_init());
    CHECK(run(0,0,4108792)==0);
    CHECK(run(1,0,1478400)==0);
    CHECK(run(1,11000,(739200u-88000u)*2u)==0);
    s_request=1; s_generation=7; s_failed=false; written=0; write_calls=0;
    CHECK(play_track_at(1,false,7,93000)); CHECK(written==0);
    s_generation=9; s_failed=false; write_error=1; write_calls=0;
    CHECK(play_track_at(0,false,8,0)); CHECK(!s_failed);
    write_error=0;
    s_request=0; s_generation=9; s_failed=false; write_error=1; write_calls=0;
    CHECK(!play_track_at(0,false,9,0)); CHECK(s_failed); CHECK(write_calls==1);
    write_error=0;
    // stale binary idle token + active busy: the second wait must acknowledge.
    s_idle->token=1; s_busy=true; s_request=0; s_generation=10; wait_calls=0;
    CHECK(cafe_audio_stop_confirmed(2)); CHECK(!s_busy); CHECK(wait_calls==1);
    // timeout is observable and leaves the task active.
    s_busy=true; force_timeout=1; wait_calls=0;
    CHECK(!cafe_audio_stop_confirmed(1)); CHECK(s_busy); CHECK(wait_calls==0);
    force_timeout=0; s_busy=false;
    printf("cafe audio player controlled-HAL PASS\n");
    return 0;
}
