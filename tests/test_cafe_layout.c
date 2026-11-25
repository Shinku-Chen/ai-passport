// tests/test_cafe_layout.c — app text pages use the real 16/20px layout.
#include "cafe_app.h"
#include "cafe_pack.h"
#include "gal_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#c); return 1; } } while(0)
static unsigned char *rf(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc((size_t)s);if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}fclose(f);*n=(size_t)s;return b;}
static void tap(cafe_app_t*a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_key(a,k,false,*t+40);*t+=80;}
static int find_longest(uint16_t *node,uint16_t *index,size_t *len){size_t best=0;for(uint16_t n=0;n<cafe_meta_u32(0);++n){cafe_node_t nd;if(!cafe_node_get(n,&nd))continue;for(uint16_t i=0;i<nd.frames;++i){cafe_frame_t f;if(!cafe_frame_get(n,i,&f)||f.kind!=0)continue;size_t l=strlen(f.text);if(l>best){best=l;*node=n;*index=i;}}}*len=best;return best>0;}
static int check_pages(cafe_app_t*a,cafe_ui_t*ui,uint32_t*t,int large){
 const int units=large?CAFE_TEXT_LARGE_UNITS:CAFE_TEXT_SMALL_UNITS;const int lines=large?CAFE_TEXT_LARGE_LINES:CAFE_TEXT_SMALL_LINES;
 int expected=0;gal_text_page_count(a->current_text,units,lines,&expected);CHECK(expected>=1);CHECK(a->frame_pages==(uint16_t)expected);
 char joined[4096];size_t used=0;joined[0]=0;
 for(int p=0;p<expected;++p){CHECK(a->frame_page==(uint16_t)p);CHECK(ui->text[0]);size_t l=strlen(ui->text);CHECK(used+l+1<sizeof joined);memcpy(joined+used,ui->text,l);used+=l;joined[used]=0;if(p+1<expected){tap(a,CAFE_KEY_OK,t);CHECK(a->frame_page==p+1);}}
 CHECK(strcmp(joined,a->current_text)==0);
 return 0;
}
int main(int ac,char**av){(void)ac;size_t n=0;unsigned char*pack=rf(av[1],&n);CHECK(pack&&cafe_pack_set_data(pack,n));uint16_t node=0,idx=0;size_t maxlen=0;CHECK(find_longest(&node,&idx,&maxlen));CHECK(maxlen>100);
 cafe_store_init();cafe_save_t s;memset(&s,0,sizeof s);s.occupied=true;s.node=node;s.index=idx;s.progression=0;s.last_complete=0xFF;snprintf(s.chapter,sizeof s.chapter,"layout");CHECK(cafe_store_save_auto(&s));
 cafe_ui_t ui;cafe_bands_t b;cafe_app_t a;uint32_t t=1000;memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=1000;a.last_activity_ms=1000;tap(&a,CAFE_KEY_OK,&t);while(a.title_sel!=1)tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME);CHECK(a.vm.node==node&&a.vm.index==idx);CHECK(check_pages(&a,&ui,&t,0)==0);
 // Return to title and reload, then test the 20px layout path.
 cafe_save_t save;snprintf(save.chapter,sizeof save.chapter,"layout");save.occupied=true;save.node=node;save.index=idx;save.progression=0;save.last_complete=0xFF;CHECK(cafe_store_save_auto(&save));memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));a.settings.font_large=true;cafe_ui_set_font_large(&ui,true);a.now_ms=2000;a.last_activity_ms=2000;tap(&a,CAFE_KEY_OK,&t);while(a.title_sel!=1)tap(&a,CAFE_KEY_DOWN,&t);tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_GAME);CHECK(check_pages(&a,&ui,&t,1)==0);
 printf("cafe layout pages PASS: node=%u idx=%u chars=%zu 16px+20px\n",node,idx,maxlen);free(pack);return 0;}
