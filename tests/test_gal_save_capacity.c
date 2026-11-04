// Encode-length witness for the 31-slot NVS capacity contract. This is a
// real production encoder run, not a raw-size estimate.
#include "gal_store.h"
#include "gal_pack.h"
#include "gal_save.h"
#include "gal_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t *load(const char *p, uint32_t *n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long z=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc((size_t)z);if(!b||fread(b,1,(size_t)z,f)!=(size_t)z)return NULL;fclose(f);*n=(uint32_t)z;return b;}
int main(int argc,char**argv){
 if(argc<2)return 2;uint32_t n=0;uint8_t*d=load(argv[1],&n);gal_pack_t p;const char*e=NULL;if(!gal_pack_open(&p,d,n,&e)){fprintf(stderr,"pack %s\n",e);return 1;}
 static gal_vm_t base;static gal_vm_t big;gal_vm_init(&base,&p);if(!gal_vm_start(&base,(uint16_t)p.meta[1],true,false,&e)){fprintf(stderr,"start %s\n",e);return 1;}
 // Ordinary state: a few text advances, default variables.
 for(int i=0;i<12;i++){gal_frame_t f;if(gal_vm_read(&base,&f)==GAL_FRAME_TEXT)gal_vm_advance(&base);else break;}
 static uint8_t blob[GAL_SAVE_MAX];size_t ordinary=0;gal_save_ui_t ui={.scene_mode=0,.page=0,.choice_row=0};
 int ok=gal_save_encode(&base,&ui,blob,sizeof(blob),&ordinary);
 printf("ordinary=%zu ordinary_ok=%d\n",ordinary,ok);
 big=base;
 // Known maximal legal source state: observed max 33 sprite instances, 3
 // bodies, 168 book keys, 32 stack, 8 pending, 32 active rows, all variables.
 memset(big.sprites,0,sizeof(big.sprites));big.sprite_count=33;
 for(int i=0;i<33;i++){big.sprites[i].used=1;big.sprites[i].actor=0;big.sprites[i].sub=0;big.sprites[i].image=1;big.sprites[i].x=(int16_t)(50+i);big.sprites[i].y=50;big.sprites[i].scale100=100;big.sprites[i].order=(uint16_t)(i+1);big.sprites[i].visible=1;}
 memset(big.bodies,0,sizeof(big.bodies));big.body_count=3;
 for(int i=0;i<3;i++){big.bodies[i].used=1;big.bodies[i].actor=0;big.bodies[i].sub=0;big.bodies[i].image=(uint16_t)(i+1);big.bodies[i].x=(int16_t)(40+i);big.bodies[i].y=50;big.bodies[i].scale100=100;big.bodies[i].order=(uint16_t)(100+i);big.bodies[i].visible=1;}
 memset(big.book,0,sizeof(big.book));memset(big.book_order,0,sizeof(big.book_order));big.book_count=168;
 for(int i=0;i<168;i++){big.book[i]=1;big.book_order[i]=(uint8_t)i;}
 big.stack_count=GAL_MAX_STACK;for(int i=0;i<GAL_MAX_STACK;i++){big.stack[i][0]=0;big.stack[i][1]=0;}
 big.pending_count=GAL_MAX_PENDING;
 for(int i=0;i<GAL_MAX_PENDING;i++){big.pending[i].used=1;big.pending[i].text_str=0;big.pending[i].to=0;big.pending[i].end=0;big.pending[i].menu=(uint16_t)i;}
 big.active_valid=1;big.active_chapter=0;big.active_cursor=0;big.active_count=GAL_MAX_ROWS;
 for(int i=0;i<GAL_MAX_ROWS;i++){big.active_rows[i].kind=GAL_ROW_ITEM;big.active_rows[i].text_off=0;big.active_rows[i].text_len=1;big.active_rows[i].to=0;big.active_rows[i].end=0;}
 for(uint16_t i=0;i<big.var_count;i++){big.vars[i].type=GAL_VAL_NUM;big.vars[i].num=(float)(i+1);}
 size_t maxlen=0;ok=gal_save_encode(&big,&ui,blob,sizeof(blob),&maxlen);
 printf("max=%zu max_ok=%d cap=%u slot_count=%d\n",maxlen,ok,(unsigned)GAL_SAVE_MAX,GAL_SLOT_COUNT+1);
 printf("max_payload_31=%llu\n",(unsigned long long)maxlen*31u);
    // 31 real encoder results for a legal state sweep (slot distribution witness).
    printf("slot_lengths=");
    for(int i=0;i<31;i++){
        gal_vm_t v=base;
        int bn=(i*7)%169;
        memset(v.book,0,sizeof(v.book));memset(v.book_order,0,sizeof(v.book_order));
        v.book_count=(uint16_t)bn;
        for(int k=0;k<bn;k++){v.book[k]=1;v.book_order[k]=(uint8_t)k;}
        int sn=i%34;
        memset(v.sprites,0,sizeof(v.sprites));v.sprite_count=(uint8_t)sn;
        for(int k=0;k<sn;k++){v.sprites[k].used=1;v.sprites[k].actor=0;v.sprites[k].sub=0;v.sprites[k].image=1;v.sprites[k].x=50;v.sprites[k].y=50;v.sprites[k].scale100=100;v.sprites[k].order=(uint16_t)k;v.sprites[k].visible=1;}
        size_t l=0;int e=gal_save_encode(&v,&ui,blob,sizeof(blob),&l);
        printf("%s%zu%s",i?",":"",l,e?"":"!");
    }
    putchar(10);
 free(d);return ok?0:1;
}
