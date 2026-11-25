// tests/test_cafe_nav_pages.c — real app key navigation on Music/Chapter/Recall pages.
#include "cafe_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
extern int audit_ui_rows_selected;
extern int audit_ui_list_selected[CAFE_LIST_COUNT];
extern int audit_ui_choice_selected;
static unsigned char *rf(const char *p, size_t *n) { FILE *f=fopen(p,"rb"); if(!f)return NULL; fseek(f,0,SEEK_END); long s=ftell(f); fseek(f,0,SEEK_SET); unsigned char*b=malloc((size_t)s); if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;} fclose(f);*n=(size_t)s;return b; }
static void tap(cafe_app_t *a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_key(a,k,false,*t+40);*t+=80;}
static void long_key(cafe_app_t *a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_tick(a,*t+520,520);*t+=520;cafe_app_key(a,k,false,*t);*t+=40;}
static void open_gallery(cafe_app_t *a,uint32_t*t){tap(a,CAFE_KEY_OK,t);for(int i=0;i<4;i++)tap(a,CAFE_KEY_DOWN,t);tap(a,CAFE_KEY_OK,t);}
static void open_music(cafe_app_t *a,uint32_t*t){open_gallery(a,t);for(int i=0;i<3;i++)tap(a,CAFE_KEY_DOWN,t);tap(a,CAFE_KEY_OK,t);}
int main(int ac,char**av){(void)ac;size_t n;unsigned char*p=rf(av[1],&n);CHECK(p&&cafe_pack_set_data(p,n));cafe_ui_t ui;cafe_bands_t b;cafe_app_t a;memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);uint32_t t=1000;
CHECK(cafe_app_init(&a,&ui,&b));open_music(&a,&t);CHECK(a.page==CAFE_PAGE_MUSIC);CHECK(ui.rows_count==2);CHECK(a.music_sel==0&&audit_ui_rows_selected==0);
tap(&a,CAFE_KEY_DOWN,&t);CHECK(a.music_sel==1&&audit_ui_rows_selected==1);
tap(&a,CAFE_KEY_UP,&t);CHECK(a.music_sel==0&&audit_ui_rows_selected==0);
long_key(&a,CAFE_KEY_DOWN,&t);CHECK(a.music_sel==1&&audit_ui_rows_selected==1);
long_key(&a,CAFE_KEY_UP,&t);CHECK(a.music_sel==0&&audit_ui_rows_selected==0);
tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_MUSIC); // plays selected, does not return
long_key(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GALLERY); // long OK returns gallery

memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));open_gallery(&a,&t);tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_CHAPTER&&a.chapter_routes);CHECK(audit_ui_list_selected[CAFE_LIST_ROUTE]==0);
long_key(&a,CAFE_KEY_DOWN,&t);CHECK(a.route_sel==1&&audit_ui_list_selected[CAFE_LIST_ROUTE]==1);
long_key(&a,CAFE_KEY_UP,&t);CHECK(a.route_sel==0&&audit_ui_list_selected[CAFE_LIST_ROUTE]==0);
for(int i=0;i<6;i++)tap(&a,CAFE_KEY_DOWN,&t);CHECK(a.route_sel==6&&audit_ui_list_selected[CAFE_LIST_ROUTE]==6);
tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_CHAPTER&&!a.chapter_routes);CHECK(ui.rows_count==5);CHECK(a.chapter_page==0);
for(int i=0;i<4;i++)tap(&a,CAFE_KEY_DOWN,&t);CHECK(a.chapter_sel==4&&audit_ui_list_selected[CAFE_LIST_CHAPTER]==4);
tap(&a,CAFE_KEY_DOWN,&t);CHECK(a.chapter_sel==4&&a.chapter_page==0); // no empty page
for(int i=0;i<4;i++)tap(&a,CAFE_KEY_UP,&t);CHECK(a.chapter_sel==0&&audit_ui_list_selected[CAFE_LIST_CHAPTER]==0);
long_key(&a,CAFE_KEY_DOWN,&t);CHECK(a.chapter_sel==1&&audit_ui_list_selected[CAFE_LIST_CHAPTER]==1);
long_key(&a,CAFE_KEY_UP,&t);CHECK(a.chapter_sel==0&&audit_ui_list_selected[CAFE_LIST_CHAPTER]==0);
CHECK(a.unlock.progress==0); // preview navigation must not grant permanent progress

    // Preview path: route 0 -> chapter 0 -> transition -> text.  This is the
    // same cafe_frame_get/cafe_inflate path used by NewGame and Continue.
    memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));open_gallery(&a,&t);tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_CHAPTER&&a.chapter_routes);
    tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_CHAPTER&&!a.chapter_routes);
    tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TRANSITION&&a.preview_pending);
    const uint32_t preview_unlock=a.unlock.progress;
    tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME&&a.vm.progression==CAFE_PROGRESSION_CHAPTER);
    CHECK(a.current_text[0] != 0);CHECK(a.unlock.progress==preview_unlock);
    tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME||a.page==CAFE_PAGE_CHOICE||a.page==CAFE_PAGE_TRANSITION);
    CHECK(!a.resource_fault);
free(p);printf("cafe nav pages PASS\n");return 0;}
