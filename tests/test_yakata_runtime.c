/* Exercise the real application and NVS encoders with a fake display/storage.
 * This proves state transitions, not LVGL rendering or physical key timing. */
#include "gal_app.c"
#include "nvs.h"
#include <assert.h>

const lv_font_t gal_cjk_16={0}, gal_cjk_20={0};
const uint8_t host_art[1] __asm__("_binary_yakata_art_bin_start")={0};

static uint8_t blobs[4][2048];
static size_t lengths[4];
static const char *spaces[]={"galcfg","yksave","ykprog","ykunlock"};
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h) {
    (void)mode;
    for(int i=0;i<4;++i) if(!strcmp(ns,spaces[i])) { *h=i; return ESP_OK; }
    return 2;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t n) {
    (void)key; assert(n<=sizeof(blobs[h])); memcpy(blobs[h],data,n); lengths[h]=n; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *data,size_t *n) {
    (void)key;
    if(!lengths[h]) return ESP_ERR_NVS_NOT_FOUND;
    if(*n<lengths[h]) return 2;
    *n=lengths[h];memcpy(data,blobs[h],*n);return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void gal_music_stop(void) {}
int gal_music_current(void) { return -1; }
void gal_music_play(int index) { (void)index; }

static void key(gal_app_t *app,int button,int event) {
    gal_key_t k={(uint8_t)button,(uint8_t)event}; gal_app_key(app,&k);
}
static int first_event(int kind,int index) {
    for(int i=0;i<gal_content_event_count;++i)
        if(gal_content_events[i].kind==kind && gal_content_events[i].index==index) return i;
    assert(0); return -1;
}
int main(void) {
    static gal_app_t a,b;
    assert(gal_app_init(&a)); assert(a.page==GAL_PAGE_TITLE);
    key(&a,BSP_BTN_OK,BSP_BTN_CLICK); assert(a.page==GAL_PAGE_TRANSITION);
    key(&a,BSP_BTN_OK,BSP_BTN_CLICK); assert(a.page==GAL_PAGE_GAME);
    key(&a,BSP_BTN_OK,BSP_BTN_LONG); assert(a.page==GAL_PAGE_MENU);
    key(&a,BSP_BTN_OK,BSP_BTN_LONG); assert(a.page==GAL_PAGE_GAME);

    // Long UP moves immediately, including while typing. Subsequent turns do
    // not need an elapsed-time threshold; release stops further movement.
    a.event_index=first_event(GAL_EV_LINE,0);a.page_index=0;a.shown_line=-1;enter_event(&a);
    int before_event=a.event_index,before_page=a.page_index;
    assert(a.ui.typing_active);
    key(&a,BSP_BTN_UP,BSP_BTN_LONG);
    assert(a.fast_forward && !a.ui.typing_active);
    assert(a.event_index!=before_event || a.page_index!=before_page);
    before_event=a.event_index;before_page=a.page_index;
    gal_app_tick(&a,0);
    assert(a.event_index!=before_event || a.page_index!=before_page);
    key(&a,BSP_BTN_UP,BSP_BTN_RELEASE);
    before_event=a.event_index;before_page=a.page_index;
    gal_app_tick(&a,5000);
    assert(!a.fast_forward && a.event_index==before_event && a.page_index==before_page);

    // Chapter cards neither cancel a held skip nor impose the auto-read delay.
    // A release on the card itself must still stop; long UP can start there.
    a.event_index=first_event(GAL_EV_TRANSITION,0);a.fast_forward=true;enter_event(&a);
    assert(a.page==GAL_PAGE_TRANSITION && a.fast_forward);
    gal_app_tick(&a,0);assert(a.page==GAL_PAGE_GAME && a.fast_forward);
    a.event_index=first_event(GAL_EV_TRANSITION,0);enter_event(&a);
    key(&a,BSP_BTN_UP,BSP_BTN_RELEASE);
    gal_app_tick(&a,5000);assert(a.page==GAL_PAGE_TRANSITION && !a.fast_forward);
    key(&a,BSP_BTN_UP,BSP_BTN_LONG);assert(a.page==GAL_PAGE_GAME && a.fast_forward);

    // Choices and endings always stop skip and remain available for decisions.
    for(int i=0;i<gal_content_choice_count;++i) {
        int choice_event=first_event(GAL_EV_CHOICE,i);
        assert(gal_content_events[choice_event-1].kind==GAL_EV_TRANSITION);
        a.event_index=choice_event-1;a.page_index=0;a.shown_line=-1;enter_event(&a);
        key(&a,BSP_BTN_UP,BSP_BTN_LONG);
        assert(a.page==GAL_PAGE_CHOICE && !a.fast_forward);
        gal_app_tick(&a,5000);assert(a.event_index==choice_event);
    }
    a.event_index=first_event(GAL_EV_ENDING,0);a.fast_forward=true;enter_event(&a);
    assert(a.page==GAL_PAGE_ENDING && !a.fast_forward);
    before_event=a.event_index;gal_app_tick(&a,5000);assert(a.event_index==before_event);

    // Ordinary auto-read retains its own timing and waits for typing to finish.
    a.event_index=first_event(GAL_EV_TRANSITION,0);a.shown_line=-1;a.auto_mode=true;enter_event(&a);
    gal_app_tick(&a,1599);assert(a.page==GAL_PAGE_TRANSITION);
    gal_app_tick(&a,1);assert(a.page==GAL_PAGE_GAME && a.auto_mode);
    before_event=a.event_index;before_page=a.page_index;
    assert(a.ui.typing_active);gal_app_tick(&a,5000);
    assert(a.event_index==before_event && a.page_index==before_page);
    gal_ui_show_full_text(&a.ui);gal_app_tick(&a,899);
    assert(a.event_index==before_event && a.page_index==before_page);
    gal_app_tick(&a,1);assert(a.event_index!=before_event || a.page_index!=before_page);
    a.auto_mode=false;

    start_new_game(&a);key(&a,BSP_BTN_UP,BSP_BTN_LONG);
    int steps=0;
    while(a.fast_forward && steps++<20000)gal_app_tick(&a,0);
    assert(steps<20000 && a.page==GAL_PAGE_CHOICE);
    assert(a.event_index==first_event(GAL_EV_CHOICE,0));
    start_new_game(&a);key(&a,BSP_BTN_UP,BSP_BTN_LONG);
    key(&a,BSP_BTN_OK,BSP_BTN_LONG);
    assert(a.page==GAL_PAGE_MENU && !a.fast_forward);

    // A flag-setting choice must change this playthrough and be restored by
    // both save mechanisms, even after another playthrough changed its flags.
    a.event_index=first_event(GAL_EV_CHOICE,1);a.page_index=0;enter_event(&a);
    key(&a,BSP_BTN_DOWN,BSP_BTN_CLICK); key(&a,BSP_BTN_OK,BSP_BTN_CLICK);
    assert(a.variables[1]==1);
    int saved=a.event_index;
    do_save(&a,0);a.variables[1]=0;do_load(&a,0);
    assert(a.event_index==saved && a.variables[1]==1);
    gal_app_flush(&a);assert(gal_app_init(&b));
    b.title_sel=GAL_TITLE_MENU_CONTINUE;key(&b,BSP_BTN_OK,BSP_BTN_CLICK);
    assert(b.event_index==saved && b.variables[1]==1);

    // Auto mode must remain lit while waiting at a choice for much longer
    // than the main loop's sleep timeout. Release must not cancel auto mode.
    a.event_index=first_event(GAL_EV_CHOICE,0);enter_event(&a);a.auto_mode=true;
    for(int i=0;i<1000;++i) gal_app_tick(&a,1000);
    assert(a.auto_mode && gal_app_idle_ms(&a)==0 && a.page==GAL_PAGE_CHOICE);
    key(&a,BSP_BTN_DOWN,BSP_BTN_RELEASE);assert(a.auto_mode);
    key(&a,BSP_BTN_OK,BSP_BTN_CLICK);assert(!a.auto_mode && a.page==GAL_PAGE_CHOICE);

    // Entries above the old 32-bit limit survive NVS encoding.
    gal_unlock_grant_cg(&a.unlock,55);gal_unlock_grant_chapter(&a.unlock,98);
    assert(gal_unlock_store(&a.unlock));assert(gal_unlock_load(&b.unlock));
    assert(gal_unlock_has_cg(&b.unlock,55)&&gal_unlock_has_chapter(&b.unlock,98));
    a.cg_sel=55;fill_gallery(&a);a.cg_variant=0;goto_page(&a,GAL_PAGE_CG);
    key(&a,BSP_BTN_OK,BSP_BTN_LONG);assert(a.page==GAL_PAGE_CG_LIST);
    a.chapter_sel=98;fill_chapters(&a);goto_page(&a,GAL_PAGE_CHAPTER);
    key(&a,BSP_BTN_OK,BSP_BTN_LONG);assert(a.page==a.return_page);
    start_new_game(&a);assert(a.variables[1]==0);
    // Portrait and old 90px landscape page indices do not identify the new
    // 80px panel's text position. Keep the line/route and replay it once.
    gal_save_t save_copy={0};
    save_copy.slots[0].occupied=1;save_copy.slots[0].event_index=9;
    save_copy.slots[0].page_index=2;save_copy.slots[0].variables[1]=1;
    assert(gal_save_store(&save_copy));assert(gal_save_load(&save_copy));
    assert(save_copy.slots[0].page_index==2);
    for(int layout=0;layout<2;++layout) {
        blobs[1][2]=(uint8_t)layout;assert(gal_save_load(&save_copy));
        assert(save_copy.slots[0].page_index==0 && save_copy.slots[0].event_index==9 && save_copy.slots[0].variables[1]==1);
    }
    gal_progress_t progress={.valid=1,.event_index=9,.page_index=2};progress.variables[1]=1;
    assert(gal_progress_store(&progress));assert(gal_progress_load(&progress));assert(progress.page_index==2);
    for(int layout=0;layout<2;++layout) {
        blobs[2][2]=(uint8_t)layout;assert(gal_progress_load(&progress));
        assert(progress.page_index==0 && progress.event_index==9 && progress.variables[1]==1);
    }
    puts("Yakata runtime: immediate skip, chapter/choice/release boundaries, auto timing, route variables, NVS restoration, high unlock bits, auto idle PASS");
    return 0;
}
