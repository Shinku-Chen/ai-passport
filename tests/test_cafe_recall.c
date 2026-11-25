// tests/test_cafe_recall.c — controlled CG sets through the real recall page.
// Controlled seen bits are UI fixtures only; they are not a normal-play witness.
#include "cafe_app.h"
#include "cafe_pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
extern int audit_ui_list_count[CAFE_LIST_COUNT];
extern int audit_ui_list_selected[CAFE_LIST_COUNT];
extern char audit_ui_list_labels[CAFE_LIST_COUNT][CAFE_LIST_MAX][64];
extern char audit_ui_footer[CAFE_PAGE_COUNT][96];

static unsigned char *read_file(const char *path,size_t *n){FILE*f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc((size_t)s);if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}fclose(f);*n=(size_t)s;return b;}
static void tap(cafe_app_t*a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_key(a,k,false,*t+40);*t+=80;}
static void long_key(cafe_app_t*a,uint8_t k,uint32_t*t){cafe_app_key(a,k,true,*t);cafe_app_tick(a,*t+520,520);*t+=520;cafe_app_key(a,k,false,*t);*t+=40;}
static int open_recall_ui(cafe_app_t*a,uint32_t*t){tap(a,CAFE_KEY_OK,t);for(int i=0;i<4;i++)tap(a,CAFE_KEY_DOWN,t);tap(a,CAFE_KEY_OK,t);tap(a,CAFE_KEY_DOWN,t);tap(a,CAFE_KEY_OK,t);return a->page==CAFE_PAGE_RECALL;}
static void set_seen(cafe_app_t*a,int n){memset(a->unlock.cg_seen,0,sizeof(a->unlock.cg_seen));for(int i=0;i<n;i++)a->unlock.cg_seen[i>>3]|=(uint8_t)(1u<<(i&7));}
static const char *expected_label(int group){cafe_gallery_t g;if(!cafe_gallery_get((uint16_t)group,&g))return NULL;return cafe_pack_string(g.label_off);}

int main(int argc,char**argv){
 CHECK(argc==2);size_t n=0;unsigned char*pack=read_file(argv[1],&n);CHECK(pack&&cafe_pack_set_data(pack,n));
 cafe_ui_t ui;cafe_bands_t b;cafe_app_t a;uint32_t t=1000;
 memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=1000;a.last_activity_ms=1000;
 // 0: clear empty state, not blank and no fake unlock.
 set_seen(&a,0);CHECK(open_recall_ui(&a,&t));CHECK(audit_ui_list_count[CAFE_LIST_RECALL]==1);CHECK(strstr(audit_ui_list_labels[CAFE_LIST_RECALL][0],"尚未解锁任何 CG")!=NULL);CHECK(strstr(audit_ui_footer[CAFE_PAGE_RECALL],"继续阅读")!=NULL);CHECK(a.unlock.progress==0);tap(&a,CAFE_KEY_OK,&t);CHECK(ui.notice_seq>0);CHECK(a.unlock.progress==0);

 // 1: real label and selected-row UI; OK starts exact first_node, preview does not write progress.
 memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=1000;a.last_activity_ms=1000;set_seen(&a,1);CHECK(open_recall_ui(&a,&t));
 CHECK(audit_ui_list_count[CAFE_LIST_RECALL]==1);CHECK(audit_ui_list_selected[CAFE_LIST_RECALL]==0);
  CHECK(expected_label(0) && strcmp(audit_ui_list_labels[CAFE_LIST_RECALL][0], expected_label(0)) == 0);CHECK(audit_ui_list_labels[CAFE_LIST_RECALL][0][0]!=0);
CHECK(strstr(audit_ui_footer[CAFE_PAGE_RECALL],"1 / 1")!=NULL);cafe_gallery_t g0;CHECK(cafe_gallery_get(0,&g0));const uint32_t before=a.unlock.progress;tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TRANSITION);CHECK(a.vm.node==g0.first_node);CHECK(a.vm.progression==CAFE_PROGRESSION_CHAPTER);CHECK(!a.auto_mode&&!a.fast_forward);CHECK(a.unlock.progress==before);

 // 16: two/three pages, short and long movement, first/last visible row and exact labels.
 memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=1000;a.last_activity_ms=1000;set_seen(&a,16);CHECK(open_recall_ui(&a,&t));
 CHECK(audit_ui_list_count[CAFE_LIST_RECALL]==6);CHECK(audit_ui_list_selected[CAFE_LIST_RECALL]==0);CHECK(strcmp(audit_ui_list_labels[CAFE_LIST_RECALL][0],expected_label(0))==0);
 for(int i=0;i<5;i++)tap(&a,CAFE_KEY_DOWN,&t);CHECK(audit_ui_list_selected[CAFE_LIST_RECALL]==5);CHECK(strcmp(audit_ui_list_labels[CAFE_LIST_RECALL][5],expected_label(5))==0);
 tap(&a,CAFE_KEY_DOWN,&t);CHECK(a.recall_sel==6);CHECK(audit_ui_list_selected[CAFE_LIST_RECALL]==0);CHECK(strcmp(audit_ui_list_labels[CAFE_LIST_RECALL][0],expected_label(6))==0);CHECK(strstr(audit_ui_footer[CAFE_PAGE_RECALL],"2 / 3")!=NULL);
 while(a.recall_sel<15)tap(&a,CAFE_KEY_DOWN,&t);CHECK(a.recall_sel==15);CHECK(audit_ui_list_selected[CAFE_LIST_RECALL]==3);CHECK(strcmp(audit_ui_list_labels[CAFE_LIST_RECALL][3],expected_label(15))==0);CHECK(strstr(audit_ui_footer[CAFE_PAGE_RECALL],"16 / 16")!=NULL);
 long_key(&a,CAFE_KEY_UP,&t);CHECK(a.recall_sel==14);long_key(&a,CAFE_KEY_DOWN,&t);CHECK(a.recall_sel==15);

 // >16: all 20 seen; global last item is visible on page 4, row 1.
 memset(&ui,0,sizeof ui);memset(&b,0,sizeof b);CHECK(cafe_app_init(&a,&ui,&b));a.now_ms=1000;a.last_activity_ms=1000;set_seen(&a,20);CHECK(open_recall_ui(&a,&t));
 while(a.recall_sel<19)tap(&a,CAFE_KEY_DOWN,&t);CHECK(a.recall_sel==19);CHECK(audit_ui_list_count[CAFE_LIST_RECALL]==2);CHECK(audit_ui_list_selected[CAFE_LIST_RECALL]==1);CHECK(strcmp(audit_ui_list_labels[CAFE_LIST_RECALL][1],expected_label(19))==0);CHECK(audit_ui_list_labels[CAFE_LIST_RECALL][1][0]!=0);CHECK(strstr(audit_ui_footer[CAFE_PAGE_RECALL],"20 / 20")!=NULL);CHECK(strstr(audit_ui_footer[CAFE_PAGE_RECALL],"4 / 4")!=NULL);
 CHECK(a.unlock.progress==0); // fixture only; no permanent progress was granted
 cafe_gallery_t g19;CHECK(cafe_gallery_get(19,&g19));const uint32_t before19=a.unlock.progress;tap(&a,CAFE_KEY_OK,&t);CHECK(a.page==CAFE_PAGE_TRANSITION);CHECK(a.vm.node==g19.first_node);CHECK(a.vm.progression==CAFE_PROGRESSION_CHAPTER);CHECK(!a.auto_mode&&!a.fast_forward);CHECK(a.unlock.progress==before19);
 free(pack);printf("cafe recall UI PASS\n");return 0;
}
