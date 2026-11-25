// tests/test_cafe_op_gallery.c — gallery OP is independent of story state.
#include "cafe_app.h"
#include "cafe_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#c); return 1; } } while(0)
extern uint32_t cafe_audio_stub_op_seek(void);
extern int audit_image_op_fail;
static int fail_audio;
bool __real_cafe_audio_failed(void);
bool __wrap_cafe_audio_failed(void){return fail_audio || __real_cafe_audio_failed();}
static unsigned char *rf(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc((size_t)s);if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}fclose(f);*n=(size_t)s;return b;}
static void tap(cafe_app_t*a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_key(a,k,false,*t+40);*t+=80;}
static void long_ok(cafe_app_t*a,uint32_t*t){cafe_app_key(a,CAFE_KEY_OK,true,*t);cafe_app_tick(a,*t+520,520);*t+=520;cafe_app_key(a,CAFE_KEY_OK,false,*t);*t+=40;}
static int open_gallery_row(cafe_app_t*a,uint32_t*t){tap(a,CAFE_KEY_OK,t);for(int i=0;i<4;i++)tap(a,CAFE_KEY_DOWN,t);tap(a,CAFE_KEY_OK,t);CHECK(a->page==CAFE_PAGE_GALLERY);for(int i=0;i<4;i++)tap(a,CAFE_KEY_DOWN,t);CHECK(a->gallery_sel==4);tap(a,CAFE_KEY_OK,t);return a->page==CAFE_PAGE_TRANSITION&&a->op_pending&&a->op_gallery;}
int main(int ac,char**av){(void)ac;size_t n=0;unsigned char*p=rf(av[1],&n);CHECK(p&&cafe_pack_set_data(p,n));cafe_ui_t ui;cafe_bands_t b;cafe_app_t a;uint32_t t=1000;memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=1000;a.last_activity_ms=1000;
 CHECK(open_gallery_row(&a,&t));const uint16_t vn=a.vm.node,vi=a.vm.index;const uint32_t vp=a.vm.progress;const uint8_t vprog=a.vm.progression;uint8_t flags[CAFE_FLAG_MAX];memcpy(flags,a.vm.flags,sizeof flags);const uint32_t up=a.unlock.progress;const int hc=a.history_count;CHECK(a.op_frame==0);const uint32_t st=a.op_start_ms;
 cafe_app_tick(&a,st+11000,11000);CHECK(a.page==CAFE_PAGE_TRANSITION);CHECK(a.op_frame>=30);const uint16_t mid=a.op_frame;cafe_app_tick(&a,st+92000,81000);CHECK(a.op_frame>mid);const uint16_t last=a.op_frame;cafe_app_tick(&a,st+95500,3500);CHECK(a.page==CAFE_PAGE_GALLERY&&a.gallery_sel==4&&!a.op_pending);CHECK(a.vm.node==vn&&a.vm.index==vi&&a.vm.progress==vp&&a.vm.progression==vprog);CHECK(memcmp(a.vm.flags,flags,sizeof flags)==0&&a.unlock.progress==up&&a.history_count==hc);CHECK(!a.progress_dirty&&!a.unlock_dirty);
 // Existing progress/flags/history must also survive unchanged.
 a.unlock.progress=7; a.vm.flags[0]|=0x80; a.history_count=2; a.progress_dirty=false; a.unlock_dirty=false;
 const uint32_t up_existing=a.unlock.progress; const int hc_existing=a.history_count;
 // Replay, short skip, long OK return; all return to same OP row without VM/NVS.
 tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TRANSITION&&a.op_gallery);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GALLERY&&a.gallery_sel==4);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TRANSITION);long_ok(&a,&t);CHECK(a.page==CAFE_PAGE_GALLERY&&a.gallery_sel==4);CHECK(a.vm.node==vn&&a.vm.index==vi&&a.unlock.progress==up_existing&&a.history_count==hc_existing&&!a.progress_dirty&&!a.unlock_dirty);
 // Image/JPEG fault retries the gallery OP context too.
 audit_image_op_fail=1;tap(&a,CAFE_KEY_OK,&t);CHECK(a.resource_fault&&a.fault_op_gallery);audit_image_op_fail=0;tap(&a,CAFE_KEY_OK,&t);CHECK(!a.resource_fault&&a.page==CAFE_PAGE_TRANSITION&&a.op_gallery);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GALLERY&&a.gallery_sel==4);
 // Audio fault retries the gallery OP context, not story.
 tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TRANSITION);const uint16_t fault_frame=a.op_frame;fail_audio=1;cafe_app_tick(&a,a.now_ms+40,40);CHECK(a.resource_fault&&a.fault_op_gallery);fail_audio=0;tap(&a,CAFE_KEY_OK,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(!a.resource_fault&&a.page==CAFE_PAGE_TRANSITION&&a.op_gallery);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GALLERY&&a.gallery_sel==4);CHECK(a.vm.node==vn&&a.vm.index==vi&&a.unlock.progress==up_existing&&a.history_count==hc_existing);
 printf("cafe gallery OP PASS: mid=%u last=%u fault_frame=%u\n",mid,last,fault_frame);free(p);return 0;}
