// tests/test_cafe_jpeg_guard.c — dedup pool/desc CRC and literal CRC fail closed.
#include "cafe_pack.h"
#include "cafe_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static unsigned char *rf(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc((size_t)s);if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}fclose(f);*n=(size_t)s;return b;}
static int sec_off(const unsigned char*b,uint32_t type,uint32_t*off,uint32_t*size){uint32_t n=*(const uint32_t*)(b+20);for(uint32_t i=0;i<n;i++){uint32_t t=*(const uint32_t*)(b+128+i*24);if(t==type){*off=*(const uint32_t*)(b+128+i*24+4);*size=*(const uint32_t*)(b+128+i*24+8);return 1;}}return 0;}
int main(int ac,char**av){(void)ac;size_t n=0;unsigned char*b=rf(av[1],&n);CHECK(b&&cafe_pack_set_data(b,n));uint32_t off,sz;
 CHECK(sec_off(b,27,&off,&sz));unsigned char *bad=malloc(n);CHECK(bad);memcpy(bad,b,n);bad[off+sz/2]^=1;CHECK(!cafe_pack_set_data(bad,n));free(bad);
 CHECK(sec_off(b,28,&off,&sz));bad=malloc(n);CHECK(bad);memcpy(bad,b,n);bad[off+sz/2]^=1;CHECK(!cafe_pack_set_data(bad,n));free(bad);
 CHECK(cafe_pack_set_data(b,n));uint32_t io,isz,doff,dsz;CHECK(sec_off(b,18,&io,&isz));CHECK(sec_off(b,19,&doff,&dsz));cafe_image_t img;CHECK(cafe_image_get(0,&img));bad=malloc(n);CHECK(bad);memcpy(bad,b,n);CHECK(img.off<dsz);bad[doff+img.off+img.len/2]^=1;CHECK(cafe_pack_set_data(bad,n));CHECK(!cafe_image_verify(0));free(bad);
 cafe_pack_set_data(b,n);CHECK(cafe_image_verify(0));printf("cafe JPEG guard PASS\n");free(b);return 0;}
