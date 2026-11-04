/* Independent negative/roundtrip tests for the three codec functions. */
#include "anemoi_pack.h"
#include "anemoi_script.h"
#include "anemoi_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static uint32_t crc32_ieee(const uint8_t *data, size_t n) {
    uint32_t c=0xFFFFFFFFu;
    for (size_t i=0;i<n;i++) { c^=data[i]; for(int b=0;b<8;b++) c=(c>>1)^(0xEDB88320u & (uint32_t)(-(int32_t)(c&1u))); }
    return ~c;
}
static void fix_crc(uint8_t *b, size_t n) {
    uint32_t c=crc32_ieee(b,n-4); b[n-4]=(uint8_t)c; b[n-3]=(uint8_t)(c>>8); b[n-2]=(uint8_t)(c>>16); b[n-1]=(uint8_t)(c>>24);
}
static void put16(uint8_t *b,size_t o,uint16_t v){b[o]=(uint8_t)v;b[o+1]=(uint8_t)(v>>8);}
static void put32(uint8_t *b,size_t o,uint32_t v){b[o]=(uint8_t)v;b[o+1]=(uint8_t)(v>>8);b[o+2]=(uint8_t)(v>>16);b[o+3]=(uint8_t)(v>>24);}

static anem_slot_t make_slot(void) {
    anem_slot_t s; memset(&s,0,sizeof s);
    s.valid=true; s.mode=0; s.progression=0; s.chapter=7; s.root_chapter=7; s.cursor=123;
    s.page=2; s.speaker=1; s.text=2; s.regular_bg=3; s.cg=4; s.changed=5; s.changed_cg=1;
    s.body_count=1; s.saved_at=1700000000u;
    s.bodies[0].name=10; s.bodies[0].dress=11; s.bodies[0].valid=true;
    s.dress[0]=2; s.pose[0]=3;
    s.var_count=2;
    s.vars[0].num=42; s.vars[0].text=0; s.vars[0].kind=ANEM_VAL_NUM; s.vars[0].len=0;
    s.vars[1].num=0; s.vars[1].text=12; s.vars[1].kind=ANEM_VAL_STR; s.vars[1].len=0;
    return s;
}
static anem_progress_t make_progress(void) {
    anem_progress_t p; memset(&p,0,sizeof p); p.version=ANEM_PROGRESS_VERSION; p.level=4; p.var_count=2;
    p.vars[0].num=1; p.vars[0].kind=ANEM_VAL_NUM;
    p.vars[1].text=12; p.vars[1].kind=ANEM_VAL_STR;
    p.cg[0]=0x5a; p.ending[0]=0x3c; return p;
}
static void expect_slot_bad(const char *name, uint8_t *b, size_t n, const anem_slot_t *valid) {
    (void)valid;
    anem_slot_t out; memset(&out,0xA5,sizeof out); const anem_slot_t before=out;
    int ok=anem_slot_decode(b,n,&out);
    if (ok || memcmp(&out,&before,sizeof out)!=0) { printf("BAD_SLOT %s ok=%d changed=%d\n",name,ok,memcmp(&out,&before,sizeof out)!=0); fails++; }
}
static void expect_progress_bad(const char *name, uint8_t *b, size_t n) {
    anem_progress_t out; memset(&out,0xA5,sizeof out); const anem_progress_t before=out;
    int ok=anem_progress_decode(b,n,&out);
    if (ok || memcmp(&out,&before,sizeof out)!=0) { printf("BAD_PROGRESS %s ok=%d changed=%d\n",name,ok,memcmp(&out,&before,sizeof out)!=0); fails++; }
}
int main(int argc,char **argv) {
    const char *path=argc>1?argv[1]:"pack/anemoi_pack.bin";
    FILE *f=fopen(path,"rb"); if(!f){perror(path);return 2;} fseek(f,0,SEEK_END); long pn=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *packbuf=malloc((size_t)pn); if(!packbuf||fread(packbuf,1,(size_t)pn,f)!=(size_t)pn)return 2; fclose(f);
    anem_pack_t pack; CHECK(anem_pack_open(&pack,packbuf,(size_t)pn));
    anem_slot_t s=make_slot(); uint8_t sb[4608]; size_t sn=anem_slot_blob_size(); CHECK(sn<=sizeof sb); CHECK(anem_slot_encode(&s,sb,sn));
    anem_slot_t d; memset(&d,0x5a,sizeof d); CHECK(anem_slot_decode(sb,sn,&d)); CHECK(d.valid&&d.chapter==7&&d.page==2&&d.body_count==1&&d.var_count==2&&d.vars[0].num==42&&d.vars[1].kind==ANEM_VAL_STR);
    anem_progress_t p=make_progress(); uint8_t pb[4608]; size_t pn2=anem_progress_blob_size(); CHECK(pn2<=sizeof pb); CHECK(anem_progress_encode(&p,pb,pn2));
    /* Max legal slot: var_count=512, body_count=3, page/cursor boundaries. */
    anem_slot_t sm=make_slot(); sm.var_count=ANEM_VAR_MAX; sm.body_count=ANEM_BODY_MAX; sm.page=ANEM_PAGE_MAX; sm.cursor=0xFFFFFFu;
    for(int i=0;i<ANEM_VAR_MAX;i++){sm.vars[i].kind=ANEM_VAL_NUM;sm.vars[i].num=i;sm.vars[i].len=0;}
    for(int i=0;i<ANEM_BODY_MAX;i++){sm.bodies[i].name=(uint16_t)(20+i);sm.bodies[i].valid=true;}
    uint8_t smb[4608]; CHECK(anem_slot_encode(&sm,smb,sn)); anem_slot_t smd; CHECK(anem_slot_decode(smb,sn,&smd)); CHECK(smd.var_count==ANEM_VAR_MAX && smd.body_count==ANEM_BODY_MAX && smd.page==ANEM_PAGE_MAX && smd.cursor==0xFFFFFFu && smd.vars[511].num==511);
    anem_progress_t pm=make_progress(); pm.var_count=ANEM_VAR_MAX; for(int i=0;i<ANEM_VAR_MAX;i++){pm.vars[i].kind=ANEM_VAL_NUM;pm.vars[i].num=i;}
    uint8_t pmb[4608]; CHECK(anem_progress_encode(&pm,pmb,pn2)); anem_progress_t pmd; CHECK(anem_progress_decode(pmb,pn2,&pmd)); CHECK(pmd.var_count==ANEM_VAR_MAX && pmd.vars[511].num==511);

    anem_progress_t pd; memset(&pd,0x5a,sizeof pd); CHECK(anem_progress_decode(pb,pn2,&pd)); CHECK(pd.version==ANEM_PROGRESS_VERSION&&pd.level==4&&pd.var_count==2&&pd.cg[0]==0x5a&&pd.ending[0]==0x3c&&pd.vars[0].num==1);
    uint8_t b[4608];
    memcpy(b,sb,sn); b[sn-1]^=1; expect_slot_bad("crc",b,sn,&s);
    /* own extra: truncated by one byte, expected reject with caller unchanged. */
    { anem_slot_t tout; memset(&tout,0xA5,sizeof tout); const anem_slot_t tbefore=tout; int tok=anem_slot_decode(sb,sn-1,&tout); if(tok||memcmp(&tout,&tbefore,sizeof tout)!=0){printf("BAD_SLOT truncate ok=%d changed=%d\n",tok,memcmp(&tout,&tbefore,sizeof tout)!=0);fails++;} }
    memcpy(b,sb,sn); put32(b,0,0); expect_slot_bad("magic",b,sn,&s);
    memcpy(b,sb,sn); put16(b,4,9); expect_slot_bad("version",b,sn,&s);
    memcpy(b,sb,sn); put16(b,6,(uint16_t)(sn-4)); expect_slot_bad("size",b,sn,&s);
    memcpy(b,sb,sn); b[8]=0; fix_crc(b,sn); expect_slot_bad("valid_false",b,sn,&s);
    memcpy(b,sb,sn); b[9]=2; fix_crc(b,sn); expect_slot_bad("mode",b,sn,&s);
    memcpy(b,sb,sn); b[10]=2; fix_crc(b,sn); expect_slot_bad("progression",b,sn,&s);
    memcpy(b,sb,sn); put16(b,12,0); fix_crc(b,sn); expect_slot_bad("chapter0",b,sn,&s);
    memcpy(b,sb,sn); put32(b,16,0x1000000u); fix_crc(b,sn); expect_slot_bad("cursor",b,sn,&s);
    memcpy(b,sb,sn); put16(b,20,65); fix_crc(b,sn); expect_slot_bad("page",b,sn,&s);
    memcpy(b,sb,sn); b[11]=4; fix_crc(b,sn); expect_slot_bad("body_count",b,sn,&s);
    memcpy(b,sb,sn); put16(b,172,1); b[176]=0; fix_crc(b,sn); expect_slot_bad("body_invalid_name",b,sn,&s);
    memcpy(b,sb,sn); put16(b,38,513); fix_crc(b,sn); expect_slot_bad("var_count",b,sn,&s);
    memcpy(b,sb,sn); b[190+6]=9; fix_crc(b,sn); expect_slot_bad("value_kind",b,sn,&s);
    memcpy(b,sb,sn); b[190+7]=9; /* ARR len>8 */ b[190+6]=ANEM_VAL_ARR; fix_crc(b,sn); expect_slot_bad("value_arrlen",b,sn,&s);
    /* slot_valid only checks vars < var_count; trailing var is preserved but not semantic. */
    memcpy(b,sb,sn); b[190+500*8+6]=9; fix_crc(b,sn); { anem_slot_t td; memset(&td,0xA5,sizeof td); CHECK(anem_slot_decode(b,sn,&td)); CHECK(td.var_count==2); }
    memcpy(b,pb,pn2); b[pn2-1]^=1; expect_progress_bad("p_crc",b,pn2);
    memcpy(b,pb,pn2); put32(b,0,0); expect_progress_bad("p_magic",b,pn2);
    memcpy(b,pb,pn2); put16(b,4,9); expect_progress_bad("p_version",b,pn2);
    memcpy(b,pb,pn2); put16(b,6,(uint16_t)(pn2-4)); expect_progress_bad("p_size",b,pn2);
    memcpy(b,pb,pn2); put16(b,10,513); fix_crc(b,pn2); expect_progress_bad("p_var_count",b,pn2);
    memcpy(b,pb,pn2); b[44+6]=9; fix_crc(b,pn2); expect_progress_bad("p_value_kind",b,pn2);
    memcpy(b,pb,pn2); b[44+7]=9; b[44+6]=ANEM_VAL_ARR; fix_crc(b,pn2); expect_progress_bad("p_value_arrlen",b,pn2);
    memcpy(b,pb,pn2); b[44+500*8+6]=9; fix_crc(b,pn2); expect_progress_bad("p_trailing_value",b,pn2);
    /* from_script clears all fields including cg/ending, sets version. */
    static anem_script_t script; CHECK(anem_script_init(&script,&pack)); script.st.var_count=0;
    anem_progress_t from; memset(&from,0xA5,sizeof from); anem_progress_from_script(&from,&script);
    CHECK(from.version==ANEM_PROGRESS_VERSION && from.var_count==0);
    for(size_t i=0;i<sizeof(from.cg);i++) CHECK(from.cg[i]==0);
    for(size_t i=0;i<sizeof(from.ending);i++) CHECK(from.ending[i]==0);
    /* Nonzero g:/z: extraction: use pack's g:30 when present. */
    { uint16_t vid=0; if(anem_var_id_by_name(&pack,"g:30",&vid) && vid<ANEM_VAR_MAX) {
        CHECK(anem_script_init(&script,&pack)); script.st.var_count=(uint16_t)(vid+1);
        memset(script.st.vars,0,sizeof script.st.vars);
        script.st.vars[vid].num=77; script.st.vars[vid].kind=ANEM_VAL_NUM;
        memset(&from,0xA5,sizeof from); anem_progress_from_script(&from,&script);
        CHECK(from.var_count==vid+1); CHECK(from.vars[vid].num==77);
        for(size_t i=0;i<sizeof(from.cg);i++) CHECK(from.cg[i]==0);
      } }
    free(packbuf);
    printf("test_codec_negative: %s (%d failures)\n",fails?"FAIL":"PASS",fails);
    return fails?1:0;
}
