// Bounded Flash parser and shared zlib workspace. Inflate design is adapted from
// Shinku-Chen/ai-passport PARQUET 0ad8ba41 main/parquet/parquet_inflate.c.
#include "nine_pack.h"
#include "miniz.h"
#include <limits.h>
#include <string.h>

enum { S_NODE, S_FRAME, S_BLOCK, S_VISUAL, S_LAYER, S_VAR, S_COND, S_EFFECT,
       S_EDGE, S_CHAPTER, S_ENDING, S_GALLERY, S_STRING, S_TITLE };
_Static_assert(sizeof(nine_script_header_t) == 256, "script header layout");
_Static_assert(sizeof(nine_node_record_t) == 48, "node layout");
static uint32_t rd(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint32_t crc32_part(uint32_t crc, const uint8_t *p, size_t n) {
    while (n--) { crc ^= *p++; for (unsigned b=0; b<8; b++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u); }
    return crc;
}
static uint32_t crc32_bytes(const uint8_t *p, size_t n) { return ~crc32_part(~0u,p,n); }
static bool range(uint32_t first,uint32_t count,uint32_t total) { return first<=total && count<=total-first; }
static bool table(nine_pack_t *p,unsigned s,uint32_t off,uint32_t count,uint32_t stride) {
    if (off > p->size || (uint64_t)count*stride > p->size-off) return false;
    p->offsets[s]=off; p->counts[s]=count; p->sizes[s]=count*stride; return true;
}
static const uint8_t *row(const nine_pack_t *p,unsigned s,uint32_t i,uint32_t stride) {
    return p && i<p->counts[s] ? p->data+p->offsets[s]+(size_t)i*stride : NULL;
}
const char *nine_pack_string(const nine_pack_t *p,uint32_t off) {
    if (!p || off>=p->sizes[S_STRING]) return NULL;
    const char *s=(const char*)p->data+p->offsets[S_STRING]+off;
    return memchr(s,0,p->sizes[S_STRING]-off) ? s : NULL;
}
const char *nine_pack_title(const nine_pack_t *p) { return p?nine_pack_string(p,p->offsets[S_TITLE]):NULL; }
const char *nine_pack_key(const nine_pack_t *p,uint16_t key) {
    const uint8_t *r=row(p,S_VAR,key,8); return r?nine_pack_string(p,rd(r)):NULL;
}
bool nine_pack_key_persistent(const nine_pack_t *p,uint16_t key) {
    const uint8_t *r=row(p,S_VAR,key,8); return r && (rd(r+4)&NINE_VAR_PERSISTENT)!=0;
}
int nine_pack_find_key(const nine_pack_t *p,const char *name) {
    if(!p || !name) return -1;
    for(uint32_t i=0;i<p->key_count;i++) { const char *s=nine_pack_key(p,(uint16_t)i); if(s && !strcmp(s,name)) return (int)i; }
    return -1;
}
bool nine_pack_node(const nine_pack_t *p,uint16_t i,nine_node_t *o) {
    const uint8_t *r=row(p,S_NODE,i,48); if(!r || !o) return false;
    memset(o,0,sizeof(*o)); o->first_frame=rd(r);o->frame_count=rd(r+4);
    o->first_effect=rd(r+8);o->effect_count=rd(r+12);
    o->first_next=rd(r+16);o->next_count=rd(r+20);
    o->first_choice=rd(r+24);o->choice_count=rd(r+28);
    o->title=rd(r+32);o->chapter=rd(r+36);o->name=rd(r+40);return true;
}
int nine_pack_find_node(const nine_pack_t *p,const char *name) {
    if(!p || !name) return -1;
    nine_node_t n;
    for(uint32_t i=0;i<p->node_count;i++) if(nine_pack_node(p,(uint16_t)i,&n)) {
        const char *s=nine_pack_string(p,n.name);if(s && !strcmp(s,name))return (int)i;
    } return -1;
}
bool nine_pack_frame(const nine_pack_t *p,uint32_t i,nine_frame_t *o) {
    const uint8_t*r=row(p,S_FRAME,i,24);if(!r||!o)return false;
    o->kind=(uint8_t)rd(r);o->visual=rd(r+4);o->text_block=rd(r+8);
    o->text_offset=rd(r+12);o->text_length=(uint16_t)rd(r+16);o->speaker=rd(r+20);return true;
}
bool nine_pack_condition(const nine_pack_t*p,uint32_t i,nine_condition_t*o) {
    const uint8_t*r=row(p,S_COND,i,12);if(!r||!o)return false;
    o->op=(uint8_t)rd(r);o->key=(uint16_t)rd(r+4);o->value=(int32_t)rd(r+8);return true;
}
bool nine_pack_effect(const nine_pack_t*p,uint32_t i,nine_effect_t*o) {
    const uint8_t*r=row(p,S_EFFECT,i,12);if(!r||!o)return false;
    o->add=rd(r)==NINE_EFFECT_ADD;o->key=(uint16_t)rd(r+4);o->value=(int32_t)rd(r+8);return true;
}
bool nine_pack_edge(const nine_pack_t*p,bool choice,uint32_t i,nine_edge_t*o) {
    (void)choice;const uint8_t*r=row(p,S_EDGE,i,24);if(!r||!o)return false;
    const uint32_t to=rd(r);o->to=to==NINE_NONE?NINE_NODE_NONE:(uint16_t)to;
    o->first_condition=rd(r+4);o->condition_count=rd(r+8);o->first_effect=rd(r+12);
    o->effect_count=rd(r+16);o->label=rd(r+20);return true;
}
bool nine_pack_chapter(const nine_pack_t*p,uint32_t i,nine_chapter_t*o) {
    const uint8_t*r=row(p,S_CHAPTER,i,8);if(!r||!o)return false;
    o->title=rd(r);o->node=(uint16_t)rd(r+4);o->preview=0;return true;
}
bool nine_pack_ending(const nine_pack_t*p,uint32_t i,nine_ending_t*o) {
    const uint8_t*r=row(p,S_ENDING,i,12);if(!r||!o)return false;
    o->name=rd(r);o->key=(uint16_t)rd(r+4);o->description=rd(r+8);return true;
}
bool nine_pack_visual(const nine_pack_t*p,uint32_t i,nine_visual_record_t*o) {
    const uint8_t*r=row(p,S_VISUAL,i,8);if(!r||!o)return false;
    o->first_layer=rd(r);o->layer_count=rd(r+4);return true;
}
bool nine_pack_layer(const nine_pack_t*p,uint32_t i,nine_layer_record_t*o) {
    const uint8_t*r=row(p,S_LAYER,i,24);if(!r||!o)return false;
    o->image=rd(r);o->x=(int32_t)rd(r+4);o->y=(int32_t)rd(r+8);
    o->width=rd(r+12);o->height=rd(r+16);o->opacity=rd(r+20);return true;
}
bool nine_pack_gallery(const nine_pack_t*p,uint32_t i,nine_gallery_record_t*o) {
    const uint8_t*r=row(p,S_GALLERY,i,12);if(!r||!o)return false;
    o->name_str=rd(r);o->image=rd(r+4);o->group=rd(r+8);return true;
}
uint32_t nine_pack_gallery_count(const nine_pack_t*p){return p?p->counts[S_GALLERY]:0;}
bool nine_pack_visual_contains(const nine_pack_t*p,uint32_t visual,uint32_t image){
    nine_visual_record_t v;if(!nine_pack_visual(p,visual,&v))return false;
    for(uint32_t i=0;i<v.layer_count;i++){nine_layer_record_t l;if(!nine_pack_layer(p,v.first_layer+i,&l))return false;if(l.image==image&&l.opacity)return true;}return false;
}
static tinfl_decompressor s_inflate;
static bool s_inflate_busy;
size_t nine_inflate(void*dst,size_t cap,const uint8_t*src,size_t size) {
    if(!dst||!src||s_inflate_busy)return SIZE_MAX;
    s_inflate_busy=true;tinfl_init(&s_inflate);size_t in=size,out=cap;
    tinfl_status st=tinfl_decompress(&s_inflate,src,&in,dst,dst,&out,
        TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    s_inflate_busy=false;return st==TINFL_STATUS_DONE && in==size?out:SIZE_MAX;
}
const char *nine_pack_text(nine_pack_t*p,const nine_frame_t*f) {
    if(!p||!f||f->kind!=NINE_FRAME_TEXT)return NULL;
    const uint8_t*r=row(p,S_BLOCK,f->text_block,16);if(!r)return NULL;
    uint32_t off=rd(r),comp=rd(r+4),raw=rd(r+8);
    if(!range(off,comp,p->size)||raw>NINE_TEXT_BLOCK_MAX||!range(f->text_offset,f->text_length,raw))return NULL;
    if(p->cached_block!=f->text_block){
        p->cached_block=NINE_NONE;
        if(nine_inflate(p->text_cache,raw,p->data+off,comp)!=raw || crc32_bytes(p->text_cache,raw)!=rd(r+12))return NULL;
        p->cached_block=f->text_block;
    }
    // Packer stores a NUL following every frame text; never scan outside raw.
    uint32_t end=f->text_offset+f->text_length;
    if(end>=raw || p->text_cache[end])return NULL;
    return (const char*)p->text_cache+f->text_offset;
}

bool nine_pack_open(nine_pack_t*p,const uint8_t*d,size_t size) {
    if(!p||!d||size<256||size>UINT32_MAX||memcmp(d,NINE_SCRIPT_MAGIC,8)||rd(d+8)!=NINE_FORMAT_VERSION||rd(d+12)!=size)return false;
    uint32_t crc=crc32_part(~0u,d,16);const uint8_t zero[4]={0};crc=crc32_part(crc,zero,4);crc=~crc32_part(crc,d+20,size-20);
    if(crc!=rd(d+16))return false;
    memset(p,0,sizeof(*p));p->data=d;p->size=(uint32_t)size;p->identity=crc;p->cached_block=NINE_NONE;
    const uint32_t stride[]={48,24,16,8,24,8,12,12,24,8,12,12};
    for(unsigned s=0;s<12;s++)if(!table(p,s,rd(d+40+s*8),rd(d+36+s*8),stride[s]))return false;
    if(!table(p,S_STRING,rd(d+132),rd(d+136),1))return false;
    p->offsets[S_TITLE]=rd(d+24);p->node_count=p->counts[S_NODE];p->frame_count=p->counts[S_FRAME];
    p->condition_count=p->counts[S_COND];p->effect_count=p->counts[S_EFFECT];p->choice_count=p->next_count=p->counts[S_EDGE];
    p->key_count=p->counts[S_VAR];p->chapter_count=p->counts[S_CHAPTER];p->ending_count=p->counts[S_ENDING];
    p->gallery_count=p->counts[S_GALLERY];p->visual_count=p->counts[S_VISUAL];
    if(!p->node_count||p->node_count>=NINE_NODE_NONE||p->key_count>NINE_KEY_MAX||rd(d+32)>=p->node_count)return false;
    p->start=(uint16_t)rd(d+32);
    const char*ep=nine_pack_string(p,rd(d+20));if(!ep||!nine_pack_title(p)||!nine_pack_string(p,rd(d+28)))return false;
    p->episode=strstr(ep,"kokoiro")?1u:strstr(ep,"sorairo")?2u:strstr(ep,"haruiro")?3u:strstr(ep,"yukiiro")?4u:0u;
    if(!p->episode)return false;
    p->filter_choices=p->episode!=1;
    p->loop_key=NINE_NODE_NONE;p->clear_key=NINE_NODE_NONE;
    for(uint32_t i=0;i<p->key_count;i++){
        const char*s=nine_pack_key(p,(uint16_t)i);if(!s)return false;
        if(!strcmp(s,p->episode==1?"loop_my":"loop1"))p->loop_key=(uint16_t)i;
        if(!strcmp(s,"clear_true"))p->clear_key=(uint16_t)i;
    }
    nine_node_t n;nine_edge_t e;nine_frame_t f;
    for(uint32_t i=0;i<p->node_count;i++){
        if(!nine_pack_node(p,(uint16_t)i,&n)||!range(n.first_frame,n.frame_count,p->frame_count)||!range(n.first_effect,n.effect_count,p->effect_count)||
           !range(n.first_choice,n.choice_count,p->choice_count)||n.choice_count>NINE_CHOICE_MAX||!range(n.first_next,n.next_count,p->next_count)||
           !nine_pack_string(p,n.name)||!nine_pack_string(p,n.title)||(n.chapter!=NINE_NONE&&n.chapter>=p->chapter_count))return false;
    }
    for(uint32_t i=0;i<p->counts[S_EDGE];i++){
        if(!nine_pack_edge(p,true,i,&e)||(e.to!=NINE_NODE_NONE&&e.to>=p->node_count)||!range(e.first_condition,e.condition_count,p->condition_count)||
           !range(e.first_effect,e.effect_count,p->effect_count)||!nine_pack_string(p,e.label))return false;
    }
    for(uint32_t i=0;i<p->condition_count;i++){
        const uint8_t*r=row(p,S_COND,i,12);uint32_t op=rd(r),var=rd(r+4);int32_t val=(int32_t)rd(r+8);
        if(op>NINE_COND_AND || (op>=NINE_COND_EQ&&op<=NINE_COND_GE&&var>=p->key_count)||(op==NINE_COND_AND&&(val<0||val>64)))return false;
    }
    for(uint32_t i=0;i<p->effect_count;i++){
        const uint8_t*r=row(p,S_EFFECT,i,12);if(rd(r)>NINE_EFFECT_ADD||rd(r+4)>=p->key_count)return false;
    }
    for(uint32_t i=0;i<p->counts[S_BLOCK];i++){
        const uint8_t*r=row(p,S_BLOCK,i,16);if(!range(rd(r),rd(r+4),p->size)||!rd(r+8)||rd(r+8)>NINE_TEXT_BLOCK_MAX)return false;
    }
    for(uint32_t i=0;i<p->frame_count;i++){
        const uint8_t*r=row(p,S_FRAME,i,24);nine_pack_frame(p,i,&f);
        if(rd(r)>NINE_FRAME_OP || (f.visual!=NINE_NONE&&f.visual>=p->counts[S_VISUAL])||!nine_pack_string(p,f.speaker))return false;
        if(f.kind==NINE_FRAME_TEXT){const uint8_t*b=row(p,S_BLOCK,f.text_block,16);
            if(!b||rd(r+16)>=NINE_TEXT_MAX||!range(f.text_offset,rd(r+16)+1,rd(b+8)))return false;
        }
    }
    for(uint32_t i=0;i<p->counts[S_VISUAL];i++){nine_visual_record_t v;nine_pack_visual(p,i,&v);if(!range(v.first_layer,v.layer_count,p->counts[S_LAYER])||v.layer_count>32)return false;}
    for(uint32_t i=0;i<p->counts[S_LAYER];i++){nine_layer_record_t l;nine_pack_layer(p,i,&l);if(l.width>4096||l.height>4096||l.opacity>255)return false;}
    for(uint32_t i=0;i<p->chapter_count;i++){nine_chapter_t c;nine_pack_chapter(p,i,&c);if(c.node>=p->node_count||!nine_pack_string(p,c.title))return false;}
    for(uint32_t i=0;i<p->ending_count;i++){nine_ending_t en;nine_pack_ending(p,i,&en);if(en.key>=p->key_count||!nine_pack_string(p,en.name)||!nine_pack_string(p,en.description))return false;}
    for(uint32_t i=0;i<p->counts[S_GALLERY];i++){nine_gallery_record_t g;nine_pack_gallery(p,i,&g);if(!nine_pack_string(p,g.name_str))return false;}
    return true;
}
