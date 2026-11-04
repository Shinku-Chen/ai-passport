// Source map rows carry a real area value; choosing a row must update
// chosenAreaId, not only a hardcoded zero in the trace formatter.
#include "gal_pack.h"
#include "gal_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures;
#define CHECK(c,...) do{if(!(c)){fprintf(stderr,"FAIL %s:%d: ",__FILE__,__LINE__);fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);failures++;}}while(0)
static uint8_t *load(const char*p,uint32_t*n){FILE*f=fopen(p,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long z=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc(z);if(!b||fread(b,1,z,f)!=(size_t)z)return NULL;fclose(f);*n=(uint32_t)z;return b;}
int main(int c,char**v){if(c<2)return 2;uint32_t n;uint8_t*d=load(v[1],&n);gal_pack_t p;const char*e=NULL;if(!gal_pack_open(&p,d,n,&e)){fprintf(stderr,"%s\n",e);return 1;}uint32_t op=UINT32_MAX;for(uint32_t i=0;i<p.count[GAL_SEC_OPS];++i){const gal_op_t*o=gal_pack_op(&p,i);if(o&&o->op==GAL_OP_MAP){op=i;break;}}CHECK(op!=UINT32_MAX,"no map op");if(op==UINT32_MAX)return 1;int scene=-1;for(uint32_t i=0;i<p.count[GAL_SEC_SCENES];++i){const gal_scene_t*s=gal_pack_scene(&p,(uint16_t)i);if(s&&op>=s->op_start&&op<s->op_start+s->op_count){scene=(int)i;break;}}CHECK(scene>=0,"map scene");const gal_scene_t*s=gal_pack_scene(&p,(uint16_t)scene);gal_vm_t vm;gal_vm_init(&vm,&p);CHECK(gal_vm_start(&vm,(uint16_t)scene,true,false,&e),"start");vm.cursor=(uint16_t)(op-s->op_start);gal_frame_t f;gal_frame_type_t t=gal_vm_read(&vm,&f);CHECK(t==GAL_FRAME_CHOICE,"map frame=%d",t);CHECK(f.row_count>0,"rows=0");int found=0;for(int i=0;i<f.row_count;i++){const gal_row_t*r=&vm.rows[i];if(r->kind==GAL_ROW_MAP){found=1;CHECK(r->value< p.count[GAL_SEC_MAP_AREAS]||r->value!=0,"value=%u",r->value);CHECK(r->to>0,"to=0");}}CHECK(found,"no map row");if(found){const gal_row_t*r=&vm.rows[0];uint16_t val=r->value;CHECK(gal_vm_choose(&vm,0,&e),"choose %s",e?e:"");uint16_t id=gal_vm_var_id(&vm,"chosenAreaId");CHECK(id!=GAL_NONE,"var");if(id!=GAL_NONE){const gal_area_t*a=gal_pack_area(&p,val);CHECK(a!=NULL,"area");if(a)CHECK(vm.vars[id].str==a->id,"chosenAreaId=%u areaid=%u",vm.vars[id].str,a->id);}}
printf("test_gal_map_value: %s\n",failures?"FAIL":"PASS");free(d);return failures?1:0;}
