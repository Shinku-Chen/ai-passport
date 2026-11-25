// IMA WAV block decoder vs independent ffmpeg reference; exact sample count.
#include "cafe_audio_codec.h"
#include "cafe_audio_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"CHECK failed line %d: %s\n",__LINE__,#c); return 1; } } while(0)
static unsigned char *rf(const char *p, size_t *n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc((size_t)s);if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}fclose(f);*n=(size_t)s;return b;}
static int run(const char *path,uint32_t total,const uint32_t idx[3],const int16_t ref[3],int block_align){
 size_t n=0; unsigned char *b=rf(path,&n); CHECK(b); cafe_ima_state_t st={0}; int16_t out[4096]; uint32_t got=0; size_t off=0; int16_t first[3]={0}; int k=0;
 while(off<n && got<total){size_t nb=(n-off<(size_t)block_align?n-off:(size_t)block_align);size_t m=cafe_ima_decode_block(b+off,nb,&st,out,4096);CHECK(m>0);size_t take=m;if(got+take>total)take=total-got;for(size_t i=0;i<take;i++){uint32_t at=got+(uint32_t)i;if(k<3&&at==idx[k]){first[k]=out[i];k++;}}got+=take;off+=nb;}
 CHECK(got==total); printf("%s samples=%u\n",path,got); for(int i=0;i<3;i++){int d=abs((int)first[i]-(int)ref[i]);printf(" idx%u got=%d ref=%d diff=%d\n",idx[i],first[i],ref[i],d);CHECK(d<=500);} free(b);return 0;}
int main(int ac,char**av){(void)ac;CHECK(run(av[1],1027198,k_title_idx,k_title_ref,1024)==0);CHECK(run(av[2],743959,k_op_idx,k_op_ref,1024)==0);puts("cafe audio codec PASS");return 0;}
