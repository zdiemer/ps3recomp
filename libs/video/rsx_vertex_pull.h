#ifndef RSX_VERTEX_PULL_H
#define RSX_VERTEX_PULL_H
/* Per-draw immutable raw geometry snapshot. The GPU decodes attributes; CPU
 * reference gathering/topology remain unchanged in this first implementation.
 * Layout: 16 x 48-byte attribute records, uint2 references, merged source spans.
 * A caller retains the snapshot upload until the consuming GPU fence completes. */
#include "rsx_vertex_compact.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { RSX_PULL_RECORD=48, RSX_PULL_REFS=16*48 };
typedef struct rsx_vertex_pull_payload {
    u8* data;
    u32 capacity, bytes;
} rsx_vertex_pull_payload;

/* plan must have been prepared for these exact references. For frequency <=1,
 * prepare validates each mapped span using their masked min/max indices. */
static int rsx_vertex_pull_pack(rsx_vertex_pull_payload* out,
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs,
    u32 count, u32 limit)
{
    struct range { uintptr_t first, end; u32 offset; } ranges[16];
    u32 n=0;
    out->bytes=0;
    if (!count || !refs || plan->inline_data || plan->layout.count>16) return 0;
    u64 bytes=(RSX_PULL_REFS+(u64)count*8+15)&~15ull;
    for (u32 slot=0;slot<plan->layout.count;++slot) {
        const u32 attr=plan->layout.attrs[slot];
        if (attr>=16) return 0;
        const rsx_vertex_fetch_attr* f=&plan->attr[attr];
        if (!f->desc.type || !f->desc.size) continue;
        if (!f->stable_base || !f->elem_size || f->desc.type>7 ||
            f->desc.size>4 || f->desc.frequency>1 || f->stable_last<f->stable_first) return 0;
        const u64 size=(u64)(f->stable_last-f->stable_first)*f->stride+f->elem_size;
        const uintptr_t first=(uintptr_t)f->stable_base;
        if (!size || size>limit || first>UINTPTR_MAX-size) return 0;
        u32 at=n++;
        while (at && ranges[at-1].first>first) { ranges[at]=ranges[at-1]; --at; }
        ranges[at].first=first; ranges[at].end=first+(uintptr_t)size;
    }
    u32 merged=0;
    for (u32 i=0;i<n;++i) {
        if (merged && ranges[i].first<=ranges[merged-1].end) {
            if (ranges[i].end>ranges[merged-1].end) ranges[merged-1].end=ranges[i].end;
        } else ranges[merged++]=ranges[i];
    }
    for (u32 i=0;i<merged;++i) {
        if (bytes>limit) return 0;
        ranges[i].offset=(u32)bytes;
        /* Four safe padding bytes permit an unaligned final Load on the GPU. */
        bytes=(bytes+ranges[i].end-ranges[i].first+4+15)&~15ull;
    }
    if (bytes>limit) return 0;
    if (out->capacity<bytes) {
        u8* data=(u8*)realloc(out->data,(size_t)bytes);
        if (!data) return 0;
        out->data=data; out->capacity=(u32)bytes;
    }
    memset(out->data,0,RSX_PULL_REFS);
    memcpy(out->data+RSX_PULL_REFS,refs,(size_t)count*8);
    for (u32 i=0;i<merged;++i) {
        const size_t size=ranges[i].end-ranges[i].first;
        memcpy(out->data+ranges[i].offset,(const void*)ranges[i].first,size);
        memset(out->data+ranges[i].offset+size,0,4);
    }
    for (u32 slot=0;slot<plan->layout.count;++slot) {
        const u32 attr=plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* f=&plan->attr[attr];
        u32 record[12]={0};
        memcpy(record+8,f->default_value,16);
        if (f->desc.type && f->desc.size) {
            const uintptr_t first=(uintptr_t)f->stable_base;
            for (u32 i=0;i<merged;++i) if (first>=ranges[i].first && first<ranges[i].end) {
                record[0]=ranges[i].offset+(u32)(first-ranges[i].first); break;
            }
            record[1]=f->stride; record[2]=f->stable_first;
            record[3]=f->desc.type; record[4]=f->desc.size;
        }
        memcpy(out->data+attr*RSX_PULL_RECORD,record,sizeof(record));
    }
    out->bytes=(u32)bytes;
    return 1;
}

/* Shared by the real vertex shader and GPU decoder readback tests. Explicit
 * integer reconstruction matches the CPU's endian and half-denormal policy.
 * Keep the component loop and scalar stores explicit: dynamic vector writes
 * expand compiler work without changing the component conversion. */
static const char rsx_vertex_pull_hlsl[] =
"ByteAddressBuffer rsx_pull_data:register(t20);\n"
"uint pull_le32(uint a){uint s=(a&3u)*8u;uint lo=rsx_pull_data.Load(a&~3u);"
"return s?((lo>>s)|(rsx_pull_data.Load((a&~3u)+4u)<<(32u-s))):lo;}\n"
"uint pull_be32(uint a){uint x=pull_le32(a);return (x>>24)|((x>>8)&65280u)|((x<<8)&16711680u)|(x<<24);}\n"
"uint pull_be16(uint a){uint x=pull_le32(a);return ((x&255u)<<8)|((x>>8)&255u);}\n"
"float4 rsx_pull(uint vertex,uint attr){uint at=attr*48u;uint4 m=rsx_pull_data.Load4(at);"
"if(!m.w)return asfloat(rsx_pull_data.Load4(at+32u));"
"uint2 r=rsx_pull_data.Load2(768u+vertex*8u);uint a=m.x+(((r.x+r.y)&1048575u)-m.z)*m.y;"
"uint n=rsx_pull_data.Load(at+16u);float4 v=float4(0,0,0,1);"
"if(m.w==6u){uint x=pull_be32(a);int sx=(int)(x<<21)>>21;int sy=(int)(x<<10)>>21;int sz=(int)x>>22;"
"return float4((float)sx/1023.0,(float)sy/1023.0,(float)sz/511.0,1);}\n"
"[loop]for(uint i=0;i<4u;++i){if(i<n){float component;"
"if(m.w==2u)component=asfloat(pull_be32(a+i*4u));"
"else if(m.w==3u){uint h=pull_be16(a+i*2u);uint e=(h>>10)&31u;uint f=h&1023u;"
"uint bits=((h&32768u)<<16)|(e==0u?0u:(e==31u?2139095040u|(f<<13):((e+112u)<<23)|(f<<13)));component=asfloat(bits);}"
"else if(m.w==4u)component=(float)(pull_le32(a+i)&255u)/255.0;"
"else if(m.w==7u)component=(float)(pull_le32(a+i)&255u);"
"else {int s=(int)(pull_be16(a+i*2u)<<16)>>16;component=m.w==1u?(float)s/32767.0:(float)s;}"
"if(i==0u)v.x=component;else if(i==1u)v.y=component;"
"else if(i==2u)v.z=component;else v.w=component;}}return v;}\n";

static int rsx_vertex_pull_wrap(char* shader,u32 capacity,u32 mask)
{
    const char* entry="VSOutput main(VSInput input) {";
    char* at=strstr(shader,entry);
    if (!at) return 0;
    char replacement[1536];
    int used=snprintf(replacement,sizeof replacement,
        "VSOutput main(uint rsx_vertex_id:SV_VertexID) { VSInput input;\n");
    for (u32 attr=0;attr<16;++attr) if (mask&(1u<<attr))
        used+=snprintf(replacement+used,sizeof replacement-used,
            "input.a%u=rsx_pull(rsx_vertex_id,%u);\n",attr,attr);
    if (!mask) used+=snprintf(replacement+used,sizeof replacement-used,
        "input.vertex_id=rsx_vertex_id;\n");
    const size_t prefix=(size_t)(at-shader), tail=strlen(at+strlen(entry));
    const size_t helper=sizeof(rsx_vertex_pull_hlsl)-1;
    const size_t total=helper+prefix+(size_t)used+tail+1;
    if (used<0 || used>=sizeof replacement || total>capacity) return 0;
    char* copy=(char*)malloc(total);
    if (!copy) return 0;
    memcpy(copy,rsx_vertex_pull_hlsl,helper);
    memcpy(copy+helper,shader,prefix);
    memcpy(copy+helper+prefix,replacement,(size_t)used);
    memcpy(copy+helper+prefix+used,at+strlen(entry),tail+1);
    memcpy(shader,copy,total); free(copy); return 1;
}
#endif
