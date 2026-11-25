// Interpreter of the already-converted Watch graph, not the original PC VM.
#include "nine_vm.h"
#include "nine_crc32.h"
#include <limits.h>
#include <string.h>

static bool has(const uint8_t *bits,uint16_t key){return (bits[key/8]&(1u<<(key%8)))!=0;}
static void mark(uint8_t *bits,uint16_t key){bits[key/8]|=(uint8_t)(1u<<(key%8));}
static const nine_view_t *error(nine_vm_t*v,const char*s){memset(&v->view,0,sizeof(v->view));v->view.kind=NINE_VM_ERROR;v->view.error=s;return &v->view;}
static void remember(nine_vm_t*v){
    if(v->history_count==NINE_HISTORY_MAX){memmove(v->history,v->history+1,(NINE_HISTORY_MAX-1)*sizeof(v->history[0]));v->history_count--;}
    v->history[v->history_count++]=v->state;
}
int32_t nine_vm_condition_value(const nine_vm_t*v,uint16_t key){
    if(!v||!v->pack||key>=v->pack->key_count)return 0;
    // The source overlays just one loop flag, not all persistent variables.
    if(key==v->pack->loop_key)return v->state.progress[key]?1:0;
    return v->state.flag[key];
}
bool nine_vm_matches(const nine_vm_t*v,uint32_t first,uint32_t count){
    bool stack[64];unsigned depth=0;
    if(!v||!v->pack||first>v->pack->condition_count||count>v->pack->condition_count-first)return false;
    if(!count)return true;
    for(uint32_t i=0;i<count;i++){
        nine_condition_t c;if(!nine_pack_condition(v->pack,first+i,&c))return false;
        if(c.op==NINE_COND_AND){
            if(c.value<0||(unsigned)c.value>depth)return false;
            bool out=true;for(int32_t j=0;j<c.value;j++)out=stack[--depth]&&out;
            if(depth==64)return false;
            stack[depth++]=out;continue;
        }
        if(depth==64)return false;
        int32_t cur=nine_vm_condition_value(v,c.key);bool out;
        switch(c.op){
        case NINE_COND_TRUE:out=true;break;
        case NINE_COND_EQ:out=cur==c.value;break;case NINE_COND_NE:out=cur!=c.value;break;
        case NINE_COND_LT:out=cur<c.value;break;case NINE_COND_LE:out=cur<=c.value;break;
        case NINE_COND_GT:out=cur>c.value;break;case NINE_COND_GE:out=cur>=c.value;break;
        default:return false;
        }stack[depth++]=out;
    }return depth==1&&stack[0];
}
static bool apply(nine_vm_t*v,uint32_t first,uint32_t count){
    for(uint32_t i=0;i<count;i++){
        nine_effect_t ef;if(!nine_pack_effect(v->pack,first+i,&ef)||ef.key>=v->pack->key_count)return false;
        bool permanent=nine_pack_key_persistent(v->pack,ef.key);
        if(permanent&&v->state.preview)continue;
        int32_t *values=permanent?v->state.progress:v->state.flag;
        int64_t val=ef.value;if(ef.add)val+=values[ef.key];
        if(val<INT32_MIN||val>INT32_MAX)return false;
        values[ef.key]=(int32_t)val;mark(permanent?v->state.progress_set:v->state.flag_set,ef.key);
    }return true;
}
bool nine_vm_init(nine_vm_t*v,nine_pack_t*p){
    if(!v||!p||!p->data)return false;
    memset(v,0,sizeof(*v));v->pack=p;v->state.node=p->start;return true;
}
bool nine_vm_start(nine_vm_t*v,uint16_t node,bool preview){
    if(!v||!v->pack)return false;
    if(node==NINE_NODE_NONE)node=v->pack->start;
    if(node>=v->pack->node_count)return false;
    memset(v->state.flag,0,sizeof(v->state.flag));memset(v->state.flag_set,0,sizeof(v->state.flag_set));
    v->state.node=node;v->state.index=0;v->state.preview=preview;v->history_count=0;
    return nine_vm_locate(v)->kind!=NINE_VM_ERROR;
}
const nine_view_t *nine_vm_locate(nine_vm_t*v){
    if(!v||!v->pack)return NULL;
    for(unsigned step=0;step<500;step++){
        memset(&v->view,0,sizeof(v->view));v->view.node=v->state.node;v->view.index=v->state.index;
        if(v->state.node==NINE_NODE_NONE){v->view.kind=NINE_VM_END;return &v->view;}
        nine_node_t n;if(!nine_pack_node(v->pack,v->state.node,&n))return error(v,"missing node");
        if(v->state.index>n.frame_count)return error(v,"frame cursor out of bounds");
        if(v->state.index<n.frame_count){
            nine_frame_t f;v->view.frame=n.first_frame+v->state.index;
            if(!nine_pack_frame(v->pack,v->view.frame,&f))return error(v,"missing frame");
            v->view.kind=f.kind==NINE_FRAME_OP?NINE_VM_OP:NINE_VM_TEXT;return &v->view;
        }
        if(!apply(v,n.first_effect,n.effect_count))return error(v,"invalid effect");
        if(n.choice_count){
            v->view.kind=NINE_VM_CHOICE;
            if(v->pack->filter_choices)for(uint32_t i=0;i<n.choice_count;i++){
                nine_edge_t edge;if(!nine_pack_edge(v->pack,true,n.first_choice+i,&edge))return error(v,"missing choice");
                if(nine_vm_matches(v,edge.first_condition,edge.condition_count))v->view.choices[v->view.choice_count++]=n.first_choice+i;
            }
            if(!v->view.choice_count)for(uint32_t i=0;i<n.choice_count;i++)v->view.choices[v->view.choice_count++]=n.first_choice+i;
            return &v->view;
        }
        bool found=false;for(uint32_t i=0;i<n.next_count;i++){
            nine_edge_t edge;if(!nine_pack_edge(v->pack,false,n.first_next+i,&edge))return error(v,"missing next");
            if(nine_vm_matches(v,edge.first_condition,edge.condition_count)){
                // Source next edges do not have effects; this rejects accidental
                // semantic expansion if a corrupted pack adds such an effect.
                if(edge.effect_count)return error(v,"unexpected next effect");
                v->state.node=edge.to;v->state.index=0;found=true;break;
            }
        }if(!found)return error(v,"no matching next");
    }return error(v,"graph hop limit");
}
const nine_view_t *nine_vm_advance(nine_vm_t*v){
    const nine_view_t*p=nine_vm_locate(v);if(!p)return NULL;
    if(p->kind!=NINE_VM_TEXT&&p->kind!=NINE_VM_OP)return p;
    remember(v);v->state.index++;return nine_vm_locate(v);
}
const nine_view_t *nine_vm_choose(nine_vm_t*v,uint32_t index){
    const nine_view_t*p=nine_vm_locate(v);if(!p)return NULL;
    if(p->kind!=NINE_VM_CHOICE||index>=p->choice_count)return error(v,"invalid choice");
    nine_edge_t e;if(!nine_pack_edge(v->pack,true,p->choices[index],&e))return error(v,"missing selected edge");
    remember(v);if(!apply(v,e.first_effect,e.effect_count))return error(v,"invalid choice effect");
    v->state.node=e.to;v->state.index=0;return nine_vm_locate(v);
}
const nine_view_t *nine_vm_previous(nine_vm_t*v){
    if(!v||!v->pack)return NULL;
    if(v->history_count)v->state=v->history[--v->history_count];
    return nine_vm_locate(v);
}
const nine_view_t *nine_vm_skip_chapter(nine_vm_t*v){
    const nine_view_t*p=nine_vm_locate(v);if(!p||p->kind!=NINE_VM_TEXT)return p;
    nine_node_t initial;if(!nine_pack_node(v->pack,v->state.node,&initial))return error(v,"missing chapter");
    remember(v);
    for(unsigned step=0;step<500;step++){
        nine_node_t n;if(!nine_pack_node(v->pack,v->state.node,&n))return error(v,"missing skip node");
        uint32_t to=n.frame_count;for(uint32_t i=v->state.index+1;i<n.frame_count;i++){
            nine_frame_t f;if(!nine_pack_frame(v->pack,n.first_frame+i,&f))return error(v,"invalid skip frame");
            if(f.kind==NINE_FRAME_OP){to=i;break;}
        }v->state.index=to;p=nine_vm_locate(v);
        if(p->kind!=NINE_VM_TEXT)return p;
        if(!nine_pack_node(v->pack,v->state.node,&n))return error(v,"missing chapter node");
        if(n.chapter!=initial.chapter)return p;
    }return error(v,"chapter hop limit");
}
bool nine_vm_current_frame(const nine_vm_t*v,nine_frame_t*out){
    return v&&(v->view.kind==NINE_VM_TEXT||v->view.kind==NINE_VM_OP)&&nine_pack_frame(v->pack,v->view.frame,out);
}

static uint32_t read32(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void write32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
#define SAVE_MAGIC 0x3153564eu
#define PROGRESS_MAGIC 0x3150564eu
static bool record(const nine_pack_t*p,const uint8_t*d,size_t size,uint32_t magic,size_t expect){
    return p&&d&&size==expect&&size>=24&&read32(d)==magic&&read32(d+4)==1&&read32(d+8)==p->episode&&read32(d+12)==p->identity&&read32(d+16)==size&&read32(d+size-4)==nine_crc32_bytes(d,size-4);
}
size_t nine_vm_save_encode(const nine_vm_t*v,uint16_t page,uint8_t*out,size_t cap){
    if(!v||!v->pack||!out)return 0;
    uint32_t keys=v->pack->key_count;size_t size=40+keys*8+32+4;if(cap<size)return 0;
    memset(out,0,size);write32(out,SAVE_MAGIC);write32(out+4,1);write32(out+8,v->pack->episode);write32(out+12,v->pack->identity);write32(out+16,(uint32_t)size);
    write32(out+20,v->state.node);write32(out+24,v->state.index);write32(out+28,page);write32(out+32,v->state.preview);write32(out+36,keys);
    for(uint32_t i=0;i<keys;i++){write32(out+40+i*4,(uint32_t)v->state.flag[i]);write32(out+40+keys*4+i*4,(uint32_t)v->state.progress[i]);}
    memcpy(out+40+keys*8,v->state.flag_set,16);memcpy(out+56+keys*8,v->state.progress_set,16);write32(out+size-4,nine_crc32_bytes(out,size-4));return size;
}
bool nine_vm_save_peek(const nine_pack_t*p,const uint8_t*d,size_t size,uint16_t*node,uint32_t*index,uint16_t*page,bool*preview){
    if(!p||!record(p,d,size,SAVE_MAGIC,40+p->key_count*8+32+4)||read32(d+36)!=p->key_count||read32(d+28)>UINT16_MAX||read32(d+32)>1)return false;
    uint32_t n=read32(d+20),i=read32(d+24);nine_node_t nr;
    if(n!=NINE_NODE_NONE&&(!nine_pack_node(p,(uint16_t)n,&nr)||n>=p->node_count||i>nr.frame_count))return false;
    if(n==NINE_NODE_NONE&&i)return false;
    if(node)*node=(uint16_t)n;
    if(index)*index=i;
    if(page)*page=(uint16_t)read32(d+28);
    if(preview)*preview=read32(d+32)!=0;
    return true;
}
bool nine_vm_save_decode(nine_vm_t*v,const uint8_t*d,size_t size,uint16_t*page){
    uint16_t node,pg;uint32_t index;bool preview;
    if(!v||!nine_vm_save_peek(v->pack,d,size,&node,&index,&pg,&preview))return false;
    nine_state_t next;memset(&next,0,sizeof(next));next.node=node;next.index=index;next.preview=preview;
    uint32_t keys=v->pack->key_count;
    for(uint32_t i=0;i<keys;i++){next.flag[i]=(int32_t)read32(d+40+i*4);next.progress[i]=(int32_t)read32(d+40+keys*4+i*4);}
    memcpy(next.flag_set,d+40+keys*8,16);memcpy(next.progress_set,d+56+keys*8,16);
    // A chapter preview restores its exact isolated snapshot. Normal play loads
    // the snapshot's local flags while keeping the newest permanent record.
    if(!preview)for(uint32_t i=0;i<keys;i++)if(has(v->state.progress_set,(uint16_t)i)){
        next.progress[i]=v->state.progress[i];mark(next.progress_set,(uint16_t)i);
    }
    v->state=next;v->history_count=0;if(page)*page=pg;
    // No locate here: index==count is a valid pending boundary, whose effects
    // execute only when the caller actually resumes the VM.
    memset(&v->view,0,sizeof(v->view));return true;
}
size_t nine_vm_progress_encode(const nine_vm_t*v,uint8_t*out,size_t cap){
    if(!v||!v->pack||!out)return 0;
    uint32_t keys=v->pack->key_count;size_t size=24+keys*4+16+4;if(cap<size)return 0;
    memset(out,0,size);write32(out,PROGRESS_MAGIC);write32(out+4,1);write32(out+8,v->pack->episode);write32(out+12,v->pack->identity);write32(out+16,(uint32_t)size);write32(out+20,keys);
    for(uint32_t i=0;i<keys;i++)write32(out+24+i*4,(uint32_t)v->state.progress[i]);
    memcpy(out+24+keys*4,v->state.progress_set,16);write32(out+size-4,nine_crc32_bytes(out,size-4));return size;
}
bool nine_vm_progress_decode(nine_vm_t*v,const uint8_t*d,size_t size){
    if(!v||!v->pack||!record(v->pack,d,size,PROGRESS_MAGIC,24+v->pack->key_count*4+16+4)||read32(d+20)!=v->pack->key_count)return false;
    for(uint32_t i=0;i<v->pack->key_count;i++)v->state.progress[i]=(int32_t)read32(d+24+i*4);
    memcpy(v->state.progress_set,d+24+v->pack->key_count*4,16);return true;
}
