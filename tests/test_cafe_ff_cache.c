// tests/test_cafe_ff_cache.c — same-visual text frames must not re-decode art.
#include "cafe_app.h"
#include "cafe_image.h"
#include "cafe_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static unsigned char *read_file(const char *p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc((size_t)s);if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}fclose(f);*n=(size_t)s;return b;}
static void tap(cafe_app_t*a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_key(a,k,false,*t+40);*t+=80;}
static void hold(cafe_app_t*a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_tick(a,*t+520,520);*t+=520;cafe_app_key(a,k,false,*t);*t+=40;}
static int same_visual(const cafe_frame_t*a,const cafe_frame_t*b){
 if(a->bg_asset!=b->bg_asset||a->sprite_count!=b->sprite_count)return 0;
 for(unsigned i=0;i<a->sprite_count;i++){cafe_sprite_t x,y;if(!cafe_frame_sprite(a,i,&x)||!cafe_frame_sprite(b,i,&y))return 0;if(memcmp(&x,&y,sizeof x)!=0)return 0;}
 return 1;
}
int main(int argc,char**argv){
 CHECK(argc==2);size_t n=0;unsigned char*pack=read_file(argv[1],&n);CHECK(pack&&cafe_pack_set_data(pack,n));
 unsigned short node0=0xFFFF,idx0=0,idx1=0;
 for(unsigned short node=0;node<cafe_meta_u32(0)&&node0==0xFFFF;node++){
  cafe_node_t nd;if(!cafe_node_get(node,&nd))continue;
  for(unsigned short i=0;i+1<nd.frames;i++){cafe_frame_t a,b;if(!cafe_frame_get(node,i,&a)||!cafe_frame_get(node,i+1,&b))continue;if(a.kind==0&&b.kind==0&&same_visual(&a,&b)){node0=node;idx0=i;idx1=i+1;break;}}
 }
 CHECK(node0!=0xFFFF);
 cafe_store_init();cafe_save_t seed;memset(&seed,0,sizeof seed);seed.occupied=true;seed.node=node0;seed.index=idx0;seed.progression=0;seed.last_complete=0xFF;snprintf(seed.chapter,sizeof seed.chapter,"cache-test");CHECK(cafe_store_save_auto(&seed));
 cafe_ui_t ui;cafe_bands_t b;cafe_app_t a;uint32_t t=1000;memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_image_bands_init(&b));CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=1000;a.last_activity_ms=1000;
 tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TITLE);tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME);CHECK(a.vm.node==node0&&a.vm.index==idx0);
 cafe_image_reset_counts();tap(&a,CAFE_KEY_OK,&t);CHECK(a.vm.node==node0);CHECK(a.vm.index==idx1);CHECK(cafe_image_render_scene_count()==0);CHECK(cafe_image_verify_count()==0);
 // A visual change must draw exactly once.
 cafe_frame_t now,next;CHECK(cafe_frame_get(a.vm.node,a.vm.index,&now));CHECK(cafe_frame_get(a.vm.node,(uint16_t)(a.vm.index+1),&next));
 if(!same_visual(&now,&next)){tap(&a,CAFE_KEY_OK,&t);CHECK(cafe_image_render_scene_count()>=1);}
 // Title/return must invalidate; the next game sentence redraws.
 hold(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_MENU);for(int i=0;i<5;i++)tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TITLE);cafe_image_reset_counts();for(int ti=0;ti<6&&a.title_sel!=1;ti++)tap(&a,a.title_sel<1?CAFE_KEY_DOWN:CAFE_KEY_UP,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME);CHECK(cafe_image_render_scene_count()>=1);
 // A fresh app/rebind also must redraw (no hash-0 shortcut).
 cafe_image_reset_counts();CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=2000;a.last_activity_ms=2000;tap(&a,CAFE_KEY_OK,&t);for(int ti=0;ti<6&&a.title_sel!=1;ti++)tap(&a,a.title_sel<1?CAFE_KEY_DOWN:CAFE_KEY_UP,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME);CHECK(cafe_image_render_scene_count()>=1);
 // CG path invalidates too: fresh app, controlled seen-only UI fixture.
 memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_image_bands_init(&b));CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=3000;a.last_activity_ms=3000;a.unlock.cg_seen[0]=1;tap(&a,CAFE_KEY_OK,&t);for(int i=0;i<4;i++)tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_OK,&t);tap(&a,CAFE_KEY_OK,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_CG);cafe_image_reset_counts();hold(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_CG_LIST);hold(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GALLERY);hold(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TITLE);for(int ti=0;ti<6&&a.title_sel!=1;ti++)tap(&a,a.title_sel<1?CAFE_KEY_DOWN:CAFE_KEY_UP,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME);CHECK(cafe_image_render_scene_count()>=1);
 cafe_image_bands_free(&b);free(pack);printf("cafe FF visual cache PASS (same node=%u idx=%u->%u)\n",node0,idx0,idx1);return 0;
}
