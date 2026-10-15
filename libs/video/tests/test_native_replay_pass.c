/* Asset-free execution of the actual replay/payload modules and packet helpers.
 * Uses Windows WARP D3D12 only: no window, guest memory, audio or game.
 * Run test_native_replay_pass.py from a VS developer environment. */
#ifdef NDEBUG
#error This fixture requires active assertions
#endif
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <assert.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_native_replay_pass.h"

#define OK(expression) do { HRESULT h_ = (expression); if (FAILED(h_)) { \
    fprintf(stderr,"line %d HRESULT 0x%08lX\n",__LINE__,(unsigned long)h_); abort(); } } while (0)
#define RELEASE(object) do { if (object) { (object)->lpVtbl->Release(object); (object)=NULL; } } while (0)
#define SRV_TABLE_SIZE 16u
#include "native_draw_packet_actual.inc"

typedef struct gpu_context {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE event;
    UINT64 fence_value;
} gpu_context;
typedef struct scene {
    ID3D12Resource* color;
    ID3D12Resource* depth;
    ID3D12Resource* constants;
    ID3D12Resource* geometry;
    ID3D12Resource* original_readback;
    ID3D12DescriptorHeap* rtv_heap;
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12DescriptorHeap* srv_cpu_heap;
    ID3D12DescriptorHeap* sampler_cpu_heap;
    ID3D12DescriptorHeap* srv_heap;
    ID3D12DescriptorHeap* sampler_heap;
    ID3D12RootSignature* root;
    ID3D12PipelineState* pso;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv;
    D3D12_CPU_DESCRIPTOR_HANDLE srvs[16], samplers[16];
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, sampler_gpu;
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view;
} scene;

static void submit_wait(gpu_context* g) {
    OK(g->list->lpVtbl->Close(g->list));
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g->list};
    g->queue->lpVtbl->ExecuteCommandLists(g->queue,1,lists);
    ++g->fence_value;
    OK(g->queue->lpVtbl->Signal(g->queue,g->fence,g->fence_value));
    OK(g->fence->lpVtbl->SetEventOnCompletion(g->fence,g->fence_value,g->event));
    assert(WaitForSingleObject(g->event,30000) == WAIT_OBJECT_0);
    UINT64 completed = g->fence->lpVtbl->GetCompletedValue(g->fence);
    assert(completed != UINT64_MAX && completed >= g->fence_value);
}
static void reset_list(gpu_context* g) {
    UINT64 completed = g->fence->lpVtbl->GetCompletedValue(g->fence);
    assert(completed != UINT64_MAX && completed >= g->fence_value);
    OK(g->allocator->lpVtbl->Reset(g->allocator));
    OK(g->list->lpVtbl->Reset(g->list,g->allocator,NULL));
}
static ID3D12DescriptorHeap* heap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                  UINT count, int visible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {0};
    desc.Type = type; desc.NumDescriptors = count;
    desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12DescriptorHeap* result = NULL;
    OK(device->lpVtbl->CreateDescriptorHeap(device,&desc,&IID_ID3D12DescriptorHeap,(void**)&result));
    return result;
}
static ID3D12Resource* buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 size) {
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = type;
    D3D12_RESOURCE_DESC desc = {0}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size; desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = NULL;
    OK(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&desc,
       type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,
       NULL,&IID_ID3D12Resource,(void**)&resource));
    return resource;
}
static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {0}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
    list->lpVtbl->ResourceBarrier(list,1,&barrier);
}
static void create_scene(gpu_context* g, scene* s, int depth_enabled, int pull_enabled) {
    memset(s,0,sizeof(*s));
    ID3D12Device* device = g->device;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {0}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 32; desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    OK(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&s->color));
    s->rtv_heap = heap(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,0);
    s->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->rtv_heap,&s->rtv);
    device->lpVtbl->CreateRenderTargetView(device,s->color,NULL,s->rtv);
    if (depth_enabled) {
        desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        OK(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,NULL,&IID_ID3D12Resource,(void**)&s->depth));
        s->dsv_heap = heap(device,D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,0);
        s->dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->dsv_heap,&s->dsv);
        s->depth_view.Format = desc.Format; s->depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->lpVtbl->CreateDepthStencilView(device,s->depth,&s->depth_view,s->dsv);
    }
    s->srv_cpu_heap = heap(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,32,0);
    s->sampler_cpu_heap = heap(device,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,32,0);
    s->srv_heap = heap(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,16,1);
    s->sampler_heap = heap(device,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,16,1);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_base, sampler_base, srv_dst, sampler_dst;
    s->srv_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->srv_cpu_heap,&srv_base);
    s->sampler_cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->sampler_cpu_heap,&sampler_base);
    s->srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->srv_heap,&srv_dst);
    s->sampler_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->sampler_heap,&sampler_dst);
    s->srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s->srv_heap,&s->srv_gpu);
    s->sampler_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s->sampler_heap,&s->sampler_gpu);
    UINT srv_step = device->lpVtbl->GetDescriptorHandleIncrementSize(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT sampler_step = device->lpVtbl->GetDescriptorHandleIncrementSize(device,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0}; srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_SAMPLER_DESC sampler = {0}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxAnisotropy = 1; sampler.MaxLOD = D3D12_FLOAT32_MAX;
    for (UINT i = 0; i < 16; ++i) {
        /* Deliberately scattered CPU sources; neither table is contiguous. */
        s->srvs[i].ptr = srv_base.ptr+(SIZE_T)((15-i)*2)*srv_step;
        s->samplers[i].ptr = sampler_base.ptr+(SIZE_T)(((i*7)%16)*2)*sampler_step;
        device->lpVtbl->CreateShaderResourceView(device,NULL,&srv,s->srvs[i]);
        device->lpVtbl->CreateSampler(device,&sampler,s->samplers[i]);
        D3D12_CPU_DESCRIPTOR_HANDLE sd = {srv_dst.ptr+(SIZE_T)i*srv_step};
        D3D12_CPU_DESCRIPTOR_HANDLE md = {sampler_dst.ptr+(SIZE_T)i*sampler_step};
        device->lpVtbl->CopyDescriptorsSimple(device,1,sd,s->srvs[i],D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->lpVtbl->CopyDescriptorsSimple(device,1,md,s->samplers[i],D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }
    D3D12_DESCRIPTOR_RANGE ranges[3] = {0};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 16;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER; ranges[1].NumDescriptors = 16;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[2].NumDescriptors = 4; ranges[2].BaseShaderRegister = 16;
    D3D12_ROOT_PARAMETER parameters[6] = {0};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    for (UINT i = 0; i < 2; ++i) {
        parameters[i+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i+1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[i+1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i+1].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[3].DescriptorTable.NumDescriptorRanges = 1; parameters[3].DescriptorTable.pDescriptorRanges = &ranges[2];
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; parameters[4].Descriptor.ShaderRegister = 1;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX; parameters[5].Descriptor.ShaderRegister = 20;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {0}; root_desc.NumParameters = 6; root_desc.pParameters = parameters;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* signature = NULL; ID3DBlob* vs = NULL; ID3DBlob* ps = NULL;
    OK(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,NULL));
    OK(device->lpVtbl->CreateRootSignature(device,0,signature->lpVtbl->GetBufferPointer(signature),
        signature->lpVtbl->GetBufferSize(signature),&IID_ID3D12RootSignature,(void**)&s->root));
    const char* vertex_source = "cbuffer C:register(b0){float4 color;} struct O{float4 p:SV_Position;float4 c:COLOR0;}; O main(uint id:SV_VertexID){O o;float2 p=float2((id<<1)&2,id&2);o.p=float4(p*2-1,0.5,1);o.c=color;return o;}";
    const char* pull_source = "cbuffer C:register(b0){float4 color;} StructuredBuffer<float2> positions:register(t20); struct O{float4 p:SV_Position;float4 c:COLOR0;}; O main(uint id:SV_VertexID){O o;o.p=float4(positions[id],0.5,1);o.c=color;return o;}";
    const char* pixel_source = "float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target{return c;}";
    const char* selected_vertex = pull_enabled ? pull_source : vertex_source;
    OK(D3DCompile(selected_vertex,strlen(selected_vertex),NULL,NULL,NULL,"main","vs_5_0",0,0,&vs,NULL));
    OK(D3DCompile(pixel_source,strlen(pixel_source),NULL,NULL,NULL,"main","ps_5_0",0,0,&ps,NULL));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {0}; pso.pRootSignature = s->root;
    pso.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); pso.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    pso.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); pso.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    pso.SampleMask = UINT_MAX; pso.SampleDesc.Count = 1; pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    D3D12_RENDER_TARGET_BLEND_DESC* blend = &pso.BlendState.RenderTarget[0];
    blend->SrcBlend = blend->SrcBlendAlpha = D3D12_BLEND_ONE; blend->DestBlend = blend->DestBlendAlpha = D3D12_BLEND_ZERO;
    blend->BlendOp = blend->BlendOpAlpha = D3D12_BLEND_OP_ADD; blend->LogicOp = D3D12_LOGIC_OP_NOOP;
    blend->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = depth_enabled; pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pso.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    pso.DepthStencilState.FrontFace.StencilFailOp = pso.DepthStencilState.FrontFace.StencilDepthFailOp =
        pso.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    pso.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
    if (depth_enabled) pso.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    OK(device->lpVtbl->CreateGraphicsPipelineState(device,&pso,&IID_ID3D12PipelineState,(void**)&s->pso));
    RELEASE(signature); RELEASE(vs); RELEASE(ps);
    s->constants = buffer(device,D3D12_HEAP_TYPE_UPLOAD,512);
    if (pull_enabled) s->geometry = buffer(device,D3D12_HEAP_TYPE_UPLOAD,256);
    s->original_readback = buffer(device,D3D12_HEAP_TYPE_READBACK,8192);
}
static void destroy_scene(scene* s) {
    RELEASE(s->pso); RELEASE(s->root); RELEASE(s->constants); RELEASE(s->geometry); RELEASE(s->original_readback);
    RELEASE(s->color); RELEASE(s->depth); RELEASE(s->rtv_heap); RELEASE(s->dsv_heap);
    RELEASE(s->srv_cpu_heap); RELEASE(s->sampler_cpu_heap); RELEASE(s->srv_heap); RELEASE(s->sampler_heap);
}
static void fill_packet(gpu_context* g, scene* s, rsx_native_draw_packet* p, UINT draw_index, int clear_depth) {
    memset(p,0,sizeof(*p)); p->list = g->list; p->pipeline_state = s->pso; p->root_signature = s->root;
    p->descriptor_heaps[0] = s->srv_heap; p->descriptor_heaps[1] = s->sampler_heap;
    p->srv_table = s->srv_gpu; p->sampler_table = s->sampler_gpu;
    p->render_targets[0] = s->rtv; p->render_target_count = 1;
    p->has_depth_stencil = s->depth != NULL; p->depth_stencil = s->dsv; p->clear_depth_stencil = clear_depth;
    p->viewport.Width = p->viewport.Height = 32; p->viewport.MaxDepth = 1;
    p->scissor.left = draw_index ? 16 : 4; p->scissor.right = draw_index ? 28 : 16;
    p->scissor.top = 4; p->scissor.bottom = 28;
    p->topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; p->vertex_or_index_count = 3; p->instance_count = 1;
    p->vertex_constant_buffer = s->constants->lpVtbl->GetGPUVirtualAddress(s->constants)+(UINT64)draw_index*256;
    if (s->geometry) {
        p->has_pull_shader_resource = 1;
        p->pull_shader_resource = s->geometry->lpVtbl->GetGPUVirtualAddress(s->geometry);
        assert(!p->has_vertex_buffer);
    }
}
static void original_readback(gpu_context* g, scene* s) {
    transition(g->list,s->color,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = s->color; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = s->original_readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = dst.PlacedFootprint.Footprint.Height = 32;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
    g->list->lpVtbl->CopyTextureRegion(g->list,&dst,0,0,0,&src,NULL);
    transition(g->list,s->color,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
}
static void verify_original(scene* s, UINT draws) {
    D3D12_RANGE range = {0,8192}; const unsigned char* bytes = NULL;
    OK(s->original_readback->lpVtbl->Map(s->original_readback,0,&range,(void**)&bytes));
    for (UINT y = 0; y < 32; ++y) for (UINT x = 0; x < 32; ++x) {
        unsigned selected = 2; /* Uncovered pixels retain the blue initial clear. */
        if (y >= 4 && y < 28) {
            if (x >= 4 && x < 16) selected = 0;
            else if (draws == 2 && x >= 16 && x < 28) selected = 1;
        }
        const unsigned char* pixel = bytes+(SIZE_T)y*256+x*4;
        for (unsigned c = 0; c < 3; ++c) assert(pixel[c] == (c == selected ? 255 : 0));
        assert(pixel[3] == 255);
    }
    D3D12_RANGE no_write = {0,0}; s->original_readback->lpVtbl->Unmap(s->original_readback,0,&no_write);
}
static void exercise(gpu_context* g, int depth_enabled, UINT draws, int rejection, int pull_enabled) {
    scene s; create_scene(g,&s,depth_enabled,pull_enabled);
    const FLOAT blue[4] = {0,0,1,1};
    g->list->lpVtbl->ClearRenderTargetView(g->list,s.rtv,blue,0,NULL);
    int implicit_clear = depth_enabled && draws == 1;
    if (s.depth) g->list->lpVtbl->ClearDepthStencilView(g->list,s.dsv,
        D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,implicit_clear ? 0.0f : 1.0f,0,0,NULL);
    rsx_native_replay_pass* pass = rsx_native_replay_pass_create(g->device,2,4096);
    assert(pass);
    ID3D12Resource* colors[4] = {s.color,NULL,NULL,NULL};
    assert(rsx_native_replay_pass_begin(pass,g->list,colors,1,s.depth,s.depth ? &s.depth_view : NULL));
    unsigned char cpu_constants[2][256] = {{0}};
    const FLOAT red[4] = {1,0,0,1}, green[4] = {0,1,0,1};
    memcpy(cpu_constants[0],red,sizeof(red)); memcpy(cpu_constants[1],green,sizeof(green));
    void* mapped = NULL; D3D12_RANGE no_read = {0,0};
    OK(s.constants->lpVtbl->Map(s.constants,0,&no_read,&mapped));
    memcpy(mapped,cpu_constants,sizeof(cpu_constants));
    D3D12_RANGE written = {0,sizeof(cpu_constants)}; s.constants->lpVtbl->Unmap(s.constants,0,&written);
    FLOAT positions[2][6] = {{-1,-1,3,-1,-1,3},{-1,-1,3,-1,-1,3}};
    if (s.geometry) {
        OK(s.geometry->lpVtbl->Map(s.geometry,0,&no_read,&mapped));
        memcpy(mapped,positions[0],sizeof(positions[0]));
        D3D12_RANGE geometry_written = {0,sizeof(positions[0])};
        s.geometry->lpVtbl->Unmap(s.geometry,0,&geometry_written);
    }
    ID3D12Resource* sampled[32] = {0};
    for (UINT i = 0; i < draws; ++i) {
        rsx_native_draw_packet packet; fill_packet(g,&s,&packet,i,implicit_clear && i == 0);
        rsx_native_draw_payload_input input = {0}; input.draw_ordinal = i+101;
        input.vertex_constants.data = cpu_constants[i]; input.vertex_constants.size = 256;
        if (s.geometry && rejection != 3) { input.vertex.data = positions[i]; input.vertex.size = sizeof(positions[i]); }
        if (rejection) {
            if (rejection == 1) { packet.transition_count = 1; packet.transition_resources[0] = s.color; }
            else if (rejection == 2) packet.render_target_count = 2;
            else assert(rejection == 3 && packet.has_pull_shader_resource && !input.vertex.size);
            assert(!rsx_native_replay_pass_capture(pass,&packet,&input,s.srvs,s.samplers,NULL,sampled));
            assert(rsx_native_replay_pass_draw_count(pass) == 0);
            assert(rsx_native_replay_pass_error(pass)[0] != '\0');
            break;
        }
        assert(rsx_native_replay_pass_capture(pass,&packet,&input,s.srvs,s.samplers,NULL,sampled));
        /* Mutation cannot affect the separately uploaded live CB or owned replay bytes. */
        memset(cpu_constants[i],0xFF,sizeof(cpu_constants[i])); memset(&input,0,sizeof(input));
        for (unsigned j = 0; j < 6; ++j) positions[i][j] = 1000.0f;
        rsx_native_draw_packet_begin(&packet); rsx_native_draw_packet_targets(&packet);
        rsx_native_draw_packet_bind(&packet); rsx_native_draw_packet_draw(&packet); rsx_native_draw_packet_end(&packet);
        memset(&packet,0,sizeof(packet)); /* Captured packet fields must be copied too. */
    }
    if (!rejection) original_readback(g,&s);
    submit_wait(g); /* Initial clone and original draws have completed. */
    if (!rejection) {
        verify_original(&s,draws);
        if (s.geometry) {
            /* The live GPU VA remains valid but now describes offscreen
             * geometry. Correct replay must bind its own captured payload VA. */
            FLOAT offscreen[6] = {1000,1000,1000,1000,1000,1000};
            OK(s.geometry->lpVtbl->Map(s.geometry,0,&no_read,&mapped));
            memcpy(mapped,offscreen,sizeof(offscreen));
            D3D12_RANGE geometry_written = {0,sizeof(offscreen)};
            s.geometry->lpVtbl->Unmap(s.geometry,0,&geometry_written);
        }
        assert(rsx_native_replay_pass_draw_count(pass) == draws);
        reset_list(g);
        assert(rsx_native_replay_pass_record(pass,g->list,NULL));
        assert(rsx_native_replay_pass_record_compare(pass,g->list));
        submit_wait(g);
        rsx_native_replay_compare_result result;
        assert(rsx_native_replay_pass_compare(pass,&result));
        assert(result.compared_targets == 1 && result.compared_bytes == 32u*32u*4u && result.mismatch_bytes == 0);
        printf("WARP replay depth=%d draws=%u compared=%llu mismatches=%llu CPU-source-mutation passed\n",
            depth_enabled,draws,(unsigned long long)result.compared_bytes,(unsigned long long)result.mismatch_bytes);
        if (pull_enabled) printf("WARP pull-only draws=%u owned-SRV-VA passed after original-GPU-buffer overwrite\n",draws);
    } else printf("WARP replay rejection=%d passed after completion\n",rejection);
    /* Never destroy resources referenced by submitted or pending GPU commands. */
    UINT64 completed = g->fence->lpVtbl->GetCompletedValue(g->fence);
    assert(completed != UINT64_MAX && completed >= g->fence_value);
    rsx_native_replay_pass_destroy(pass); destroy_scene(&s); reset_list(g);
}
int main(void) {
    ID3D12Debug* debug = NULL;
    if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug,(void**)&debug))) {
        debug->lpVtbl->EnableDebugLayer(debug); RELEASE(debug);
    } else puts("D3D12 debug layer unavailable; actual WARP execution remains required");
    IDXGIFactory4* factory = NULL; IDXGIAdapter* warp = NULL;
    gpu_context g; memset(&g,0,sizeof(g));
    OK(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
    OK(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
    OK(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&g.device));
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    OK(g.device->lpVtbl->CreateCommandQueue(g.device,&queue_desc,&IID_ID3D12CommandQueue,(void**)&g.queue));
    OK(g.device->lpVtbl->CreateCommandAllocator(g.device,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&g.allocator));
    OK(g.device->lpVtbl->CreateCommandList(g.device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,g.allocator,NULL,
        &IID_ID3D12GraphicsCommandList,(void**)&g.list));
    OK(g.device->lpVtbl->CreateFence(g.device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&g.fence));
    g.event = CreateEventA(NULL,FALSE,FALSE,NULL); assert(g.event);
    for (int depth = 0; depth < 2; ++depth) for (UINT draws = 1; draws <= 2; ++draws) exercise(&g,depth,draws,0,0);
    exercise(&g,0,1,1,0); exercise(&g,0,1,2,0);
    exercise(&g,0,1,0,1); exercise(&g,0,2,0,1); exercise(&g,0,1,3,1);
    /* Last reset left an empty list recording; close it without submitting. */
    OK(g.list->lpVtbl->Close(g.list));
    ID3D12InfoQueue* info = NULL;
    if (SUCCEEDED(g.device->lpVtbl->QueryInterface(g.device,&IID_ID3D12InfoQueue,(void**)&info))) {
        UINT64 count = info->lpVtbl->GetNumStoredMessagesAllowedByRetrievalFilter(info);
        unsigned errors = 0;
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0; OK(info->lpVtbl->GetMessage(info,i,NULL,&size));
            D3D12_MESSAGE* message = (D3D12_MESSAGE*)malloc(size); assert(message);
            OK(info->lpVtbl->GetMessage(info,i,message,&size));
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                fprintf(stderr,"D3D12 validation: %s\n",message->pDescription); ++errors;
            }
            free(message);
        }
        assert(errors == 0); RELEASE(info);
        puts("D3D12 debug-layer error/corruption messages=0");
    }
    CloseHandle(g.event); RELEASE(g.list); RELEASE(g.allocator); RELEASE(g.queue); RELEASE(g.fence);
    RELEASE(g.device); RELEASE(warp); RELEASE(factory);
    puts("actual-source native replay WARP integration passed; descriptor contents and depth bytes are not directly compared");
    return 0;
}
