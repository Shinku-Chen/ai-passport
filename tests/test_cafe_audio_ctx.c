// Audio failure during OP must retain OP context/timebase and restore OP on retry.
#include "cafe_app.h"
#include "cafe_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#c); return 1; } } while(0)
extern uint32_t cafe_audio_stub_op_seek(void);
static int fail_audio;
bool __real_cafe_audio_failed(void);
bool __wrap_cafe_audio_failed(void){return fail_audio || __real_cafe_audio_failed();}
static unsigned char *rf(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc(s);if(!b||fread(b,1,s,f)!=(size_t)s)return NULL;fclose(f);*n=s;return b;}
static void tap(cafe_app_t*a,uint8_t k,uint32_t t){cafe_app_key(a,k,true,t);cafe_app_key(a,k,false,t+40);}
static int find_op(uint16_t *on,uint16_t *oi){for(uint16_t n=0;n<923;n++){cafe_node_t x;if(!cafe_node_get(n,&x))continue;for(uint16_t i=0;i<x.frames;i++){uint8_t k=0;if(cafe_frame_kind(n,i,&k)&&k==1){*on=n;*oi=i;return 0;}}}return 1;}
int main(int ac,char**av){(void)ac;size_t n;unsigned char*p=rf(av[1],&n);CHECK(p&&cafe_pack_set_data(p,n));uint16_t on,oi;CHECK(find_op(&on,&oi)==0);CHECK(cafe_store_init());cafe_save_t s;memset(&s,0,sizeof s);s.occupied=true;s.node=on;s.index=oi;s.last_complete=0xff;CHECK(cafe_store_save_auto(&s));cafe_ui_t u;cafe_bands_t b;memset(&u,0,sizeof u);memset(&b,0,sizeof b);cafe_app_t a;CHECK(cafe_app_init(&a,&u,&b));a.now_ms=1000;a.last_activity_ms=1000;tap(&a,CAFE_KEY_OK,1000);a.title_sel=1;tap(&a,CAFE_KEY_OK,1100);CHECK(a.op_pending);const uint32_t start=a.op_start_ms;cafe_app_tick(&a,start+11000,11000);const uint16_t frame=a.op_frame;const uint16_t vmn=a.vm.node,vmi=a.vm.index;fail_audio=1;cafe_app_tick(&a,start+11040,40);CHECK(a.resource_fault);CHECK(a.fault_kind==5);CHECK(a.fault_page==CAFE_PAGE_TRANSITION);CHECK(a.fault_op_pending);CHECK(a.fault_op_elapsed_ms>=11000);fail_audio=0;cafe_app_tick(&a,start+12000,80);CHECK(a.resource_fault);/* swallow first cycle */
tap(&a,CAFE_KEY_OK,start+12000);CHECK(a.resource_fault);/* still modal */
tap(&a,CAFE_KEY_OK,start+12500);CHECK(!a.resource_fault);CHECK(a.op_pending);CHECK(a.page==CAFE_PAGE_TRANSITION);CHECK(a.vm.node==vmn&&a.vm.index==vmi);CHECK(cafe_audio_stub_op_seek()>=11000);CHECK(a.op_frame==frame);cafe_app_tick(&a,start+12560,60);CHECK(a.op_frame>=frame);CHECK(a.vm.node==vmn&&a.vm.index==vmi);printf("cafe audio OP retry context PASS: frame=%u seek=%u elapsed=%u\n",(unsigned)frame,(unsigned)cafe_audio_stub_op_seek(),(unsigned)a.fault_op_elapsed_ms);memset(&u,0,sizeof u);memset(&b,0,sizeof b);fail_audio=0;CHECK(cafe_app_init(&a,&u,&b));a.now_ms=2000;a.last_activity_ms=2000;tap(&a,CAFE_KEY_OK,2000);a.title_sel=0;tap(&a,CAFE_KEY_OK,2100);CHECK(a.page==CAFE_PAGE_GAME);uint16_t pn=a.vm.node,pi=a.vm.index;cafe_app_key(&a,CAFE_KEY_UP,true,2200);fail_audio=1;cafe_app_tick(&a,2800,600);CHECK(a.resource_fault);CHECK(!a.fast_forward);CHECK(!a.auto_mode);CHECK(a.vm.node==pn&&a.vm.index==pi);cafe_app_key(&a,CAFE_KEY_UP,false,2850);CHECK(a.vm.node==pn&&a.vm.index==pi);printf("cafe audio pending-long fault PASS\n");free(p);return 0;}
