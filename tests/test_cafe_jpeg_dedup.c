// tests/test_cafe_jpeg_dedup.c — every image uses the segmented stream exactly.
#include "cafe_pack.h"
#include "cafe_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static unsigned char *rf(const char *p,size_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);unsigned char*b=malloc((size_t)s);if(!b||fread(b,1,(size_t)s,f)!=(size_t)s){free(b);fclose(f);return NULL;}fclose(f);*n=(size_t)s;return b;}
int main(int ac,char**av){(void)ac;size_t n=0;unsigned char*b=rf(av[1],&n);CHECK(b&&cafe_pack_set_data(b,n));const uint32_t count=cafe_meta_u32(10);CHECK(count==2284);cafe_bands_t bands;CHECK(cafe_image_bands_init(&bands));uint32_t decoded=0;
 for(uint32_t i=0;i<count;++i){cafe_image_t img;CHECK(cafe_image_get((uint16_t)i,&img));CHECK(cafe_image_verify((uint16_t)i));cafe_jpeg_t st;CHECK(cafe_image_jpeg(&img,&st));CHECK(st.part_count>0);CHECK(cafe_image_render_image(&bands,(uint16_t)i));decoded++;}
 printf("cafe JPEG dedup verify/decode PASS: %u images\n",decoded);cafe_image_bands_free(&bands);free(b);return 0;}
