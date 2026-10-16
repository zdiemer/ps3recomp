#include "rsx_pipeline_recipes.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <d3dcompiler.h>
#include <limits.h>
#include <wchar.h>

#define RPR_MAX_ROOT_BYTES (1024u * 1024u)
#define RPR_MAX_RECIPE_BYTES (20u * 1024u * 1024u)
#define RPR_MAX_STAGE_BYTES (4u * 1024u * 1024u)
#define RPR_MAX_INPUTS 32u
#define RPR_MAX_SEMANTIC 64u
#define RPR_HEADER_BYTES 40u
#define RPR_RECORD_HEADER_BYTES 16u

typedef struct rpr_item {
    uint8_t* bytes;
    uint32_t size;
    uint64_t hash;
    ID3D12PipelineState* pso;
} rpr_item;

struct rsx_pipeline_recipes {
    ID3D12Device* device;
    ID3D12RootSignature* root;
    uint8_t* root_bytes;
    uint32_t root_size;
    wchar_t* path;
    rpr_item* items;
    uint32_t count;
    uint64_t owned_bytes;
    rsx_pipeline_recipes_stats stats;
    int loaded;
    int saved;
};

typedef struct rpr_buf {
    uint8_t* p;
    size_t n;
    size_t cap;
    size_t limit;
    int failed;
} rpr_buf;

typedef struct rpr_cursor {
    const uint8_t* p;
    size_t n;
    size_t at;
    int failed;
} rpr_cursor;

typedef struct rpr_decoded {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    D3D12_INPUT_ELEMENT_DESC inputs[RPR_MAX_INPUTS];
} rpr_decoded;

static uint32_t rpr_crc32(const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    for (i = 0; i < size; ++i) {
        unsigned bit;
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return ~crc;
}

static uint64_t rpr_hash64(const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1u;
}

static int rpr_reserve(rpr_buf* b, size_t add)
{
    size_t need, cap, limit = b->limit ? b->limit : RPR_MAX_RECIPE_BYTES;
    uint8_t* p;
    if (b->failed || add > limit || b->n > limit - add) {
        b->failed = 1; return 0;
    }
    need = b->n + add;
    if (need <= b->cap) return 1;
    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > limit / 2) { cap = limit; break; }
        cap *= 2;
    }
    if (cap < need) { b->failed = 1; return 0; }
    p = (uint8_t*)realloc(b->p, cap);
    if (!p) { b->failed = 1; return 0; }
    b->p = p; b->cap = cap; return 1;
}

static void rpr_put(rpr_buf* b, const void* p, size_t n)
{
    if (!rpr_reserve(b, n)) return;
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void rpr_put_u32(rpr_buf* b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    rpr_put(b, x, sizeof x);
}

static uint32_t rpr_get_u32(rpr_cursor* c)
{
    uint32_t v;
    if (c->failed || c->at > c->n || c->n - c->at < 4) { c->failed = 1; return 0; }
    v = (uint32_t)c->p[c->at] | ((uint32_t)c->p[c->at + 1] << 8) |
        ((uint32_t)c->p[c->at + 2] << 16) | ((uint32_t)c->p[c->at + 3] << 24);
    c->at += 4; return v;
}

static const uint8_t* rpr_get(rpr_cursor* c, size_t n)
{
    const uint8_t* p;
    if (c->failed || c->at > c->n || n > c->n - c->at) { c->failed = 1; return NULL; }
    p = c->p + c->at; c->at += n; return p;
}

static D3D12_RENDER_TARGET_BLEND_DESC rpr_norm_target_blend(
    const D3D12_RENDER_TARGET_BLEND_DESC* s)
{
    D3D12_RENDER_TARGET_BLEND_DESC d; memset(&d, 0, sizeof d);
    d.BlendEnable=s->BlendEnable; d.LogicOpEnable=s->LogicOpEnable;
    d.SrcBlend=s->SrcBlend; d.DestBlend=s->DestBlend; d.BlendOp=s->BlendOp;
    d.SrcBlendAlpha=s->SrcBlendAlpha; d.DestBlendAlpha=s->DestBlendAlpha;
    d.BlendOpAlpha=s->BlendOpAlpha; d.LogicOp=s->LogicOp;
    d.RenderTargetWriteMask=s->RenderTargetWriteMask; return d;
}

static D3D12_BLEND_DESC rpr_norm_blend(const D3D12_BLEND_DESC* s)
{
    D3D12_BLEND_DESC d; unsigned i; memset(&d, 0, sizeof d);
    d.AlphaToCoverageEnable=s->AlphaToCoverageEnable;
    d.IndependentBlendEnable=s->IndependentBlendEnable;
    for (i=0;i<8;i++) d.RenderTarget[i]=rpr_norm_target_blend(&s->RenderTarget[i]);
    return d;
}

static D3D12_RASTERIZER_DESC rpr_norm_raster(const D3D12_RASTERIZER_DESC* s)
{
    D3D12_RASTERIZER_DESC d; memset(&d, 0, sizeof d);
    d.FillMode=s->FillMode; d.CullMode=s->CullMode;
    d.FrontCounterClockwise=s->FrontCounterClockwise; d.DepthBias=s->DepthBias;
    d.DepthBiasClamp=s->DepthBiasClamp; d.SlopeScaledDepthBias=s->SlopeScaledDepthBias;
    d.DepthClipEnable=s->DepthClipEnable; d.MultisampleEnable=s->MultisampleEnable;
    d.AntialiasedLineEnable=s->AntialiasedLineEnable; d.ForcedSampleCount=s->ForcedSampleCount;
    d.ConservativeRaster=s->ConservativeRaster; return d;
}

static D3D12_DEPTH_STENCILOP_DESC rpr_norm_stencilop(const D3D12_DEPTH_STENCILOP_DESC* s)
{
    D3D12_DEPTH_STENCILOP_DESC d; memset(&d, 0, sizeof d);
    d.StencilFailOp=s->StencilFailOp; d.StencilDepthFailOp=s->StencilDepthFailOp;
    d.StencilPassOp=s->StencilPassOp; d.StencilFunc=s->StencilFunc; return d;
}

static D3D12_DEPTH_STENCIL_DESC rpr_norm_depth(const D3D12_DEPTH_STENCIL_DESC* s)
{
    D3D12_DEPTH_STENCIL_DESC d; memset(&d, 0, sizeof d);
    d.DepthEnable=s->DepthEnable; d.DepthWriteMask=s->DepthWriteMask; d.DepthFunc=s->DepthFunc;
    d.StencilEnable=s->StencilEnable; d.StencilReadMask=s->StencilReadMask;
    d.StencilWriteMask=s->StencilWriteMask;
    d.FrontFace=rpr_norm_stencilop(&s->FrontFace); d.BackFace=rpr_norm_stencilop(&s->BackFace);
    return d;
}

static int rpr_build_recipe(const rsx_pipeline_recipes* cache,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, rpr_buf* b)
{
    const D3D12_SHADER_BYTECODE* sh[5];
    uint32_t i, n;
    D3D12_BLEND_DESC blend;
    D3D12_RASTERIZER_DESC raster;
    D3D12_DEPTH_STENCIL_DESC depth;
    static const char magic[4] = {'R','P','S','1'};
    if (!cache || !d || d->pRootSignature != cache->root ||
        d->DS.BytecodeLength || d->DS.pShaderBytecode ||
        d->HS.BytecodeLength || d->HS.pShaderBytecode ||
        d->GS.BytecodeLength || d->GS.pShaderBytecode ||
        d->StreamOutput.pSODeclaration || d->StreamOutput.NumEntries ||
        d->StreamOutput.pBufferStrides || d->StreamOutput.NumStrides ||
        d->StreamOutput.RasterizedStream || d->CachedPSO.pCachedBlob || d->CachedPSO.CachedBlobSizeInBytes)
        return 0;
    if (d->NumRenderTargets > 8 || !d->SampleDesc.Count) return 0;
    if (d->InputLayout.NumElements > RPR_MAX_INPUTS ||
        (!!d->InputLayout.NumElements != !!d->InputLayout.pInputElementDescs)) return 0;
    sh[0]=&d->VS; sh[1]=&d->PS; sh[2]=&d->DS; sh[3]=&d->HS; sh[4]=&d->GS;
    for (i=0;i<5;i++)
        if ((sh[i]->BytecodeLength && !sh[i]->pShaderBytecode) ||
            (!sh[i]->BytecodeLength && sh[i]->pShaderBytecode) ||
            sh[i]->BytecodeLength > RPR_MAX_STAGE_BYTES) return 0;
    if (sh[2]->BytecodeLength || sh[3]->BytecodeLength || sh[4]->BytecodeLength) return 0;
    rpr_put(b, magic, sizeof magic); rpr_put_u32(b, 1);
    rpr_put_u32(b, (uint32_t)sizeof(D3D12_BLEND_DESC));
    rpr_put_u32(b, (uint32_t)sizeof(D3D12_RASTERIZER_DESC));
    rpr_put_u32(b, (uint32_t)sizeof(D3D12_DEPTH_STENCIL_DESC));
    for (i=0;i<5;i++) rpr_put_u32(b, (uint32_t)sh[i]->BytecodeLength);
    rpr_put_u32(b, d->InputLayout.NumElements);
    for (i=0;i<5;i++) rpr_put(b, sh[i]->pShaderBytecode, sh[i]->BytecodeLength);
    for (i=0;i<d->InputLayout.NumElements;i++) {
        const D3D12_INPUT_ELEMENT_DESC* e=&d->InputLayout.pInputElementDescs[i];
        size_t len;
        if (!e->SemanticName) return 0;
        len=strnlen(e->SemanticName, RPR_MAX_SEMANTIC+1u);
        if (!len || len>RPR_MAX_SEMANTIC) return 0;
        rpr_put_u32(b, (uint32_t)len); rpr_put(b, e->SemanticName, len);
        { const uint8_t zero = 0; rpr_put(b, &zero, 1); }
        rpr_put_u32(b, e->SemanticIndex); rpr_put_u32(b, (uint32_t)e->Format);
        rpr_put_u32(b, e->InputSlot); rpr_put_u32(b, e->AlignedByteOffset);
        rpr_put_u32(b, (uint32_t)e->InputSlotClass); rpr_put_u32(b, e->InstanceDataStepRate);
    }
    blend=rpr_norm_blend(&d->BlendState); raster=rpr_norm_raster(&d->RasterizerState);
    depth=rpr_norm_depth(&d->DepthStencilState);
    rpr_put(b, &blend, sizeof blend); rpr_put_u32(b, d->SampleMask);
    rpr_put(b, &raster, sizeof raster); rpr_put(b, &depth, sizeof depth);
    rpr_put_u32(b, (uint32_t)d->IBStripCutValue);
    rpr_put_u32(b, (uint32_t)d->PrimitiveTopologyType); rpr_put_u32(b, d->NumRenderTargets);
    for (n=0;n<8;n++) rpr_put_u32(b, (uint32_t)d->RTVFormats[n]);
    rpr_put_u32(b, (uint32_t)d->DSVFormat); rpr_put_u32(b, d->SampleDesc.Count);
    rpr_put_u32(b, d->SampleDesc.Quality); rpr_put_u32(b, d->NodeMask);
    rpr_put_u32(b, (uint32_t)d->Flags);
    return !b->failed && b->n <= RPR_MAX_RECIPE_BYTES;
}

static int rpr_decode_recipe(const rsx_pipeline_recipes* cache,
    const uint8_t* bytes, size_t size, rpr_decoded* out)
{
    rpr_cursor c; const uint8_t* p; uint32_t version, sizes[3], lens[5], n, i;
    D3D12_SHADER_BYTECODE* sh[5];
    memset(out,0,sizeof *out); c.p=bytes; c.n=size; c.at=0; c.failed=0;
    p=rpr_get(&c,4); if (!p || memcmp(p,"RPS1",4)) return 0;
    version=rpr_get_u32(&c); sizes[0]=rpr_get_u32(&c); sizes[1]=rpr_get_u32(&c); sizes[2]=rpr_get_u32(&c);
    if (version!=1 || sizes[0]!=sizeof(D3D12_BLEND_DESC) || sizes[1]!=sizeof(D3D12_RASTERIZER_DESC) ||
        sizes[2]!=sizeof(D3D12_DEPTH_STENCIL_DESC)) return 0;
    for (i=0;i<5;i++) { lens[i]=rpr_get_u32(&c); if (lens[i]>RPR_MAX_STAGE_BYTES) return 0; }
    if (lens[2] || lens[3] || lens[4]) return 0;
    n=rpr_get_u32(&c); if (n>RPR_MAX_INPUTS) return 0;
    sh[0]=&out->desc.VS; sh[1]=&out->desc.PS; sh[2]=&out->desc.DS; sh[3]=&out->desc.HS; sh[4]=&out->desc.GS;
    for (i=0;i<5;i++) { sh[i]->BytecodeLength=lens[i]; p=rpr_get(&c,lens[i]); if (c.failed) return 0; sh[i]->pShaderBytecode=lens[i]?p:NULL; }
    for (i=0;i<n;i++) {
        D3D12_INPUT_ELEMENT_DESC* e=&out->inputs[i]; uint32_t len;
        len=rpr_get_u32(&c); if (!len || len>RPR_MAX_SEMANTIC) return 0;
        p=rpr_get(&c,len+1u); if (!p || p[len]) return 0;
        for (uint32_t j=0;j<len;j++) if (!p[j]) return 0;
        e->SemanticName=(LPCSTR)p; e->SemanticIndex=rpr_get_u32(&c);
        e->Format=(DXGI_FORMAT)rpr_get_u32(&c); e->InputSlot=rpr_get_u32(&c);
        e->AlignedByteOffset=rpr_get_u32(&c); e->InputSlotClass=(D3D12_INPUT_CLASSIFICATION)rpr_get_u32(&c);
        e->InstanceDataStepRate=rpr_get_u32(&c);
    }
    p=rpr_get(&c,sizeof(D3D12_BLEND_DESC)); if (!p) return 0; memcpy(&out->desc.BlendState,p,sizeof(D3D12_BLEND_DESC));
    out->desc.SampleMask=rpr_get_u32(&c);
    p=rpr_get(&c,sizeof(D3D12_RASTERIZER_DESC)); if (!p) return 0; memcpy(&out->desc.RasterizerState,p,sizeof(D3D12_RASTERIZER_DESC));
    p=rpr_get(&c,sizeof(D3D12_DEPTH_STENCIL_DESC)); if (!p) return 0; memcpy(&out->desc.DepthStencilState,p,sizeof(D3D12_DEPTH_STENCIL_DESC));
    out->desc.IBStripCutValue=(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)rpr_get_u32(&c);
    out->desc.PrimitiveTopologyType=(D3D12_PRIMITIVE_TOPOLOGY_TYPE)rpr_get_u32(&c);
    out->desc.NumRenderTargets=rpr_get_u32(&c); if (out->desc.NumRenderTargets>8) return 0;
    for (i=0;i<8;i++) out->desc.RTVFormats[i]=(DXGI_FORMAT)rpr_get_u32(&c);
    out->desc.DSVFormat=(DXGI_FORMAT)rpr_get_u32(&c);
    out->desc.SampleDesc.Count=rpr_get_u32(&c); out->desc.SampleDesc.Quality=rpr_get_u32(&c);
    out->desc.NodeMask=rpr_get_u32(&c); out->desc.Flags=(D3D12_PIPELINE_STATE_FLAGS)rpr_get_u32(&c);
    if (c.failed || c.at!=c.n || out->desc.NumRenderTargets>8 || !out->desc.SampleDesc.Count) return 0;
    out->desc.pRootSignature=cache->root;
    out->desc.InputLayout.pInputElementDescs=n ? out->inputs : NULL;
    out->desc.InputLayout.NumElements=n;
    {
        rpr_buf canonical={0};
        int equal;
        if (!rpr_build_recipe(cache,&out->desc,&canonical)) { free(canonical.p); return 0; }
        equal=canonical.n==size && !memcmp(canonical.p,bytes,size);
        free(canonical.p);
        if (!equal) return 0;
    }
    return 1;
}

static void rpr_item_release(rpr_item* it)
{
    if (it->pso) it->pso->lpVtbl->Release(it->pso);
    free(it->bytes); memset(it,0,sizeof *it);
}

static int rpr_add_owned(rsx_pipeline_recipes* cache,uint8_t* bytes,uint32_t size,ID3D12PipelineState* pso)
{
    uint64_t hash=rpr_hash64(bytes,size); uint32_t i;
    for (i=0;i<cache->count;i++)
        if (cache->items[i].hash==hash && cache->items[i].size==size && !memcmp(cache->items[i].bytes,bytes,size)) {
            if (!cache->items[i].pso && pso) { pso->lpVtbl->AddRef(pso); cache->items[i].pso=pso; }
            free(bytes); return 1;
        }
    if (cache->count>=RSX_PIPELINE_RECIPES_MAX_ENTRIES ||
        size > RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES - RPR_RECORD_HEADER_BYTES ||
        cache->owned_bytes > RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES - RPR_RECORD_HEADER_BYTES - size) {
        cache->stats.capacity_rejections++;
        free(bytes); return 0;
    }
    if (pso) pso->lpVtbl->AddRef(pso);
    cache->items[cache->count].bytes=bytes; cache->items[cache->count].size=size;
    cache->items[cache->count].hash=hash; cache->items[cache->count].pso=pso;
    cache->count++; cache->owned_bytes+=RPR_RECORD_HEADER_BYTES+size;
    return 1;
}

rsx_pipeline_recipes* rsx_pipeline_recipes_create(ID3D12Device* device,
    ID3D12RootSignature* root,const void* root_bytes,size_t root_size,const wchar_t* path)
{
    rsx_pipeline_recipes* c; size_t chars;
    if (!device || !root || !root_bytes || !root_size || root_size>RPR_MAX_ROOT_BYTES || !path) return NULL;
    chars=wcslen(path); if (!chars || chars>32700) return NULL;
    c=(rsx_pipeline_recipes*)calloc(1,sizeof *c); if (!c) return NULL;
    c->device=device; c->root=root; c->root_size=(uint32_t)root_size;
    device->lpVtbl->AddRef(device);
    root->lpVtbl->AddRef(root);
    c->root_bytes=(uint8_t*)malloc(root_size); c->path=(wchar_t*)malloc((chars+1)*sizeof(wchar_t));
    c->items=(rpr_item*)calloc(RSX_PIPELINE_RECIPES_MAX_ENTRIES,sizeof(rpr_item));
    if (!c->root_bytes || !c->path || !c->items) { rsx_pipeline_recipes_destroy(c); return NULL; }
    memcpy(c->root_bytes,root_bytes,root_size); memcpy(c->path,path,(chars+1)*sizeof(wchar_t));
    c->owned_bytes=RPR_HEADER_BYTES+root_size; return c;
}

int rsx_pipeline_recipes_load(rsx_pipeline_recipes* c,rsx_pipeline_recipes_stats* out_stats)
{
    HANDLE f=INVALID_HANDLE_VALUE; LARGE_INTEGER sz; uint8_t* file=NULL; uint32_t i,count,root_size;
    size_t at; int valid=0, validation_started=0; rpr_item* pending=NULL; uint64_t prewarm_begin=0;
    if (!c || c->loaded) return 0;
    c->loaded=1;
    f=CreateFileW(c->path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if (f==INVALID_HANDLE_VALUE) goto done;
    validation_started=1;
    if (!GetFileSizeEx(f,&sz) || sz.QuadPart<(LONGLONG)RPR_HEADER_BYTES ||
        (uint64_t)sz.QuadPart>RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES) goto done;
    file=(uint8_t*)malloc((size_t)sz.QuadPart); if (!file) goto done;
    { size_t off=0; while (off<(size_t)sz.QuadPart) { DWORD got=0, ask=(DWORD)(((size_t)sz.QuadPart-off)>0x40000000u?0x40000000u:(size_t)sz.QuadPart-off); if (!ReadFile(f,file+off,ask,&got,NULL)||!got) goto done; off+=got; } }
    if (memcmp(file,"HPSORC01",8) || rpr_crc32(file+RPR_HEADER_BYTES,(size_t)sz.QuadPart-RPR_HEADER_BYTES)!=
        ((uint32_t)file[32]|((uint32_t)file[33]<<8)|((uint32_t)file[34]<<16)|((uint32_t)file[35]<<24))) goto done;
    { rpr_cursor h={file,(size_t)sz.QuadPart,8,0}; uint32_t ver=rpr_get_u32(&h), hs=rpr_get_u32(&h), total=rpr_get_u32(&h); count=rpr_get_u32(&h); root_size=rpr_get_u32(&h); uint32_t root_crc=rpr_get_u32(&h); (void)rpr_get_u32(&h); (void)rpr_get_u32(&h);
      if (ver!=1 || hs!=RPR_HEADER_BYTES || total!=(uint32_t)sz.QuadPart || count>RSX_PIPELINE_RECIPES_MAX_ENTRIES || root_size!=c->root_size || root_crc!=rpr_crc32(c->root_bytes,c->root_size)) goto done; }
    at=RPR_HEADER_BYTES;
    if (at>(size_t)sz.QuadPart || c->root_size>(size_t)sz.QuadPart-at || memcmp(file+at,c->root_bytes,c->root_size)) goto done;
    at+=c->root_size;
    pending=(rpr_item*)calloc(count?count:1,sizeof(rpr_item)); if (!pending) goto done;
    for (i=0;i<count;i++) {
        uint32_t len,crc; uint64_t hash; rpr_decoded decoded;
        if (at>(size_t)sz.QuadPart || (size_t)sz.QuadPart-at<RPR_RECORD_HEADER_BYTES) goto done;
      { rpr_cursor h={file,(size_t)sz.QuadPart,at,0}; uint32_t hash_lo,hash_hi; len=rpr_get_u32(&h); crc=rpr_get_u32(&h); hash_lo=rpr_get_u32(&h); hash_hi=rpr_get_u32(&h); hash=(uint64_t)hash_lo|((uint64_t)hash_hi<<32); at=h.at; }
        if (!len || len>RPR_MAX_RECIPE_BYTES || len>(size_t)sz.QuadPart-at) goto done;
        if (crc!=rpr_crc32(file+at,len) || hash!=rpr_hash64(file+at,len) || !rpr_decode_recipe(c,file+at,len,&decoded)) goto done;
        pending[i].bytes=(uint8_t*)malloc(len); if (!pending[i].bytes) goto done;
        memcpy(pending[i].bytes,file+at,len); pending[i].size=len; pending[i].hash=hash; at+=len;
        { uint32_t j; for (j=0;j<i;j++) if (pending[j].hash==hash && pending[j].size==len && !memcmp(pending[j].bytes,pending[i].bytes,len)) goto done; }
    }
    if (at!=(size_t)sz.QuadPart) goto done;
    valid=1;
    /* Integrity, root identity, and supported-layout validation finish before
     * the driver sees a CreateGraphicsPipelineState call. The driver remains
     * responsible for enum and bytecode validity. */
    prewarm_begin=GetTickCount64();
    for (i=0;i<count;i++) {
        rpr_decoded d; ID3D12PipelineState* pso=NULL;
        if (!rpr_decode_recipe(c,pending[i].bytes,pending[i].size,&d)) { valid=0; goto done; }
        if (SUCCEEDED(c->device->lpVtbl->CreateGraphicsPipelineState(c->device,&d.desc,
                &IID_ID3D12PipelineState,(void**)&pso)) && pso) {
            pending[i].pso=pso; c->stats.prewarmed++;
        } else c->stats.prewarm_failed++;
    }
    for (i=0;i<count;i++) {
        if (c->count>=RSX_PIPELINE_RECIPES_MAX_ENTRIES ||
        pending[i].size > RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES - RPR_RECORD_HEADER_BYTES ||
        c->owned_bytes > RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES - RPR_RECORD_HEADER_BYTES - pending[i].size) { valid=0; goto done; }
        c->items[c->count++]=pending[i]; c->owned_bytes+=pending[i].size;
        c->owned_bytes+=RPR_RECORD_HEADER_BYTES;
        memset(&pending[i],0,sizeof pending[i]); c->stats.loaded++;
    }
done:
    if (f!=INVALID_HANDLE_VALUE) CloseHandle(f);
    if (pending) { for (i=0;i<count;i++) rpr_item_release(&pending[i]); free(pending); }
    free(file);
    if (!valid) { if(validation_started)c->stats.corrupt_archives++; for(i=0;i<c->count;i++) rpr_item_release(&c->items[i]); c->count=0; c->owned_bytes=RPR_HEADER_BYTES+c->root_size; c->stats.loaded=0; c->stats.prewarmed=0; }
    c->stats.owned_bytes=c->owned_bytes;
    c->stats.prewarm_ms=prewarm_begin ? (double)(GetTickCount64()-prewarm_begin) : 0.0;
    if (out_stats) *out_stats=c->stats;
    return valid;
}

ID3D12PipelineState* rsx_pipeline_recipes_find(rsx_pipeline_recipes* c,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d)
{
    rpr_buf b={0}; uint64_t hash; uint32_t i; ID3D12PipelineState* pso=NULL;
    if (!c || !d || !rpr_build_recipe(c,d,&b)) { if(c) c->stats.unsupported_recipes++; free(b.p); return NULL; }
    hash=rpr_hash64(b.p,b.n);
    for (i=0;i<c->count;i++) if (c->items[i].hash==hash && c->items[i].size==b.n &&
        !memcmp(c->items[i].bytes,b.p,b.n) && c->items[i].pso) {
        pso=c->items[i].pso; pso->lpVtbl->AddRef(pso); c->stats.find_hits++; break;
    }
    if (!pso) c->stats.find_misses++;
    free(b.p); return pso;
}

int rsx_pipeline_recipes_remember(rsx_pipeline_recipes* c,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,ID3D12PipelineState* pso)
{
    rpr_buf b={0}; int ok; uint32_t old_count;
    if (!c || !d || !pso) return 0;
    if (!rpr_build_recipe(c,d,&b) || !b.n || b.n>UINT32_MAX) { c->stats.unsupported_recipes++; free(b.p); return 0; }
    old_count=c->count;
    ok=rpr_add_owned(c,b.p,(uint32_t)b.n,pso);
    if (ok) {
        c->stats.remembered++; c->stats.owned_bytes=c->owned_bytes;
        if (c->count!=old_count) c->saved=0;
    }
    return ok;
}

int rsx_pipeline_recipes_save_atomic(rsx_pipeline_recipes* c)
{
    rpr_buf b={0}; uint32_t i; HANDLE f=INVALID_HANDLE_VALUE; wchar_t* tmp=NULL; size_t pathn;
    wchar_t suffix[80]; int ok=0, temp_owned=0, suffix_len; static volatile LONG serial=0;
    if (!c || !c->path || c->saved) return 0;
    b.limit=RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES;
    rpr_put(&b,"HPSORC01",8); rpr_put_u32(&b,1); rpr_put_u32(&b,RPR_HEADER_BYTES);
    rpr_put_u32(&b,0); rpr_put_u32(&b,c->count); rpr_put_u32(&b,c->root_size);
    rpr_put_u32(&b,rpr_crc32(c->root_bytes,c->root_size)); rpr_put_u32(&b,0); rpr_put_u32(&b,0);
    rpr_put(&b,c->root_bytes,c->root_size);
    for (i=0;i<c->count;i++) {
        rpr_put_u32(&b,c->items[i].size); rpr_put_u32(&b,rpr_crc32(c->items[i].bytes,c->items[i].size));
        rpr_put_u32(&b,(uint32_t)c->items[i].hash); rpr_put_u32(&b,(uint32_t)(c->items[i].hash>>32));
        rpr_put(&b,c->items[i].bytes,c->items[i].size);
    }
    if (b.failed || b.n>RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES || b.n>UINT32_MAX) goto done;
    b.p[16]=(uint8_t)b.n; b.p[17]=(uint8_t)(b.n>>8); b.p[18]=(uint8_t)(b.n>>16); b.p[19]=(uint8_t)(b.n>>24);
    { uint32_t crc=rpr_crc32(b.p+RPR_HEADER_BYTES,b.n-RPR_HEADER_BYTES); b.p[32]=(uint8_t)crc; b.p[33]=(uint8_t)(crc>>8); b.p[34]=(uint8_t)(crc>>16); b.p[35]=(uint8_t)(crc>>24); }
    pathn=wcslen(c->path); tmp=(wchar_t*)malloc((pathn+80)*sizeof(wchar_t)); if(!tmp) goto done;
    suffix_len=swprintf_s(suffix,80,L".%08lx.%08lx.tmp",(unsigned long)GetCurrentProcessId(),(unsigned long)InterlockedIncrement(&serial));
    if (suffix_len<0 || (size_t)suffix_len+1>80) goto done;
    memcpy(tmp,c->path,pathn*sizeof(wchar_t)); memcpy(tmp+pathn,suffix,((size_t)suffix_len+1)*sizeof(wchar_t));
    f=CreateFileW(tmp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL); if(f==INVALID_HANDLE_VALUE) goto done;
    temp_owned=1;
    { size_t off=0; while(off<b.n) { DWORD wrote=0,ask=(DWORD)((b.n-off)>0x40000000u?0x40000000u:b.n-off); if(!WriteFile(f,b.p+off,ask,&wrote,NULL)||!wrote) goto done; off+=wrote; } }
    if(!FlushFileBuffers(f)) goto done;
    if(!CloseHandle(f)) { f=INVALID_HANDLE_VALUE; goto done; } f=INVALID_HANDLE_VALUE;
    if(!MoveFileExW(tmp,c->path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) goto done;
    temp_owned=0; ok=1; c->saved=1;
done:
    if(f!=INVALID_HANDLE_VALUE) CloseHandle(f);
    if(!ok && temp_owned && tmp) DeleteFileW(tmp);
    free(tmp); free(b.p); return ok;
}

void rsx_pipeline_recipes_get_stats(const rsx_pipeline_recipes* c,rsx_pipeline_recipes_stats* s)
{ if(c&&s){*s=c->stats;s->owned_bytes=c->owned_bytes;} }

void rsx_pipeline_recipes_destroy(rsx_pipeline_recipes* c)
{ uint32_t i; if(!c)return; for(i=0;i<c->count;i++)rpr_item_release(&c->items[i]); free(c->items);free(c->root_bytes);free(c->path); if(c->root)c->root->lpVtbl->Release(c->root); if(c->device)c->device->lpVtbl->Release(c->device); free(c); }

#else /* non-Windows builds retain a linkable, inert API */
struct rsx_pipeline_recipes { int unused; };
rsx_pipeline_recipes* rsx_pipeline_recipes_create(ID3D12Device* d,ID3D12RootSignature* r,const void* b,size_t n,const wchar_t* p){(void)d;(void)r;(void)b;(void)n;(void)p;return NULL;}
int rsx_pipeline_recipes_load(rsx_pipeline_recipes* c,rsx_pipeline_recipes_stats* s){(void)c;if(s)memset(s,0,sizeof *s);return 0;}
ID3D12PipelineState* rsx_pipeline_recipes_find(rsx_pipeline_recipes* c,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d){(void)c;(void)d;return NULL;}
int rsx_pipeline_recipes_remember(rsx_pipeline_recipes* c,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,ID3D12PipelineState* p){(void)c;(void)d;(void)p;return 0;}
int rsx_pipeline_recipes_save_atomic(rsx_pipeline_recipes* c){(void)c;return 0;}
void rsx_pipeline_recipes_get_stats(const rsx_pipeline_recipes* c,rsx_pipeline_recipes_stats* s){(void)c;if(s)memset(s,0,sizeof *s);}
void rsx_pipeline_recipes_destroy(rsx_pipeline_recipes* c){(void)c;}
#endif
