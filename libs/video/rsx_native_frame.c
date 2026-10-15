#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#include "rsx_native_frame.h"
#include "rsx_color_clear.h"
#include <stdlib.h>
#include <string.h>

#define FRAME_SAMPLER_HEAP_DESCRIPTORS 2048u
#define FRAME_MAX_SAMPLER_HEAPS (RSX_NATIVE_FRAME_MAX_EVENTS * 16u / FRAME_SAMPLER_HEAP_DESCRIPTORS)
#define FRAME_MAX_BINDING_DESCRIPTORS 1000000u
#define FRAME_UPLOAD_ALIGNMENT 512u

typedef struct frame_resource {
    ID3D12Resource* original;
    ID3D12Resource* clone;
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_STATES capture_state, replay_state;
    uint64_t bytes;
    uint32_t mutable_resource;
    uint32_t initialized;
} frame_resource;

typedef struct frame_draw {
    rsx_native_draw_packet packet;
    uint32_t descriptor_base;
    rsx_native_frame_binding srv[16], vtex[16];
    D3D12_CPU_DESCRIPTOR_HANDLE sampler[16];
    uint32_t sampler_base;
    uint32_t color_indices[4], color_count, depth_index, has_depth;
    D3D12_RENDER_TARGET_VIEW_DESC color_views[4];
    D3D12_RESOURCE_STATES color_states[4], depth_state;
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view;
    size_t payload_offsets[4], payload_sizes[4];
} frame_draw;

typedef struct frame_copy {
    uint32_t src, dst;
    D3D12_RESOURCE_STATES src_state, dst_state;
    UINT src_sub, dst_sub, x, y, z;
    uint32_t full_resource;
    D3D12_BOX box;
    uint32_t has_box;
} frame_copy;

typedef struct frame_clear_color {
    uint32_t target;
    D3D12_RESOURCE_STATES state;
    D3D12_RENDER_TARGET_VIEW_DESC view;
    DXGI_FORMAT format;
    UINT width, height;
    D3D12_RECT rect;
    UINT channels;
    FLOAT color[4];
} frame_clear_color;

typedef struct frame_clear_depth {
    uint32_t target;
    D3D12_RESOURCE_STATES state;
    D3D12_DEPTH_STENCIL_VIEW_DESC view;
    D3D12_RECT rect;
    FLOAT depth;
    UINT8 stencil;
    D3D12_CLEAR_FLAGS flags;
} frame_clear_depth;

typedef struct frame_compute {
    ID3D12PipelineState* pso;
    ID3D12RootSignature* root;
    UINT groups[3];
    uint32_t table_count, constants_count;
    struct { uint32_t root, count, descriptor_base; rsx_native_frame_binding bindings[16]; } tables[4];
    rsx_native_frame_compute_constants constants[16];
} frame_compute;

typedef struct frame_upload {
    uint32_t dst;
    D3D12_RESOURCE_STATES state;
    UINT subresource;
    size_t offset, size;
    UINT rows;
    UINT64 row_bytes;
    ID3D12Resource* staging;
} frame_upload;

typedef struct frame_event {
    uint32_t kind;
    union {
        frame_draw draw;
        frame_copy copy;
        frame_clear_color clear_color;
        frame_clear_depth clear_depth;
        frame_compute compute;
        frame_upload upload;
    } u;
} frame_event;

struct rsx_native_frame {
    ID3D12Device* device;
    rsx_native_frame_state state;
    frame_resource resources[RSX_NATIVE_FRAME_MAX_RESOURCES];
    uint32_t resource_count;
    frame_event* events;
    uint32_t event_count, event_limit;
    uint32_t descriptor_cursor;
    uint8_t* upload_bytes;
    size_t upload_used, upload_capacity, upload_limit;
    uint64_t gpu_bytes;
    ID3D12DescriptorHeap* srv_heap;
    ID3D12DescriptorHeap* owned_binding_heap;
    ID3D12DescriptorHeap* sampler_heaps[FRAME_MAX_SAMPLER_HEAPS];
    uint32_t sampler_heap_count;
    UINT srv_stride, sampler_stride;
    uint32_t sampler_cursor;
    ID3D12Resource* replay_upload;
    ID3D12DescriptorHeap* rtv_heap;
    ID3D12DescriptorHeap* dsv_heap;
    UINT rtv_stride, dsv_stride;
    rsx_color_clear clear_helper;
    uint32_t compare_index;
    D3D12_RENDER_TARGET_VIEW_DESC compare_view;
    D3D12_RESOURCE_STATES compare_original_state;
    uint64_t compare_readback_size;
    ID3D12Resource* compare_readback[2];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT compare_footprint;
    UINT64 compare_row_bytes;
    UINT compare_rows;
    uint64_t compare_bytes, mismatch_bytes;
    uint32_t compare_done;
    uint32_t coverage_verified;
    uint32_t unsupported_count;
    const char* failure;
    rsx_native_frame_stats stats;
};

static int frame_fail(rsx_native_frame* f, const char* reason)
{
    if (f) {
        f->unsupported_count++;
        if (f->state != RSX_NATIVE_FRAME_FAILED) {
            f->state = RSX_NATIVE_FRAME_FAILED;
            f->failure = reason ? reason : "unsupported frame event";
        }
    }
    return 0;
}

static int frame_ready(rsx_native_frame* f, ID3D12GraphicsCommandList* list)
{
    return f && f->state == RSX_NATIVE_FRAME_OPEN && list &&
        f->event_count < f->event_limit;
}

static int frame_push(rsx_native_frame* f, uint32_t kind, frame_event** out)
{
    if (!f || f->state != RSX_NATIVE_FRAME_OPEN) return 0;
    if (f->event_count >= f->event_limit) return frame_fail(f, "event capacity");
    frame_event* e = &f->events[f->event_count++];
    memset(e, 0, sizeof(*e)); e->kind = kind; *out = e; return 1;
}

static int frame_resource_index(rsx_native_frame* f,
    rsx_native_frame_resource_ref ref, ID3D12GraphicsCommandList* list,
    uint32_t* out)
{
    if (!f || !ref.resource || !out) return 0;
    for (uint32_t i = 0; i < f->resource_count; ++i) {
        frame_resource* r = &f->resources[i];
        if (r->original == ref.resource) {
            if (r->mutable_resource != !!ref.mutable_resource)
                return frame_fail(f, "resource state/ownership mismatch");
            r->capture_state=ref.state;
            *out = i; return 1;
        }
    }
    if (f->resource_count >= RSX_NATIVE_FRAME_MAX_RESOURCES)
        return frame_fail(f, "resource capacity");
    frame_resource* r = &f->resources[f->resource_count];
    memset(r, 0, sizeof(*r)); r->original = ref.resource;
    ref.resource->lpVtbl->GetDesc(ref.resource,&r->desc);
    r->capture_state = ref.state; r->replay_state = ref.state; r->mutable_resource = !!ref.mutable_resource;
    ref.resource->lpVtbl->AddRef(ref.resource);
    if (r->mutable_resource) {
        D3D12_RESOURCE_ALLOCATION_INFO ai={0};
        f->device->lpVtbl->GetResourceAllocationInfo(f->device,&ai,0,1,&r->desc);
        if (ai.SizeInBytes == UINT64_MAX || ai.SizeInBytes > RSX_NATIVE_FRAME_MAX_GPU_BYTES - f->gpu_bytes) {
            ref.resource->lpVtbl->Release(ref.resource);memset(r,0,sizeof(*r));return frame_fail(f, "GPU resource budget");}
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr = f->device->lpVtbl->CreateCommittedResource(f->device, &hp,
            D3D12_HEAP_FLAG_NONE, &r->desc, D3D12_RESOURCE_STATE_COPY_DEST,
            NULL, &IID_ID3D12Resource, (void**)&r->clone);
        if (FAILED(hr)) { ref.resource->lpVtbl->Release(ref.resource); memset(r,0,sizeof(*r)); return frame_fail(f, "clone allocation"); }
        r->bytes = ai.SizeInBytes; f->gpu_bytes += ai.SizeInBytes;
        D3D12_RESOURCE_BARRIER b[2] = {0};
        b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition.pResource = ref.resource; b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = ref.state; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->lpVtbl->ResourceBarrier(list, 1, &b[0]);
        list->lpVtbl->CopyResource(list, r->clone, ref.resource);
        b[1] = b[0]; b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1].Transition.StateAfter = ref.state; list->lpVtbl->ResourceBarrier(list, 1, &b[1]);
        D3D12_RESOURCE_BARRIER c = {0}; c.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        c.Transition.pResource = r->clone; c.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        c.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; c.Transition.StateAfter = ref.state;
        list->lpVtbl->ResourceBarrier(list, 1, &c); r->initialized = 1;
    } else {
        r->clone = ref.resource; r->clone->lpVtbl->AddRef(r->clone); r->initialized = 1;
    }
    *out = f->resource_count++; return 1;
}

static void frame_transition(ID3D12GraphicsCommandList* list,
    frame_resource* r, D3D12_RESOURCE_STATES after)
{
    if (r->replay_state == after) return;
    D3D12_RESOURCE_BARRIER b = {0}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r->clone; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = r->replay_state; b.Transition.StateAfter = after;
    list->lpVtbl->ResourceBarrier(list, 1, &b); r->replay_state = after;
}

static int frame_heap(rsx_native_frame* f)
{
    if (f->srv_heap) return 1;
    D3D12_DESCRIPTOR_HEAP_DESC d = {0};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = FRAME_MAX_BINDING_DESCRIPTORS;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(f->device->lpVtbl->CreateDescriptorHeap(f->device, &d,
        &IID_ID3D12DescriptorHeap, (void**)&f->srv_heap))) return 0;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(f->device->lpVtbl->CreateDescriptorHeap(f->device, &d,
        &IID_ID3D12DescriptorHeap, (void**)&f->owned_binding_heap))) return 0;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    f->srv_stride = f->device->lpVtbl->GetDescriptorHandleIncrementSize(
        f->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; d.NumDescriptors = FRAME_SAMPLER_HEAP_DESCRIPTORS;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(f->device->lpVtbl->CreateDescriptorHeap(f->device, &d,
        &IID_ID3D12DescriptorHeap, (void**)&f->sampler_heaps[0]))) return 0;
    f->sampler_heap_count=1;
    f->sampler_stride = f->device->lpVtbl->GetDescriptorHandleIncrementSize(
        f->device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (!f->srv_stride || !f->sampler_stride) return 0;
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    d.NumDescriptors = f->event_limit * 4u;
    if (FAILED(f->device->lpVtbl->CreateDescriptorHeap(f->device,&d,&IID_ID3D12DescriptorHeap,(void**)&f->rtv_heap))) return 0;
    f->rtv_stride=f->device->lpVtbl->GetDescriptorHandleIncrementSize(f->device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; d.NumDescriptors = f->event_limit;
    if (FAILED(f->device->lpVtbl->CreateDescriptorHeap(f->device,&d,&IID_ID3D12DescriptorHeap,(void**)&f->dsv_heap))) return 0;
    f->dsv_stride=f->device->lpVtbl->GetDescriptorHandleIncrementSize(f->device,D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return f->rtv_stride && f->dsv_stride;
}

static D3D12_CPU_DESCRIPTOR_HANDLE frame_cpu(rsx_native_frame* f, uint32_t slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    f->srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(f->srv_heap,&h);
    h.ptr += (SIZE_T)slot * f->srv_stride; return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE frame_owned_binding_cpu(rsx_native_frame* f, uint32_t slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    f->owned_binding_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(f->owned_binding_heap,&h);
    h.ptr += (SIZE_T)slot * f->srv_stride; return h;
}
static D3D12_GPU_DESCRIPTOR_HANDLE frame_gpu(rsx_native_frame* f, uint32_t slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    f->srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(f->srv_heap,&h);
    h.ptr += (UINT64)slot * f->srv_stride; return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE frame_sampler_cpu(rsx_native_frame* f, uint32_t slot)
{
    uint32_t heap=slot/FRAME_SAMPLER_HEAP_DESCRIPTORS,within=slot%FRAME_SAMPLER_HEAP_DESCRIPTORS;
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    f->sampler_heaps[heap]->lpVtbl->GetCPUDescriptorHandleForHeapStart(f->sampler_heaps[heap],&h);
    h.ptr += (SIZE_T)within * f->sampler_stride; return h;
}
static D3D12_GPU_DESCRIPTOR_HANDLE frame_sampler_gpu(rsx_native_frame* f, uint32_t slot)
{
    uint32_t heap=slot/FRAME_SAMPLER_HEAP_DESCRIPTORS,within=slot%FRAME_SAMPLER_HEAP_DESCRIPTORS;
    D3D12_GPU_DESCRIPTOR_HANDLE h;
    f->sampler_heaps[heap]->lpVtbl->GetGPUDescriptorHandleForHeapStart(f->sampler_heaps[heap],&h);
    h.ptr += (UINT64)within * f->sampler_stride; return h;
}
static ID3D12DescriptorHeap* frame_sampler_heap_for_slot(rsx_native_frame* f,uint32_t slot)
{uint32_t heap=slot/FRAME_SAMPLER_HEAP_DESCRIPTORS;return heap<f->sampler_heap_count?f->sampler_heaps[heap]:NULL;}

static void frame_create_srv(rsx_native_frame* f, D3D12_CPU_DESCRIPTOR_HANDLE dst,
    const rsx_native_frame_binding* b)
{
    if (b && b->enabled) {
        uint32_t index = 0;
        /* Resource already registered during capture; descriptor uses its clone. */
        while (index < f->resource_count && f->resources[index].original != b->ref.resource) index++;
        ID3D12Resource* resource = index < f->resource_count ? f->resources[index].clone : b->ref.resource;
        f->device->lpVtbl->CreateShaderResourceView(f->device, resource, &b->view.srv, dst);
    } else {
        D3D12_SHADER_RESOURCE_VIEW_DESC n = {0}; n.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        n.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; n.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        n.Texture2D.MipLevels = 1; f->device->lpVtbl->CreateShaderResourceView(f->device, NULL, &n, dst);
    }
}

static int frame_copy_sampler(rsx_native_frame* f, uint32_t base,
    const D3D12_CPU_DESCRIPTOR_HANDLE src[16])
{
    if (base > RSX_NATIVE_FRAME_MAX_EVENTS*16u-16u) return 0;
    uint32_t needed=base/FRAME_SAMPLER_HEAP_DESCRIPTORS;
    while(f->sampler_heap_count<=needed){
        if(f->sampler_heap_count>=FRAME_MAX_SAMPLER_HEAPS)return 0;
        D3D12_DESCRIPTOR_HEAP_DESC d={0};d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        d.NumDescriptors=FRAME_SAMPLER_HEAP_DESCRIPTORS;d.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(f->device->lpVtbl->CreateDescriptorHeap(f->device,&d,&IID_ID3D12DescriptorHeap,
            (void**)&f->sampler_heaps[f->sampler_heap_count])))return 0;
        f->sampler_heap_count++;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dst = frame_sampler_cpu(f, base);
    for (UINT i = 0; i < 16; ++i) {
        if (src && src[i].ptr) f->device->lpVtbl->CopyDescriptorsSimple(f->device, 1,
            (D3D12_CPU_DESCRIPTOR_HANDLE){dst.ptr + (SIZE_T)i * f->sampler_stride}, src[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        else return 0;
    }
    return 1;
}

static void frame_create_binding(rsx_native_frame* f,D3D12_CPU_DESCRIPTOR_HANDLE dst,
    const rsx_native_frame_binding* b)
{
    if(!b||!b->enabled){
        if(b&&b->immutable_descriptor.ptr)f->device->lpVtbl->CopyDescriptorsSimple(f->device,1,dst,b->immutable_descriptor,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        else if(b&&b->kind==RSX_NATIVE_FRAME_BINDING_UAV){D3D12_UNORDERED_ACCESS_VIEW_DESC n={0};n.Format=DXGI_FORMAT_R8G8B8A8_UNORM;n.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;f->device->lpVtbl->CreateUnorderedAccessView(f->device,NULL,NULL,&n,dst);}
        else frame_create_srv(f,dst,NULL);
        return;
    }
    ID3D12Resource* resource=NULL;
    for(UINT i=0;i<f->resource_count;i++)if(f->resources[i].original==b->ref.resource){resource=f->resources[i].clone;break;}
    if(b->kind==RSX_NATIVE_FRAME_BINDING_UAV)
        f->device->lpVtbl->CreateUnorderedAccessView(f->device,resource,NULL,&b->view.uav,dst);
    else f->device->lpVtbl->CreateShaderResourceView(f->device,resource,&b->view.srv,dst);
}

static D3D12_CPU_DESCRIPTOR_HANDLE frame_rtv(rsx_native_frame* f,UINT slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    f->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(f->rtv_heap,&h);
    h.ptr+=(SIZE_T)slot*f->rtv_stride;return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE frame_dsv(rsx_native_frame* f,UINT slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    f->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(f->dsv_heap,&h);
    h.ptr+=(SIZE_T)slot*f->dsv_stride;return h;
}

static int frame_make_buffer(rsx_native_frame* f,uint64_t bytes,D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_STATES state,ID3D12Resource** out)
{
    D3D12_RESOURCE_DESC d={0};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
    d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=heap;
    return SUCCEEDED(f->device->lpVtbl->CreateCommittedResource(f->device,&hp,D3D12_HEAP_FLAG_NONE,
        &d,state,NULL,&IID_ID3D12Resource,(void**)out));
}

static int frame_prepare_replay_upload(rsx_native_frame* f)
{
    if(!f->upload_used)return 1;
    uint64_t bytes=(f->upload_used+255u)&~UINT64_C(255);
    if(bytes>RSX_NATIVE_FRAME_MAX_GPU_BYTES-f->gpu_bytes)return 0;
    if(!frame_make_buffer(f,bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,&f->replay_upload))return 0;
    void* mapped=NULL;D3D12_RANGE read={0,0};HRESULT hr=f->replay_upload->lpVtbl->Map(f->replay_upload,0,&read,&mapped);
    if(FAILED(hr)||!mapped)return 0;memcpy(mapped,f->upload_bytes,f->upload_used);
    D3D12_RANGE written={0,(SIZE_T)f->upload_used};f->replay_upload->lpVtbl->Unmap(f->replay_upload,0,&written);
    f->gpu_bytes+=bytes;return 1;
}

static int frame_record_draw(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    ID3D12GraphicsCommandList9* bias,frame_draw* d,uint32_t ordinal)
{
    rsx_native_draw_packet p=d->packet;p.list=list;p.bias_list=bias;
    p.render_target_count=d->color_count;
    if(p.render_target_count!=d->packet.render_target_count||p.render_target_count>4)return 0;
    for(UINT i=0;i<d->color_count;i++){
        frame_resource* r=&f->resources[d->color_indices[i]];
        if(r->mutable_resource)frame_transition(list,r,D3D12_RESOURCE_STATE_RENDER_TARGET);
        else if(r->replay_state!=D3D12_RESOURCE_STATE_RENDER_TARGET)return 0;
        D3D12_CPU_DESCRIPTOR_HANDLE h=frame_rtv(f,ordinal*4u+i);
        f->device->lpVtbl->CreateRenderTargetView(f->device,r->clone,&d->color_views[i],h);
        /* Preserve the caller's typed view in the event's target table. */
        p.render_targets[i]=h;
    }
    if(d->has_depth){
        frame_resource* r=&f->resources[d->depth_index];
        if(r->mutable_resource)frame_transition(list,r,D3D12_RESOURCE_STATE_DEPTH_WRITE);
        else if(r->replay_state!=D3D12_RESOURCE_STATE_DEPTH_WRITE)return 0;
        p.depth_stencil=frame_dsv(f,ordinal);
        f->device->lpVtbl->CreateDepthStencilView(f->device,r->clone,&d->depth_view,p.depth_stencil);
    }
    for(UINT slot=0;slot<16;slot++){
        const rsx_native_frame_binding* sets[2]={&d->srv[slot],&d->vtex[slot]};
        for(UINT set=0;set<2;set++)if(sets[set]->enabled){
            UINT ri;for(ri=0;ri<f->resource_count&&f->resources[ri].original!=sets[set]->ref.resource;ri++) { }
            if(ri==f->resource_count)return 0;frame_resource* r=&f->resources[ri];
            int transitioned_by_packet=0;for(UINT t=0;t<p.transition_count;t++)if(p.transition_resources[t]==r->original)transitioned_by_packet=1;
            if(r->mutable_resource&&!transitioned_by_packet)frame_transition(list,r,sets[set]->ref.state);
            else if(!r->mutable_resource)frame_transition(list,r,sets[set]->ref.state);
        }
    }
    /* Own descriptor contents at capture time. Replay does not rely on
       renderer CPU handles or descriptor heaps that may be rewritten. */
    UINT base=d->descriptor_base;
    for(UINT i=0;i<16;i++){
        f->device->lpVtbl->CopyDescriptorsSimple(f->device,1,frame_cpu(f,base+i),
            frame_owned_binding_cpu(f,base+i),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (p.has_vertex_texture_table)
            f->device->lpVtbl->CopyDescriptorsSimple(f->device,1,frame_cpu(f,base+16u+i),
                frame_owned_binding_cpu(f,base+16u+i),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    p.descriptor_heaps[0]=f->srv_heap;p.descriptor_heaps[1]=frame_sampler_heap_for_slot(f,d->sampler_base);
    p.srv_table=frame_gpu(f,base);if(p.has_vertex_texture_table)p.vertex_texture_table=frame_gpu(f,base+16u);
    p.sampler_table=frame_sampler_gpu(f,d->sampler_base);
    ID3D12DescriptorHeap* heaps[2]={f->srv_heap,frame_sampler_heap_for_slot(f,d->sampler_base)};
    list->lpVtbl->SetDescriptorHeaps(list,2,heaps);
    if(f->replay_upload){D3D12_GPU_VIRTUAL_ADDRESS va=f->replay_upload->lpVtbl->GetGPUVirtualAddress(f->replay_upload);
        if(p.has_vertex_buffer){p.vertex_buffer.BufferLocation=va+d->payload_offsets[0];p.vertex_buffer.SizeInBytes=(UINT)d->payload_sizes[0];}
        if(p.indexed){p.index_buffer.BufferLocation=va+d->payload_offsets[1];p.index_buffer.SizeInBytes=(UINT)d->payload_sizes[1];}
        if(d->payload_sizes[2])p.vertex_constant_buffer=va+d->payload_offsets[2];
        if(d->payload_sizes[3])p.pixel_constant_buffer=va+d->payload_offsets[3];
        if(p.has_pull_shader_resource)p.pull_shader_resource=va+d->payload_offsets[0];
    }
    for(UINT i=0;i<p.transition_count;i++){
        int found=0;
        for(UINT j=0;j<f->resource_count;j++)if(f->resources[j].original==p.transition_resources[i]){
            p.transition_resources[i]=f->resources[j].clone;
            if(f->resources[j].replay_state!=D3D12_RESOURCE_STATE_RENDER_TARGET)return 0;
            found=1;break;
        }
        if(!found)return 0;
    }
    rsx_native_draw_packet_begin(&p);
    rsx_native_draw_packet_targets(&p);
    rsx_native_draw_packet_bind(&p);
    rsx_native_draw_packet_draw(&p);
    rsx_native_draw_packet_end(&p);
    for(UINT i=0;i<p.transition_count;i++)for(UINT j=0;j<f->resource_count;j++)
        if(f->resources[j].clone==p.transition_resources[i])f->resources[j].replay_state=D3D12_RESOURCE_STATE_RENDER_TARGET;
    return 1;
}

static int frame_record_clear_color(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    frame_clear_color* c,UINT slot)
{
    frame_resource* r=&f->resources[c->target];
    if(r->mutable_resource)frame_transition(list,r,D3D12_RESOURCE_STATE_RENDER_TARGET);
    else if(r->replay_state!=D3D12_RESOURCE_STATE_RENDER_TARGET)return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE h=frame_rtv(f,slot);
    f->device->lpVtbl->CreateRenderTargetView(f->device,r->clone,&c->view,h);
    HRESULT hr=rsx_color_clear_record(&f->clear_helper,f->device,list,h,c->format,c->width,c->height,
        &c->rect,c->channels,c->color);
    return SUCCEEDED(hr);
}

static int frame_record_clear_depth(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    frame_clear_depth* c,UINT slot)
{
    frame_resource* r=&f->resources[c->target];
    if(r->mutable_resource)frame_transition(list,r,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    else if(r->replay_state!=D3D12_RESOURCE_STATE_DEPTH_WRITE)return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE h=frame_dsv(f,slot);
    f->device->lpVtbl->CreateDepthStencilView(f->device,r->clone,&c->view,h);
    list->lpVtbl->ClearDepthStencilView(list,h,c->flags,c->depth,c->stencil,1,&c->rect);
    return 1;
}

static int frame_record_compute(rsx_native_frame* f,ID3D12GraphicsCommandList* list,frame_compute* c)
{
    ID3D12DescriptorHeap* heaps[1]={f->srv_heap};list->lpVtbl->SetDescriptorHeaps(list,1,heaps);
    list->lpVtbl->SetComputeRootSignature(list,c->root);list->lpVtbl->SetPipelineState(list,c->pso);
    for(UINT t=0;t<c->table_count;t++){
        UINT base=c->tables[t].descriptor_base;
        for(UINT b=0;b<c->tables[t].count;b++){
            rsx_native_frame_binding* binding=&c->tables[t].bindings[b];
            if(binding->enabled){
                UINT ri;for(ri=0;ri<f->resource_count&&f->resources[ri].original!=binding->ref.resource;ri++) { }
                if(ri==f->resource_count)return 0;
                frame_resource* r=&f->resources[ri];
                if(r->mutable_resource){
                    D3D12_RESOURCE_STATES desired=binding->kind==RSX_NATIVE_FRAME_BINDING_UAV?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    if(binding->kind==RSX_NATIVE_FRAME_BINDING_UAV&&r->replay_state==D3D12_RESOURCE_STATE_UNORDERED_ACCESS){
                        D3D12_RESOURCE_BARRIER u={0};u.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;u.UAV.pResource=r->clone;list->lpVtbl->ResourceBarrier(list,1,&u);
                    }else frame_transition(list,r,desired);
                }else frame_transition(list,r,binding->ref.state);
            }
            f->device->lpVtbl->CopyDescriptorsSimple(f->device,1,frame_cpu(f,base+b),
                frame_owned_binding_cpu(f,base+b),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        list->lpVtbl->SetComputeRootDescriptorTable(list,c->tables[t].root,frame_gpu(f,base));
    }
    for(UINT i=0;i<c->constants_count;i++)list->lpVtbl->SetComputeRoot32BitConstants(list,
        c->constants[i].root_index,c->constants[i].value_count,c->constants[i].values,c->constants[i].destination_offset);
    list->lpVtbl->Dispatch(list,c->groups[0],c->groups[1],c->groups[2]);return 1;
}

static int frame_record_upload(rsx_native_frame* f,ID3D12GraphicsCommandList* list,frame_upload* u)
{
    frame_resource* dst=&f->resources[u->dst];D3D12_RESOURCE_DESC rd=dst->desc;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT rows;UINT64 rowbytes,total;
    f->device->lpVtbl->GetCopyableFootprints(f->device,&rd,u->subresource,1,0,&fp,&rows,&rowbytes,&total);
    if(rows!=u->rows||rowbytes!=u->row_bytes||total>RSX_NATIVE_FRAME_MAX_GPU_BYTES-f->gpu_bytes)return 0;
    if(!frame_make_buffer(f,total,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,&u->staging))return 0;
    void* mapped=NULL;D3D12_RANGE read={0,0};if(FAILED(u->staging->lpVtbl->Map(u->staging,0,&read,&mapped))||!mapped)return 0;
    for(UINT y=0;y<rows;y++)memcpy((uint8_t*)mapped+fp.Offset+(size_t)y*fp.Footprint.RowPitch,
        f->upload_bytes+u->offset+(size_t)y*(size_t)rowbytes,(size_t)rowbytes);
    D3D12_RANGE written={0,(SIZE_T)total};u->staging->lpVtbl->Unmap(u->staging,0,&written);f->gpu_bytes+=total;
    if(dst->mutable_resource)frame_transition(list,dst,D3D12_RESOURCE_STATE_COPY_DEST);
    else if(dst->replay_state!=D3D12_RESOURCE_STATE_COPY_DEST)return 0;
    D3D12_TEXTURE_COPY_LOCATION sl={0},dl={0};sl.pResource=u->staging;sl.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;sl.PlacedFootprint=fp;
    dl.pResource=dst->clone;dl.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dl.SubresourceIndex=u->subresource;
    list->lpVtbl->CopyTextureRegion(list,&dl,0,0,0,&sl,NULL);
    if(dst->mutable_resource)frame_transition(list,dst,u->state);return 1;
}

rsx_native_frame* rsx_native_frame_create(ID3D12Device* device, uint32_t max_events,
    uint64_t max_owned_upload_bytes)
{
    if (!device || !max_events || max_events > RSX_NATIVE_FRAME_MAX_EVENTS ||
        !max_owned_upload_bytes || max_owned_upload_bytes > SIZE_MAX) return NULL;
    rsx_native_frame* f = (rsx_native_frame*)calloc(1, sizeof(*f)); if (!f) return NULL;
    f->device = device; device->lpVtbl->AddRef(device); f->state = RSX_NATIVE_FRAME_OPEN;
    f->event_limit = max_events; f->upload_limit = (size_t)max_owned_upload_bytes;
    f->events = (frame_event*)calloc(max_events, sizeof(frame_event));
    if (!f->events) { rsx_native_frame_destroy(f); return NULL; }
    return f;
}

void rsx_native_frame_mark_unsupported(rsx_native_frame* f, const char* reason)
{ (void)frame_fail(f, reason); }
int rsx_native_frame_set_coverage_verified(rsx_native_frame* f)
{if(!f||f->state!=RSX_NATIVE_FRAME_OPEN||f->unsupported_count)return 0;f->coverage_verified=1;return 1;}

void rsx_native_frame_destroy(rsx_native_frame* f)
{
    if (!f) return;
    for (uint32_t i=0;i<f->resource_count;i++) {
        if (f->resources[i].clone) f->resources[i].clone->lpVtbl->Release(f->resources[i].clone);
        if (f->resources[i].original) f->resources[i].original->lpVtbl->Release(f->resources[i].original);
    }
    for (uint32_t i=0;i<f->event_count;i++) {
        if (f->events[i].kind==RSX_NATIVE_FRAME_COMPUTE) {
            if (f->events[i].u.compute.pso) f->events[i].u.compute.pso->lpVtbl->Release(f->events[i].u.compute.pso);
            if (f->events[i].u.compute.root) f->events[i].u.compute.root->lpVtbl->Release(f->events[i].u.compute.root);
        } else if (f->events[i].kind==RSX_NATIVE_FRAME_DRAW) {
            rsx_native_draw_packet* p=&f->events[i].u.draw.packet;
            if(p->pipeline_state)p->pipeline_state->lpVtbl->Release(p->pipeline_state);
            if(p->root_signature)p->root_signature->lpVtbl->Release(p->root_signature);
        } else if(f->events[i].kind==RSX_NATIVE_FRAME_UPLOAD&&f->events[i].u.upload.staging) {
            f->events[i].u.upload.staging->lpVtbl->Release(f->events[i].u.upload.staging);
        }
    }
    if (f->compare_readback[0]) f->compare_readback[0]->lpVtbl->Release(f->compare_readback[0]);
    if (f->compare_readback[1]) f->compare_readback[1]->lpVtbl->Release(f->compare_readback[1]);
    if (f->replay_upload) f->replay_upload->lpVtbl->Release(f->replay_upload);
    if (f->srv_heap) f->srv_heap->lpVtbl->Release(f->srv_heap);
    if (f->owned_binding_heap) f->owned_binding_heap->lpVtbl->Release(f->owned_binding_heap);
    for(UINT i=0;i<f->sampler_heap_count;i++)if(f->sampler_heaps[i])f->sampler_heaps[i]->lpVtbl->Release(f->sampler_heaps[i]);
    if (f->rtv_heap) f->rtv_heap->lpVtbl->Release(f->rtv_heap);
    if (f->dsv_heap) f->dsv_heap->lpVtbl->Release(f->dsv_heap);
    rsx_color_clear_release(&f->clear_helper);
    free(f->upload_bytes); free(f->events);
    if (f->device) f->device->lpVtbl->Release(f->device); free(f);
}

int rsx_native_frame_capture_clear_color(rsx_native_frame* f, ID3D12GraphicsCommandList* list,
    const rsx_native_frame_color_target* target, DXGI_FORMAT format, UINT width, UINT height,
    const D3D12_RECT* rect, UINT channels, const FLOAT color[4])
{
    if (!frame_ready(f,list)||!target||!color||!width||!height||!channels||!target->ref.mutable_resource||target->view.Format!=format) return frame_fail(f,"bad color clear");
    uint32_t ri; if (!frame_resource_index(f,target->ref,list,&ri)) return 0;
    frame_event* e; if (!frame_push(f,RSX_NATIVE_FRAME_CLEAR_COLOR,&e)) return 0;
    e->u.clear_color.target=ri; e->u.clear_color.format=format; e->u.clear_color.width=width; e->u.clear_color.height=height;
    e->u.clear_color.state=target->ref.state;
    e->u.clear_color.view=target->view;
    e->u.clear_color.rect=rect?*rect:rsx_clear_rect(width,height,0,0,0);
    e->u.clear_color.channels=channels; memcpy(e->u.clear_color.color,color,4*sizeof(FLOAT)); return 1;
}

int rsx_native_frame_capture_clear_depth(rsx_native_frame* f, ID3D12GraphicsCommandList* list,
    const rsx_native_frame_depth_target* target,const D3D12_RECT* rect,FLOAT depth,UINT8 stencil,D3D12_CLEAR_FLAGS flags)
{
    if (!frame_ready(f,list)||!target||!target->ref.mutable_resource||!rect) return frame_fail(f,"bad depth clear");
    uint32_t ri; if (!frame_resource_index(f,target->ref,list,&ri)) return 0;
    frame_event* e; if(!frame_push(f,RSX_NATIVE_FRAME_CLEAR_DEPTH,&e))return 0;
    e->u.clear_depth.target=ri; e->u.clear_depth.state=target->ref.state;e->u.clear_depth.view=target->view;e->u.clear_depth.rect=rect?*rect:(D3D12_RECT){0,0,LONG_MAX,LONG_MAX};
    e->u.clear_depth.depth=depth; e->u.clear_depth.stencil=stencil; e->u.clear_depth.flags=flags; return 1;
}

int rsx_native_frame_capture_copy(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    rsx_native_frame_resource_ref source,UINT source_sub,const D3D12_BOX* box,
    rsx_native_frame_resource_ref destination,UINT destination_sub,UINT x,UINT y,UINT z)
{
    if(!frame_ready(f,list)||!destination.mutable_resource)return frame_fail(f,"bad copy"); uint32_t s,d;
    if(!frame_resource_index(f,source,list,&s)||!frame_resource_index(f,destination,list,&d))return 0;
    if(s==d)return frame_fail(f,"copy alias");
    frame_event* e;if(!frame_push(f,RSX_NATIVE_FRAME_COPY,&e))return 0;
    e->u.copy.src=s;e->u.copy.dst=d;e->u.copy.src_state=source.state;e->u.copy.dst_state=destination.state;e->u.copy.src_sub=source_sub;e->u.copy.dst_sub=destination_sub;
    e->u.copy.x=x;e->u.copy.y=y;e->u.copy.z=z;if(box){e->u.copy.box=*box;e->u.copy.has_box=1;}return 1;
}

int rsx_native_frame_capture_copy_resource(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    rsx_native_frame_resource_ref source,rsx_native_frame_resource_ref destination)
{
    if(!frame_ready(f,list)||!destination.mutable_resource)return frame_fail(f,"bad resource copy");uint32_t s,d;
    if(!frame_resource_index(f,source,list,&s)||!frame_resource_index(f,destination,list,&d))return 0;
    if(s==d)return frame_fail(f,"resource copy alias");
    frame_event*e;if(!frame_push(f,RSX_NATIVE_FRAME_COPY,&e))return 0;
    e->u.copy.src=s;e->u.copy.dst=d;e->u.copy.src_state=source.state;e->u.copy.dst_state=destination.state;
    e->u.copy.full_resource=1;return 1;
}

static int frame_copy_binding(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    const rsx_native_frame_binding* b)
{
    if(!b->enabled)return 1;uint32_t i;
    return frame_resource_index(f,b->ref,list,&i);
}

int rsx_native_frame_capture_compute(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    const rsx_native_frame_compute_desc* c)
{
    if(!frame_ready(f,list)||!c||!c->pipeline_state||!c->root_signature||c->table_count>4||c->constants_count>16)
        return frame_fail(f,"bad compute");
    if(!frame_heap(f))return frame_fail(f,"descriptor heap allocation");
    for(UINT t=0;t<c->table_count;t++){if(!c->tables[t].binding_count||c->tables[t].binding_count>16)return frame_fail(f,"compute bindings");
        for(UINT b=0;b<c->tables[t].binding_count;b++){
            const rsx_native_frame_binding* binding=&c->tables[t].bindings[b];
            if(binding->enabled&&(binding->kind!=RSX_NATIVE_FRAME_BINDING_SRV&&binding->kind!=RSX_NATIVE_FRAME_BINDING_UAV))return frame_fail(f,"compute binding type");
            if(!binding->enabled&&!binding->immutable_descriptor.ptr)return frame_fail(f,"compute null descriptor missing");
            if(binding->enabled&&binding->kind==RSX_NATIVE_FRAME_BINDING_UAV&&!binding->ref.mutable_resource)return frame_fail(f,"immutable UAV");
            if(!frame_copy_binding(f,list,binding))return 0;
        }}
    for(UINT k=0;k<c->constants_count;k++)if(c->constants[k].value_count>16)return frame_fail(f,"compute constants");
    UINT descriptor_count=0;for(UINT t=0;t<c->table_count;t++)descriptor_count+=c->tables[t].binding_count;
    if(descriptor_count>FRAME_MAX_BINDING_DESCRIPTORS-f->descriptor_cursor)return frame_fail(f,"binding descriptor capacity");
    frame_event* e;if(!frame_push(f,RSX_NATIVE_FRAME_COMPUTE,&e))return 0; frame_compute* o=&e->u.compute;
    o->pso=c->pipeline_state;o->root=c->root_signature;o->pso->lpVtbl->AddRef(o->pso);o->root->lpVtbl->AddRef(o->root);
    o->groups[0]=c->groups_x;o->groups[1]=c->groups_y;o->groups[2]=c->groups_z;
    o->table_count=c->table_count;o->constants_count=c->constants_count;memcpy(o->constants,c->constants,sizeof(o->constants));
    for(UINT t=0;t<c->table_count;t++){
        o->tables[t].root=c->tables[t].root_index;o->tables[t].count=c->tables[t].binding_count;
        o->tables[t].descriptor_base=f->descriptor_cursor;
        for(UINT prior=0;prior<t;prior++)o->tables[t].descriptor_base+=o->tables[prior].count;
        memcpy(o->tables[t].bindings,c->tables[t].bindings,sizeof(o->tables[t].bindings));
        for(UINT b=0;b<o->tables[t].count;b++){
            UINT slot=o->tables[t].descriptor_base+b;
            frame_create_binding(f,frame_owned_binding_cpu(f,slot),&o->tables[t].bindings[b]);
            o->tables[t].bindings[b].immutable_descriptor.ptr=0;
        }
    }
    f->descriptor_cursor+=descriptor_count;
    return 1;
}

static int frame_append_upload(rsx_native_frame* f,const void* p,size_t n,size_t* offset)
{
    if(!p||!n||f->upload_used>f->upload_limit||n>f->upload_limit-f->upload_used)return frame_fail(f,"upload payload budget");
    size_t aligned=(f->upload_used+FRAME_UPLOAD_ALIGNMENT-1)&~(size_t)(FRAME_UPLOAD_ALIGNMENT-1);
    if(aligned>f->upload_limit||n>f->upload_limit-aligned)return frame_fail(f,"upload payload budget");
    if(aligned+n>f->upload_capacity){size_t cap=f->upload_capacity?f->upload_capacity:65536;
        while(cap<aligned+n){if(cap>f->upload_limit/2){cap=f->upload_limit;break;}cap*=2;}
        uint8_t* q=(uint8_t*)realloc(f->upload_bytes,cap);if(!q)return frame_fail(f,"upload allocation");f->upload_bytes=q;f->upload_capacity=cap;}
    memcpy(f->upload_bytes+aligned,p,n);*offset=aligned;f->upload_used=aligned+n;return 1;
}

static int frame_copy_payload(rsx_native_frame* f,
    const rsx_native_draw_payload_store* store,
    const rsx_native_draw_payload_record* record,
    rsx_native_draw_payload_kind kind, size_t* offset, size_t* size)
{
    const void* bytes=NULL; size_t n=0;
    if (!rsx_native_draw_payload_get_bytes(store,record,kind,&bytes,&n) || !n) {
        *offset=0; *size=0; return 1;
    }
    if (!frame_append_upload(f,bytes,n,offset)) return 0;
    *size=n; return 1;
}

int rsx_native_frame_capture_upload(rsx_native_frame* f,ID3D12GraphicsCommandList* list,const rsx_native_frame_upload_desc* u)
{
    if(!frame_ready(f,list)||!u||!u->source_bytes||!u->width||!u->height||u->depth!=1||!u->destination.mutable_resource)
        return frame_fail(f,"bad upload");
    uint32_t d;if(!frame_resource_index(f,u->destination,list,&d))return 0;
    D3D12_RESOURCE_DESC rd=f->resources[d].desc;if(rd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||rd.SampleDesc.Count!=1||rd.MipLevels==0)
        return frame_fail(f,"unsupported upload texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT rows;UINT64 rowbytes,total;
    f->device->lpVtbl->GetCopyableFootprints(f->device,&rd,u->subresource,1,0,&fp,&rows,&rowbytes,&total);
    if(!rowbytes||!rows||rowbytes>SIZE_MAX/rows||u->source_row_pitch<rowbytes||u->source_row_pitch>SIZE_MAX/rows||
        u->source_size<u->source_row_pitch*rows)return frame_fail(f,"upload layout");
    if(fp.Footprint.Width!=u->width||fp.Footprint.Height!=u->height||fp.Footprint.Format!=u->format)
        return frame_fail(f,"upload description mismatch");
    size_t tight=(size_t)rowbytes*rows;uint8_t* tightbytes=(uint8_t*)malloc(tight);if(!tightbytes)return frame_fail(f,"upload staging allocation");
    for(UINT r=0;r<rows;r++)memcpy(tightbytes+(size_t)r*(size_t)rowbytes,(const uint8_t*)u->source_bytes+(size_t)r*u->source_row_pitch,(size_t)rowbytes);
    size_t off;int ok=frame_append_upload(f,tightbytes,tight,&off);free(tightbytes);if(!ok)return 0;
    frame_event*e;if(!frame_push(f,RSX_NATIVE_FRAME_UPLOAD,&e))return 0;e->u.upload.dst=d;e->u.upload.state=u->destination.state;e->u.upload.subresource=u->subresource;e->u.upload.offset=off;e->u.upload.size=tight;e->u.upload.rows=rows;e->u.upload.row_bytes=rowbytes;return 1;
}

int rsx_native_frame_capture_draw(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    const rsx_native_draw_packet* p,const rsx_native_draw_payload_store* payload,
    const rsx_native_frame_color_target* colors,UINT color_count,const rsx_native_frame_depth_target* depth,
    const rsx_native_frame_binding srv[16],const D3D12_CPU_DESCRIPTOR_HANDLE samplers[16],
    const rsx_native_frame_binding vtex[16])
{
    if(!frame_ready(f,list)||!p||!payload||!colors||!color_count||color_count>4||p->transition_count>RSX_NATIVE_DRAW_MAX_TRANSITIONS||p->render_target_count!=color_count||!srv||!samplers||(p->has_vertex_texture_table&&!vtex))
        return frame_fail(f,"bad draw");
    UINT descriptor_count=p->has_vertex_texture_table?32u:16u;
    if(descriptor_count>FRAME_MAX_BINDING_DESCRIPTORS-f->descriptor_cursor)return frame_fail(f,"binding descriptor capacity");
    if(rsx_native_draw_payload_get_state(payload)!=RSX_NATIVE_DRAW_PAYLOAD_SEALED)return frame_fail(f,"payload not sealed");
    if(!frame_heap(f))return frame_fail(f,"descriptor heap allocation");
    uint32_t ci[4],di=0;for(UINT i=0;i<color_count;i++){if(!colors[i].ref.mutable_resource)return frame_fail(f,"immutable draw target");if(!frame_resource_index(f,colors[i].ref,list,&ci[i]))return 0;}
    if(!!depth!=!!p->has_depth_stencil)return frame_fail(f,"depth signature");if(depth&&(!depth->ref.mutable_resource||!frame_resource_index(f,depth->ref,list,&di)))return frame_fail(f,"immutable depth target");
    for(UINT i=0;i<p->transition_count;i++){
        UINT j;for(j=0;j<f->resource_count&&f->resources[j].original!=p->transition_resources[i];j++) { }
        if(j==f->resource_count||!f->resources[j].mutable_resource)return frame_fail(f,"untracked draw transition");
    }
    for(UINT i=0;i<16;i++){
        if(!srv[i].enabled&&!srv[i].immutable_descriptor.ptr)return frame_fail(f,"graphics null descriptor missing");
        if(srv[i].enabled&&(srv[i].kind!=RSX_NATIVE_FRAME_BINDING_SRV||!frame_copy_binding(f,list,&srv[i])))return frame_fail(f,"unsupported graphics SRV");
        if(p->has_vertex_texture_table&&!vtex[i].enabled&&!vtex[i].immutable_descriptor.ptr)return frame_fail(f,"vertex null descriptor missing");
        if(p->has_vertex_texture_table&&vtex[i].enabled&&(vtex[i].kind!=RSX_NATIVE_FRAME_BINDING_SRV||!frame_copy_binding(f,list,&vtex[i])))return frame_fail(f,"unsupported vertex SRV");
    }
    uint32_t payload_count=rsx_native_draw_payload_count(payload);
    rsx_native_draw_payload_record pr;if(!payload_count||!rsx_native_draw_payload_get_record(payload,payload_count-1,&pr))return frame_fail(f,"payload record");
    if(!p->pipeline_state||!p->root_signature)return frame_fail(f,"draw pipeline missing");
    frame_event*e;if(!frame_push(f,RSX_NATIVE_FRAME_DRAW,&e))return 0;frame_draw* d=&e->u.draw;d->packet=*p;d->packet.list=NULL;
    d->descriptor_base=f->descriptor_cursor;f->descriptor_cursor+=descriptor_count;
    d->packet.pipeline_state->lpVtbl->AddRef(d->packet.pipeline_state);d->packet.root_signature->lpVtbl->AddRef(d->packet.root_signature);
    d->color_count=color_count;memcpy(d->color_indices,ci,color_count*sizeof(UINT));for(UINT i=0;i<color_count;i++){d->color_states[i]=colors[i].ref.state;d->color_views[i]=colors[i].view;}d->has_depth=depth!=NULL;d->depth_index=di;if(depth){d->depth_state=depth->ref.state;d->depth_view=depth->view;}
    if(!frame_copy_payload(f,payload,&pr,RSX_NATIVE_DRAW_PAYLOAD_VERTEX,&d->payload_offsets[0],&d->payload_sizes[0])||
       !frame_copy_payload(f,payload,&pr,RSX_NATIVE_DRAW_PAYLOAD_INDEX,&d->payload_offsets[1],&d->payload_sizes[1])||
       !frame_copy_payload(f,payload,&pr,RSX_NATIVE_DRAW_PAYLOAD_VERTEX_CONSTANTS,&d->payload_offsets[2],&d->payload_sizes[2])||
       !frame_copy_payload(f,payload,&pr,RSX_NATIVE_DRAW_PAYLOAD_PIXEL_CONSTANTS,&d->payload_offsets[3],&d->payload_sizes[3]))return 0;
    if((p->has_vertex_buffer||p->has_pull_shader_resource)&&!d->payload_sizes[0])return frame_fail(f,"missing owned vertex payload");
    if(p->indexed&&!d->payload_sizes[1])return frame_fail(f,"missing owned index payload");
    if(p->vertex_constant_buffer&&!d->payload_sizes[2])return frame_fail(f,"missing owned vertex constants");
    if(p->has_pixel_constant_buffer&&!d->payload_sizes[3])return frame_fail(f,"missing owned pixel constants");
    if(srv)memcpy(d->srv,srv,sizeof(d->srv));if(vtex)memcpy(d->vtex,vtex,sizeof(d->vtex));
    UINT descriptor_base=d->descriptor_base;
    for(UINT i=0;i<16;i++){
        frame_create_binding(f,frame_owned_binding_cpu(f,descriptor_base+i),&d->srv[i]);
        if(p->has_vertex_texture_table)
            frame_create_binding(f,frame_owned_binding_cpu(f,descriptor_base+16u+i),&d->vtex[i]);
        d->srv[i].immutable_descriptor.ptr=0;
        d->vtex[i].immutable_descriptor.ptr=0;
    }
    if(f->sampler_cursor/16>=RSX_NATIVE_FRAME_MAX_EVENTS)return frame_fail(f,"sampler table capacity");
    d->sampler_base=f->sampler_cursor;if(!frame_copy_sampler(f,d->sampler_base,samplers))return frame_fail(f,"sampler descriptors");
    f->sampler_cursor+=16;
    for(UINT i=0;i<16;i++)d->sampler[i]=frame_sampler_cpu(f,d->sampler_base+i);
    return 1;
}

int rsx_native_frame_seal(rsx_native_frame* f)
{
    if(!f||f->state!=RSX_NATIVE_FRAME_OPEN||!f->event_count)return 0;
    for(UINT i=0;i<f->resource_count;i++)if(!f->resources[i].mutable_resource)
        f->resources[i].replay_state=f->resources[i].capture_state;
    f->state=RSX_NATIVE_FRAME_SEALED;return 1;
}

int rsx_native_frame_set_compare_target(rsx_native_frame* f,rsx_native_frame_resource_ref target,const D3D12_RENDER_TARGET_VIEW_DESC* view)
{
    if(!f||f->state!=RSX_NATIVE_FRAME_OPEN||!target.resource||!target.mutable_resource||!view)return 0;
    uint32_t i;/* Registration uses a temporary command list only for clone creation, so compare target must already be registered. */
    for(i=0;i<f->resource_count&&f->resources[i].original!=target.resource;i++) { }
    if(i==f->resource_count)return frame_fail(f,"compare target not registered");
    D3D12_RESOURCE_DESC desc=f->resources[i].desc;
    if(view->Format!=desc.Format)return frame_fail(f,"compare view format");
    f->compare_index=i;f->compare_view=*view;return 1;
}

int rsx_native_frame_capture_compare_reference(rsx_native_frame* f,ID3D12GraphicsCommandList* list,
    rsx_native_frame_resource_ref target,const D3D12_RENDER_TARGET_VIEW_DESC* view)
{
    if(!f||f->state!=RSX_NATIVE_FRAME_OPEN||!list||!view||!target.resource||f->compare_readback[0]||!f->coverage_verified)return 0;
    UINT i;for(i=0;i<f->resource_count&&f->resources[i].original!=target.resource;i++) { }
    if(i==f->resource_count||!f->resources[i].mutable_resource||i!=f->compare_index)return frame_fail(f,"compare target mismatch");
    D3D12_RESOURCE_DESC desc=f->resources[i].desc;
    if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.SampleDesc.Count!=1||desc.MipLevels!=1||desc.DepthOrArraySize!=1)
        return frame_fail(f,"unsupported compare target");
    UINT rows;UINT64 rowbytes,total;f->device->lpVtbl->GetCopyableFootprints(f->device,&desc,0,1,0,
        &f->compare_footprint,&rows,&rowbytes,&total);
    if(!rows||!rowbytes||total>RSX_NATIVE_FRAME_MAX_GPU_BYTES/2u||2*total>RSX_NATIVE_FRAME_MAX_GPU_BYTES-f->gpu_bytes)return frame_fail(f,"compare size");
    for(UINT j=0;j<2;j++)if(!frame_make_buffer(f,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,&f->compare_readback[j]))
        return frame_fail(f,"compare readback allocation");
    f->gpu_bytes+=2*total;f->compare_row_bytes=rowbytes;f->compare_rows=rows;f->compare_readback_size=total;
    frame_resource* original=&f->resources[i];
    D3D12_RESOURCE_BARRIER b={0};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=original->original;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=target.state;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    if(target.state!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->lpVtbl->ResourceBarrier(list,1,&b);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=original->original;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=f->compare_readback[0];dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=f->compare_footprint;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    if(target.state!=D3D12_RESOURCE_STATE_COPY_SOURCE){b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=target.state;list->lpVtbl->ResourceBarrier(list,1,&b);}
    f->compare_original_state=target.state;return 1;
}

int rsx_native_frame_record_replay(rsx_native_frame* f,ID3D12GraphicsCommandList* list,ID3D12GraphicsCommandList9* bias)
{
    if(!f||f->state!=RSX_NATIVE_FRAME_SEALED||!list||f->compare_index>=f->resource_count||!f->compare_readback[0]||!f->coverage_verified)return 0;
    if(!frame_prepare_replay_upload(f)||!frame_heap(f))return frame_fail(f,"replay resources");
    for(UINT i=0;i<f->event_count;i++){
        frame_event*e=&f->events[i];int ok=0;
        switch(e->kind){
        case RSX_NATIVE_FRAME_DRAW: ok=frame_record_draw(f,list,bias,&e->u.draw,i);break;
        case RSX_NATIVE_FRAME_CLEAR_COLOR: ok=frame_record_clear_color(f,list,&e->u.clear_color,i*4u);break;
        case RSX_NATIVE_FRAME_CLEAR_DEPTH: ok=frame_record_clear_depth(f,list,&e->u.clear_depth,i);break;
        case RSX_NATIVE_FRAME_COPY:{frame_copy*c=&e->u.copy;frame_resource*s=&f->resources[c->src],*d=&f->resources[c->dst];
            frame_transition(list,s,D3D12_RESOURCE_STATE_COPY_SOURCE);
            if(d->mutable_resource)frame_transition(list,d,D3D12_RESOURCE_STATE_COPY_DEST);else if(d->replay_state!=D3D12_RESOURCE_STATE_COPY_DEST)break;
            if(c->full_resource)list->lpVtbl->CopyResource(list,d->clone,s->clone);
            else {D3D12_TEXTURE_COPY_LOCATION sl={0},dl={0};sl.pResource=s->clone;sl.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;sl.SubresourceIndex=c->src_sub;
                dl.pResource=d->clone;dl.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dl.SubresourceIndex=c->dst_sub;
                list->lpVtbl->CopyTextureRegion(list,&dl,c->x,c->y,c->z,&sl,c->has_box?&c->box:NULL);}
            frame_transition(list,s,c->src_state);frame_transition(list,d,c->dst_state);ok=1;break;}
        case RSX_NATIVE_FRAME_COMPUTE:ok=frame_record_compute(f,list,&e->u.compute);break;
        case RSX_NATIVE_FRAME_UPLOAD:ok=frame_record_upload(f,list,&e->u.upload);break;
        default:break;
        }
        if(!ok)return frame_fail(f,"event replay unsupported");
    }
    for(UINT i=0;i<f->resource_count;i++)if(!f->resources[i].mutable_resource)
        frame_transition(list,&f->resources[i],f->resources[i].capture_state);
    f->state=RSX_NATIVE_FRAME_REPLAY_RECORDED;return 1;
}

int rsx_native_frame_record_compare(rsx_native_frame* f,ID3D12GraphicsCommandList* list)
{
    if(!f||!list||f->state!=RSX_NATIVE_FRAME_REPLAY_RECORDED||!f->compare_readback[0])return 0;
    frame_resource* r=&f->resources[f->compare_index];
    if(r->mutable_resource)frame_transition(list,r,D3D12_RESOURCE_STATE_COPY_SOURCE);
    else if(r->replay_state!=D3D12_RESOURCE_STATE_COPY_SOURCE)return 0;
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=r->clone;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=f->compare_readback[1];dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=f->compare_footprint;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    if(r->mutable_resource)frame_transition(list,r,f->compare_original_state);
    f->state=RSX_NATIVE_FRAME_COMPARE_RECORDED;return 1;
}

int rsx_native_frame_get_compare_result(const rsx_native_frame* cf,uint64_t* bytes,uint64_t* mismatch)
{
    if(!cf||cf->state!=RSX_NATIVE_FRAME_COMPARE_RECORDED)return 0;
    rsx_native_frame* f=(rsx_native_frame*)cf;void* a=NULL;void* b=NULL;D3D12_RANGE read={0,(SIZE_T)f->compare_readback_size};
    if(FAILED(f->compare_readback[0]->lpVtbl->Map(f->compare_readback[0],0,&read,&a))||
       FAILED(f->compare_readback[1]->lpVtbl->Map(f->compare_readback[1],0,&read,&b))||!a||!b){
        if(a){D3D12_RANGE none={0,0};f->compare_readback[0]->lpVtbl->Unmap(f->compare_readback[0],0,&none);}return 0;}
    uint64_t different=0,compared=0;for(UINT y=0;y<f->compare_rows;y++){
        const uint8_t* pa=(const uint8_t*)a+f->compare_footprint.Offset+(size_t)y*f->compare_footprint.Footprint.RowPitch;
        const uint8_t* pb=(const uint8_t*)b+f->compare_footprint.Offset+(size_t)y*f->compare_footprint.Footprint.RowPitch;
        for(UINT64 x=0;x<f->compare_row_bytes;x++){different+=(pa[x]!=pb[x]);compared++;}}
    D3D12_RANGE none={0,0};f->compare_readback[0]->lpVtbl->Unmap(f->compare_readback[0],0,&none);
    f->compare_readback[1]->lpVtbl->Unmap(f->compare_readback[1],0,&none);
    f->compare_bytes=compared;f->mismatch_bytes=different;f->compare_done=1;if(bytes)*bytes=compared;if(mismatch)*mismatch=different;return 1;
}
void rsx_native_frame_get_stats(const rsx_native_frame* f,rsx_native_frame_stats* s)
{if(!s)return;memset(s,0,sizeof(*s));if(!f)return;s->state=f->state;s->event_count=f->event_count;s->resource_count=f->resource_count;s->unsupported_count=f->unsupported_count;s->owned_gpu_bytes=f->gpu_bytes;s->owned_upload_bytes=f->upload_used;s->compared_bytes=f->compare_bytes;s->mismatch_bytes=f->mismatch_bytes;s->color_parity_checked=f->compare_done;s->depth_parity_checked=0;s->caller_coverage_verified=f->coverage_verified;s->first_failure=f->failure;for(UINT i=0;i<f->event_count;i++)switch(f->events[i].kind){case RSX_NATIVE_FRAME_DRAW:s->draw_count++;break;case RSX_NATIVE_FRAME_CLEAR_COLOR:s->color_clear_count++;break;case RSX_NATIVE_FRAME_CLEAR_DEPTH:s->depth_clear_count++;break;case RSX_NATIVE_FRAME_COPY:s->copy_count++;break;case RSX_NATIVE_FRAME_COMPUTE:s->compute_count++;break;case RSX_NATIVE_FRAME_UPLOAD:s->upload_count++;break;}}

#else
typedef int rsx_native_frame_non_windows_translation_unit;
#endif
