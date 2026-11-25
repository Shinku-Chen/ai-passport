// tests/test_cafe_ff.c — long-UP sentence fast-forward contract on real pack data.
#include "cafe_app.h"
#include "cafe_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static unsigned char *read_file(const char *path,size_t*n){FILE*f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc((size_t)s);if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}fclose(f);*n=(size_t)s;return b;}
static void tap(cafe_app_t*a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_key(a,k,false,*t+40);*t+=80;}
static int cursor_changed(const cafe_app_t*a,uint16_t n,uint16_t i){return a->vm.node!=n||a->vm.index!=i;}
int main(int argc,char**argv){
 CHECK(argc==2);size_t n=0;unsigned char*pack=read_file(argv[1],&n);CHECK(pack&&cafe_pack_set_data(pack,n));
 cafe_ui_t ui;cafe_bands_t b;cafe_app_t a;uint32_t t=1000;memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);
 // Find a real multi-page text frame in the pack, then seed Continue to it.
 uint16_t long_node=0xFFFF,long_index=0;
 for(uint16_t node=0;node<cafe_meta_u32(0) && long_node==0xFFFF;++node){
  cafe_node_t nd;if(!cafe_node_get(node,&nd))continue;
  for(uint16_t idx=0;idx<nd.frames;++idx){cafe_frame_t f;if(cafe_frame_get(node,idx,&f)&&f.kind==0&&strlen(f.text)>=200){long_node=node;long_index=idx;break;}}
 }
 CHECK(long_node!=0xFFFF);
 cafe_store_init();cafe_save_t seed;memset(&seed,0,sizeof seed);seed.occupied=true;seed.node=long_node;seed.index=long_index;seed.progression=0;seed.last_complete=0xFF;snprintf(seed.chapter,sizeof seed.chapter,"ff-test");
 CHECK(cafe_store_save_auto(&seed));
 CHECK(cafe_app_init(&a,&ui,&b));a.settings.font_large=true;cafe_ui_set_font_large(&ui,true);a.now_ms=1000;a.last_activity_ms=1000;
 tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TITLE);tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME);
 CHECK(a.frame_pages>1);
 // First page: long UP threshold must immediately skip typing + all pages and land next sentence page0.
 const uint16_t n0=a.vm.node,i0=a.vm.index;
 CHECK(a.frame_page==0);
 cafe_app_key(&a,CAFE_KEY_UP,true,t);t+=500;cafe_app_tick(&a,t,500);
 CHECK(a.fast_forward);CHECK(cursor_changed(&a,n0,i0));CHECK(a.frame_page==0);CHECK(!cafe_ui_typing(&ui));CHECK(!a.auto_mode);
 // The next tick advances at most one more sentence; a large elapsed does not catch up.
 uint16_t n1=a.vm.node,i1=a.vm.index;cafe_app_tick(&a,t+50,50);CHECK(cursor_changed(&a,n1,i1));CHECK(a.fast_forward);
 n1=a.vm.node;i1=a.vm.index;cafe_app_tick(&a,t+1050,1000);CHECK(cursor_changed(&a,n1,i1));CHECK(a.fast_forward);
 cafe_app_key(&a,CAFE_KEY_UP,false,t+1100);CHECK(!a.fast_forward);
 const uint16_t n2=a.vm.node,i2=a.vm.index;cafe_app_tick(&a,t+1200,100);CHECK(!cursor_changed(&a,n2,i2));

 // Mid-page: short to page1, then long UP skips remaining pages and advances once.
 if(a.page==CAFE_PAGE_GAME && a.frame_pages>1){
  while(a.frame_page+1<a.frame_pages) tap(&a,CAFE_KEY_OK,&t);
  CHECK(a.frame_page==a.frame_pages-1);
  const uint16_t n3=a.vm.node,i3=a.vm.index;cafe_app_key(&a,CAFE_KEY_UP,true,t);t+=500;cafe_app_tick(&a,t,500);
  CHECK(cursor_changed(&a,n3,i3));CHECK(a.frame_page==0);CHECK(a.fast_forward);cafe_app_key(&a,CAFE_KEY_UP,false,t+10);CHECK(!a.fast_forward);
 }

 // Auto/typing: long UP stops auto, skips typing, and still lands next sentence page0.
 if(a.page==CAFE_PAGE_GAME){
  a.auto_mode=true;cafe_ui_set_mode(&ui,true);cafe_ui_set_text(&ui,"测试","自动中的长句应当被长按UP跳过",30);CHECK(cafe_ui_typing(&ui));
  const uint16_t n4=a.vm.node,i4=a.vm.index;cafe_app_key(&a,CAFE_KEY_UP,true,t);t+=500;cafe_app_tick(&a,t,500);
  CHECK(!a.auto_mode);CHECK(!cafe_ui_typing(&ui));CHECK(cursor_changed(&a,n4,i4));CHECK(a.frame_page==0);cafe_app_key(&a,CAFE_KEY_UP,false,t+10);
 }
 free(pack);printf("cafe long-UP fast-forward PASS\n");return 0;
}
