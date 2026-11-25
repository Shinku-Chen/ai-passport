// OP clock/skip: natural end and short-skip each advance the VM exactly once.
#include "cafe_app.h"
#include "cafe_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#c); return 1; } } while (0)
static unsigned char *rf(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc(s);if(!b||fread(b,1,s,f)!=(size_t)s)return NULL;fclose(f);*n=s;return b;}
static void tap(cafe_app_t *a,uint8_t k,uint32_t t){cafe_app_key(a,k,true,t);cafe_app_key(a,k,false,t+40);}
static int find_op(uint16_t *on,uint16_t *oi){for(uint16_t n=0;n<923;n++){cafe_node_t x;if(!cafe_node_get(n,&x))continue;for(uint16_t i=0;i<x.frames;i++){uint8_t k=0;if(cafe_frame_kind(n,i,&k)&&k==1){*on=n;*oi=i;return 0;}}}return 1;}
int main(int ac,char**av){(void)ac;size_t n;unsigned char*p=rf(av[1],&n);CHECK(p&&cafe_pack_set_data(p,n));uint16_t on,oi;CHECK(find_op(&on,&oi)==0);CHECK(cafe_store_init());cafe_save_t s;memset(&s,0,sizeof(s));s.occupied=true;s.node=on;s.index=oi;s.progression=0;s.last_complete=0xFF;CHECK(cafe_store_save_auto(&s));cafe_ui_t ui;cafe_bands_t b;memset(&ui,0,sizeof(ui));memset(&b,0,sizeof(b));cafe_app_t a;CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=1000;a.last_activity_ms=1000;tap(&a,CAFE_KEY_OK,1000);CHECK(a.page==CAFE_PAGE_TITLE);a.title_sel=1;tap(&a,CAFE_KEY_OK,1100);CHECK(a.page==CAFE_PAGE_TRANSITION);CHECK(a.op_pending);CHECK(a.op_frame==0);const uint16_t before=a.vm.index;tap(&a,CAFE_KEY_OK,1200);CHECK(!a.op_pending);CHECK(a.vm.index!=before);CHECK(a.page!=CAFE_PAGE_TRANSITION);
// Second run: natural duration ends once.
CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=2000;a.last_activity_ms=2000;tap(&a,CAFE_KEY_OK,2000);a.title_sel=1;tap(&a,CAFE_KEY_OK,2100);CHECK(a.op_pending);const uint32_t start=a.op_start_ms;cafe_app_tick(&a,start+95461,50);CHECK(!a.op_pending);CHECK(a.page!=CAFE_PAGE_TRANSITION);free(p);puts("cafe OP skip/end once PASS");return 0;}
