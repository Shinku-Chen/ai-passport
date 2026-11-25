// Production-path gallery audio policy test. Media is a counting stub; the
// app/gal_ui/nine_app navigation code is the real production path.
#include "nine_app.h"
#include "bsp_button.h"

#include <assert.h>
#include <stdio.h>

extern const char *nine_menu_script_path;
extern const char *nine_menu_images_path;
extern int nine_audio_play_count;
extern int nine_audio_stop_count;
extern int nine_audio_op_count;
extern uint32_t nine_audio_current;
extern bool nine_audio_playing;

static void key(nine_app_t *app, int b, int e)
{ nine_key_t k={ (uint8_t)b, (uint8_t)e }; nine_app_key(app,&k); }
static void click(nine_app_t *app, int b) { key(app,b,BSP_BTN_CLICK); }
static void long_(nine_app_t *app, int b) { key(app,b,BSP_BTN_LONG); }

int main(int argc,char**argv)
{
    assert(argc==3); nine_menu_script_path=argv[1]; nine_menu_images_path=argv[2];
    nine_app_t app; assert(nine_app_init(&app)); assert(!app.error);

    click(&app,BSP_BTN_OK); // START -> TITLE, title BGM starts
    assert(app.page==GAL_PAGE_TITLE);
    assert(nine_audio_play_count==1 && nine_audio_current==0 && nine_audio_playing);

    for(int i=0;i<4;i++) click(&app,BSP_BTN_DOWN);
    click(&app,BSP_BTN_OK); // Gallery
    assert(app.page==GAL_PAGE_GALLERY);
    assert(nine_audio_stop_count==1);
    assert(!nine_audio_playing && !app.title_music);
    const int stops_after_gallery=nine_audio_stop_count;
    const int plays_after_gallery=nine_audio_play_count;

    click(&app,BSP_BTN_OK); // CG list
    click(&app,BSP_BTN_OK); // first viewer
    assert(app.page==GAL_PAGE_CG);
    click(&app,BSP_BTN_DOWN); // next CG variant
    assert(app.page==GAL_PAGE_CG);
    long_(&app,BSP_BTN_OK); // gallery
    assert(app.page==GAL_PAGE_GALLERY);
    assert(nine_audio_stop_count==stops_after_gallery);
    assert(nine_audio_play_count==plays_after_gallery);

    click(&app,BSP_BTN_DOWN); click(&app,BSP_BTN_OK); // recall
    assert(app.page==GAL_PAGE_RECALL);
    long_(&app,BSP_BTN_OK); // gallery
    assert(app.page==GAL_PAGE_GALLERY);
    assert(nine_audio_stop_count==stops_after_gallery);

    // Chapter browsing must also stay silent and must not add another stop.
    click(&app,BSP_BTN_DOWN); // Recall -> Chapters
    click(&app,BSP_BTN_OK);
    assert(app.page==GAL_PAGE_CHAPTER);
    assert(nine_audio_stop_count==stops_after_gallery);
    long_(&app,BSP_BTN_OK); // chapter -> gallery
    assert(app.page==GAL_PAGE_GALLERY);

    for(int i=0;i<1;i++) click(&app,BSP_BTN_DOWN); // Music entry
    click(&app,BSP_BTN_OK);
    assert(app.page==GAL_PAGE_MUSIC);
    assert(nine_audio_play_count==plays_after_gallery); // no auto-play
    assert(!nine_audio_playing);
    click(&app,BSP_BTN_OK); // explicit selection
    assert(nine_audio_play_count==plays_after_gallery+1);
    assert(nine_audio_playing);

    long_(&app,BSP_BTN_OK); // music -> gallery, then gallery long -> title
    assert(app.page==GAL_PAGE_GALLERY);
    long_(&app,BSP_BTN_OK);
    assert(app.page==GAL_PAGE_TITLE);
    assert(nine_audio_play_count==plays_after_gallery+2);
    assert(app.title_music && nine_audio_current==0 && nine_audio_playing);

    puts("nine app gallery/audio policy: PASS");
    return 0;
}
