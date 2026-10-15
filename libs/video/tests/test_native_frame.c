/* Actual ordered-frame module on WARP. Reuse asset-free GPU setup helpers,
 * not a copy of the frame algorithm. The prior fixture's main is never run. */
#define main unused_prior_replay_fixture_main
#include "test_native_replay_pass.c"
#undef main
#include "rsx_native_frame.h"
#include "rsx_color_clear.h"

static ID3D12Resource* frame_texture(ID3D12Device* device) {
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {0}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 32; desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* result = NULL;
    OK(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&result));
    return result;
}
static rsx_native_frame_color_target color_target(ID3D12Resource* resource) {
    rsx_native_frame_color_target target; memset(&target,0,sizeof(target));
    target.ref.resource = resource; target.ref.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    target.ref.mutable_resource = 1; target.view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    return target;
}
static D3D12_SHADER_RESOURCE_VIEW_DESC rgba_srv(void) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {0}; view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; return view;
}
static void make_compute(ID3D12Device* device, ID3D12RootSignature** root, ID3D12PipelineState** pso) {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {0}; ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    D3D12_ROOT_PARAMETER parameters[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        ranges[i].NumDescriptors = 1; parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1; parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC desc = {0}; desc.NumParameters = 2; desc.pParameters = parameters;
    ID3DBlob* signature = NULL; ID3DBlob* shader = NULL;
    OK(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,NULL));
    OK(device->lpVtbl->CreateRootSignature(device,0,signature->lpVtbl->GetBufferPointer(signature),
        signature->lpVtbl->GetBufferSize(signature),&IID_ID3D12RootSignature,(void**)root));
    const char* source = "Texture2D<float4> s:register(t0);RWTexture2D<float4> d:register(u0);[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){d[id.xy]=s.Load(int3(id.xy,0)).bgra;}";
    OK(D3DCompile(source,strlen(source),NULL,NULL,NULL,"main","cs_5_0",0,0,&shader,NULL));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {0}; pipeline.pRootSignature = *root;
    pipeline.CS.pShaderBytecode = shader->lpVtbl->GetBufferPointer(shader);
    pipeline.CS.BytecodeLength = shader->lpVtbl->GetBufferSize(shader);
    OK(device->lpVtbl->CreateComputePipelineState(device,&pipeline,&IID_ID3D12PipelineState,(void**)pso));
    RELEASE(signature); RELEASE(shader);
}
static ID3D12PipelineState* make_pixel_load(ID3D12Device* device, ID3D12RootSignature* root) {
    const char* v = "float4 main(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*2-1,.5,1);}";
    const char* p = "Texture2D<float4> s:register(t0);float4 main(float4 p:SV_Position):SV_Target{return s.Load(int3(p.xy,0));}";
    ID3DBlob* vs = NULL; ID3DBlob* ps = NULL;
    OK(D3DCompile(v,strlen(v),NULL,NULL,NULL,"main","vs_5_0",0,0,&vs,NULL));
    OK(D3DCompile(p,strlen(p),NULL,NULL,NULL,"main","ps_5_0",0,0,&ps,NULL));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {0}; desc.pRootSignature = root;
    desc.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); desc.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    desc.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); desc.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    desc.SampleMask = UINT_MAX; desc.SampleDesc.Count = 1; desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOp = desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.FrontFace.StencilFailOp = desc.DepthStencilState.FrontFace.StencilDepthFailOp =
        desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
    ID3D12PipelineState* result = NULL;
    OK(device->lpVtbl->CreateGraphicsPipelineState(device,&desc,&IID_ID3D12PipelineState,(void**)&result));
    RELEASE(vs); RELEASE(ps); return result;
}
static void capture_draw(rsx_native_frame* frame, gpu_context* g, scene* s,
    ID3D12GraphicsCommandList* capture_list, rsx_native_draw_packet* packet,
    const rsx_native_frame_color_target* target, const FLOAT constants[4],
    rsx_native_frame_binding bindings[16]) {
    rsx_native_draw_payload_store payload;
    assert(rsx_native_draw_payload_init(&payload,1024,1));
    unsigned char bytes[256] = {0}; memcpy(bytes,constants,4*sizeof(FLOAT));
    rsx_native_draw_payload_input input = {0}; input.vertex_constants.data = bytes; input.vertex_constants.size = sizeof(bytes);
    assert(rsx_native_draw_payload_append(&payload,&input,NULL) && rsx_native_draw_payload_seal(&payload));
    ID3D12GraphicsCommandList* saved_list = packet->list; packet->list = capture_list;
    rsx_native_frame_binding vtex[16]; memset(vtex,0,sizeof(vtex));
    assert(rsx_native_frame_capture_draw(frame,capture_list,packet,&payload,target,1,NULL,bindings,s->samplers,vtex));
    packet->list = saved_list; memset(bytes,0xCC,sizeof(bytes)); rsx_native_draw_payload_destroy(&payload);
    rsx_native_draw_packet_begin(packet); rsx_native_draw_packet_targets(packet);
    rsx_native_draw_packet_bind(packet); rsx_native_draw_packet_draw(packet); rsx_native_draw_packet_end(packet);
    (void)g;
}
static void check_chain_pixels(scene* s) {
    D3D12_RANGE range = {0,8192}; const unsigned char* bytes = NULL;
    OK(s->original_readback->lpVtbl->Map(s->original_readback,0,&range,(void**)&bytes));
    for (UINT y = 0; y < 32; ++y) for (UINT x = 0; x < 32; ++x) {
        unsigned char wanted[4] = {0,0,0,255};
        if (x >= 8 && x < 16) { wanted[1] = 255; }
        else if (x >= 16 && x < 24) {
            wanted[0] = 191; wanted[1] = 128;
            if (y >= 8 && y < 24) { wanted[2] = 255; wanted[3] = 255; }
            else { wanted[2] = 64; wanted[3] = 64; }
        }
        assert(!memcmp(bytes+(SIZE_T)y*256+x*4,wanted,4));
    }
    D3D12_RANGE no_write = {0,0}; s->original_readback->lpVtbl->Unmap(s->original_readback,0,&no_write);
}
static void exercise_frame(gpu_context* g) {
    scene s; create_scene(g,&s,0,0,0);
    ID3D12Resource* surfaces[3] = {frame_texture(g->device),frame_texture(g->device),frame_texture(g->device)};
    ID3D12DescriptorHeap* rtvs = heap(g->device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV,3,0);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[3]; rtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvs,&rtv[0]);
    UINT rtv_step = g->device->lpVtbl->GetDescriptorHandleIncrementSize(g->device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 3; ++i) { rtv[i].ptr = rtv[0].ptr+(SIZE_T)i*rtv_step; g->device->lpVtbl->CreateRenderTargetView(g->device,surfaces[i],NULL,rtv[i]); }
    const FLOAT seed[4] = {.25f,.5f,.75f,.25f}, poison[4] = {1,0,1,1}, black[4] = {0,0,0,1};
    const FLOAT blue[4] = {0,0,1,1}, red[4] = {1,0,0,1}, green[4] = {0,1,0,1};
    g->list->lpVtbl->ClearRenderTargetView(g->list,rtv[0],seed,0,NULL);
    for (UINT i = 1; i < 3; ++i) g->list->lpVtbl->ClearRenderTargetView(g->list,rtv[i],poison,0,NULL);
    g->list->lpVtbl->ClearRenderTargetView(g->list,s.rtv,poison,0,NULL);
    rsx_native_frame* frame = rsx_native_frame_create(g->device,16,4096); assert(frame);
    rsx_native_frame_color_target targets[4] = {color_target(surfaces[0]),color_target(surfaces[1]),color_target(surfaces[2]),color_target(s.color)};
    rsx_color_clear clear; memset(&clear,0,sizeof(clear));
    D3D12_RECT full = rsx_clear_rect(32,32,0,0,0);
    D3D12_RECT middle = rsx_clear_rect(32,32,(16u<<16)|8u,(16u<<16)|8u,1);
    assert(rsx_native_frame_capture_clear_color(frame,g->list,&targets[0],DXGI_FORMAT_R8G8B8A8_UNORM,32,32,&middle,9,red));
    OK(rsx_color_clear_record(&clear,g->device,g->list,rtv[0],DXGI_FORMAT_R8G8B8A8_UNORM,32,32,&middle,9,red));
    for (UINT i = 1; i < 4; ++i) {
        const FLOAT* value = i == 3 ? blue : black;
        assert(rsx_native_frame_capture_clear_color(frame,g->list,&targets[i],DXGI_FORMAT_R8G8B8A8_UNORM,32,32,&full,15,value));
        g->list->lpVtbl->ClearRenderTargetView(g->list,i == 3 ? s.rtv : rtv[i],value,0,NULL);
    }
    /* All resources are now registered. Every subsequent capture must retain
     * those SAME clones, never copy a later original pass result as a shortcut. */
    ID3D12GraphicsCommandListVtbl capture_vtable; memset(&capture_vtable,0,sizeof(capture_vtable));
    capture_vtable.ResourceBarrier = proxy_barrier; capture_vtable.CopyResource = proxy_copy;
    ID3D12GraphicsCommandList capture_list = {&capture_vtable}; proxy_target = g->list; proxy_copies = proxy_barriers = 0;
    void* mapped = NULL; D3D12_RANGE no_read = {0,0}; OK(s.constants->lpVtbl->Map(s.constants,0,&no_read,&mapped));
    memset(mapped,0,512); memcpy(mapped,green,sizeof(green)); memcpy((unsigned char*)mapped+256,green,sizeof(green));
    D3D12_RANGE written = {0,512}; s.constants->lpVtbl->Unmap(s.constants,0,&written);
    rsx_native_frame_binding bindings[16]; memset(bindings,0,sizeof(bindings));
    for (UINT i = 0; i < 16; ++i) bindings[i].immutable_descriptor = s.srvs[i];
    rsx_native_draw_packet packet; fill_packet(g,&s,&packet,0,0);
    packet.render_targets[0] = rtv[0]; packet.scissor = (D3D12_RECT){0,0,8,32};
    capture_draw(frame,g,&s,&capture_list,&packet,&targets[0],green,bindings);
    D3D12_BOX box = {0,0,0,16,32,1};
    assert(rsx_native_frame_capture_copy(frame,&capture_list,targets[0].ref,0,&box,targets[1].ref,0,8,0,0));
    transition(g->list,surfaces[0],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(g->list,surfaces[1],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0}; src.pResource = surfaces[0]; dst.pResource = surfaces[1];
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g->list->lpVtbl->CopyTextureRegion(g->list,&dst,8,0,0,&src,&box);
    transition(g->list,surfaces[0],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(g->list,surfaces[1],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12RootSignature* compute_root = NULL; ID3D12PipelineState* compute_pso = NULL;
    make_compute(g->device,&compute_root,&compute_pso);
    ID3D12DescriptorHeap* compute_heap = heap(g->device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,1);
    D3D12_CPU_DESCRIPTOR_HANDLE ch[2]; D3D12_GPU_DESCRIPTOR_HANDLE gh[2];
    compute_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(compute_heap,&ch[0]);
    compute_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(compute_heap,&gh[0]);
    UINT step = g->device->lpVtbl->GetDescriptorHandleIncrementSize(g->device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ch[1].ptr = ch[0].ptr+step; gh[1].ptr = gh[0].ptr+step;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = rgba_srv(); D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {0};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g->device->lpVtbl->CreateShaderResourceView(g->device,surfaces[1],&srv,ch[0]);
    g->device->lpVtbl->CreateUnorderedAccessView(g->device,surfaces[2],NULL,&uav,ch[1]);
    rsx_native_frame_compute_desc compute; memset(&compute,0,sizeof(compute));
    compute.pipeline_state = compute_pso; compute.root_signature = compute_root; compute.table_count = 2;
    compute.groups_x = compute.groups_y = 4; compute.groups_z = 1;
    for (UINT i = 0; i < 2; ++i) {
        compute.tables[i].root_index = i; compute.tables[i].binding_count = 1;
        rsx_native_frame_binding* b = &compute.tables[i].bindings[0]; b->enabled = 1;
        b->kind = i ? RSX_NATIVE_FRAME_BINDING_UAV : RSX_NATIVE_FRAME_BINDING_SRV;
        b->ref = targets[i+1].ref;
        b->ref.state = i ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (i) b->view.uav = uav; else b->view.srv = srv;
    }
    transition(g->list,surfaces[1],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(g->list,surfaces[2],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    assert(rsx_native_frame_capture_compute(frame,&capture_list,&compute));
    g->list->lpVtbl->SetDescriptorHeaps(g->list,1,&compute_heap); g->list->lpVtbl->SetPipelineState(g->list,compute_pso);
    g->list->lpVtbl->SetComputeRootSignature(g->list,compute_root);
    g->list->lpVtbl->SetComputeRootDescriptorTable(g->list,0,gh[0]);
    g->list->lpVtbl->SetComputeRootDescriptorTable(g->list,1,gh[1]); g->list->lpVtbl->Dispatch(g->list,4,4,1);
    D3D12_RESOURCE_BARRIER ub = {0}; ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; ub.UAV.pResource = surfaces[2];
    g->list->lpVtbl->ResourceBarrier(g->list,1,&ub);
    transition(g->list,surfaces[1],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(g->list,surfaces[2],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12PipelineState* final_pso = make_pixel_load(g->device,s.root);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv; s.srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s.srv_heap,&cpu_srv);
    g->device->lpVtbl->CreateShaderResourceView(g->device,surfaces[2],&srv,cpu_srv);
    bindings[0].enabled = 1; bindings[0].kind = RSX_NATIVE_FRAME_BINDING_SRV;
    bindings[0].ref = targets[2].ref; bindings[0].ref.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bindings[0].view.srv = srv;
    fill_packet(g,&s,&packet,1,0); packet.pipeline_state = final_pso; packet.scissor = full;
    capture_draw(frame,g,&s,&capture_list,&packet,&targets[3],green,bindings);
    assert(proxy_copies == 0 && proxy_barriers == 0);
    assert(rsx_native_frame_set_compare_target(frame,targets[3].ref,&targets[3].view));
    /* Coverage is a separate caller assertion, never implied by one recorded
     * operation or by a comparison target. Reference pixels must be frozen
     * after the final original write, before ANY original texture is reused. */
    assert(!rsx_native_frame_capture_compare_reference(frame,g->list,targets[3].ref,&targets[3].view));
    assert(rsx_native_frame_set_coverage_verified(frame));
    assert(rsx_native_frame_capture_compare_reference(frame,g->list,targets[3].ref,&targets[3].view));
    assert(rsx_native_frame_seal(frame));
    rsx_native_frame_stats stats; rsx_native_frame_get_stats(frame,&stats);
    assert(stats.state == RSX_NATIVE_FRAME_SEALED && stats.resource_count == 4 && stats.event_count == 8);
    assert(stats.draw_count == 2 && stats.color_clear_count == 4 && stats.copy_count == 1 && stats.compute_count == 1);
    assert(!stats.unsupported_count && !stats.color_parity_checked && !stats.depth_parity_checked);
    assert(stats.caller_coverage_verified);
    original_readback(g,&s);
    submit_wait(g); check_chain_pixels(&s); reset_list(g);
    g->list->lpVtbl->ClearRenderTargetView(g->list,rtv[0],poison,0,NULL);
    g->list->lpVtbl->ClearRenderTargetView(g->list,rtv[1],poison,0,NULL);
    transition(g->list,surfaces[2],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    g->list->lpVtbl->ClearRenderTargetView(g->list,rtv[2],poison,0,NULL);
    transition(g->list,surfaces[2],D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g->list->lpVtbl->ClearRenderTargetView(g->list,s.rtv,poison,0,NULL);
    OK(s.constants->lpVtbl->Map(s.constants,0,&no_read,&mapped)); memset(mapped,0,512);
    s.constants->lpVtbl->Unmap(s.constants,0,&written);
    /* The source CPU descriptors are also reusable after capture. These
     * disabled slots are not sampled by this shader, so this exercises the
     * ownership/lifetime path without claiming descriptor-content parity. */
    for (UINT i = 0; i < 16; ++i)
        g->device->lpVtbl->CreateShaderResourceView(g->device,surfaces[2],&srv,s.srvs[i]);
    assert(rsx_native_frame_record_replay(frame,g->list,NULL));
    assert(rsx_native_frame_record_compare(frame,g->list)); submit_wait(g);
    uint64_t compared = 0, mismatches = UINT64_MAX;
    assert(rsx_native_frame_get_compare_result(frame,&compared,&mismatches));
    assert(compared == 4096 && mismatches == 0);
    rsx_native_frame_get_stats(frame,&stats);
    assert(stats.color_parity_checked && !stats.depth_parity_checked && stats.compared_bytes == 4096 && !stats.mismatch_bytes);
    rsx_native_frame_destroy(frame); rsx_color_clear_release(&clear);
    for (UINT i = 0; i < 3; ++i) RELEASE(surfaces[i]);
    RELEASE(rtvs); RELEASE(compute_root); RELEASE(compute_pso); RELEASE(compute_heap); RELEASE(final_pso);
    destroy_scene(&s); reset_list(g);
    puts("actual-source ordered native frame WARP dependency chain passed; masked channels, shifted copy, compute, sampled replay outputs, no late snapshots, all originals overwritten; 4096 bytes mismatch=0");
}
static void unsupported_guards(gpu_context* g) {
    rsx_native_frame* frame = rsx_native_frame_create(g->device,4,1024); assert(frame);
    uint64_t bytes = UINT64_MAX, mismatch = UINT64_MAX;
    assert(!rsx_native_frame_get_compare_result(frame,&bytes,&mismatch));
    rsx_native_frame_stats open_stats; rsx_native_frame_get_stats(frame,&open_stats);
    assert(!open_stats.caller_coverage_verified && !open_stats.color_parity_checked && !open_stats.depth_parity_checked);
    rsx_native_frame_mark_unsupported(frame,"required operation omitted");
    assert(!rsx_native_frame_seal(frame));
    assert(!rsx_native_frame_set_coverage_verified(frame));
    assert(!rsx_native_frame_record_replay(frame,g->list,NULL));
    assert(!rsx_native_frame_record_compare(frame,g->list));
    assert(!rsx_native_frame_get_compare_result(frame,&bytes,&mismatch));
    rsx_native_frame_stats stats; rsx_native_frame_get_stats(frame,&stats);
    assert(stats.state == RSX_NATIVE_FRAME_FAILED && stats.unsupported_count != 0);
    assert(!stats.color_parity_checked && !stats.depth_parity_checked && !stats.compared_bytes);
    assert(stats.first_failure && strstr(stats.first_failure,"required operation omitted"));
    rsx_native_frame_destroy(frame);
    puts("native frame unsupported/incomplete rejected; no false parity flags");
}
int main(void) {
    ID3D12Debug* debug = NULL;
    if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug,(void**)&debug))) { debug->lpVtbl->EnableDebugLayer(debug); RELEASE(debug); }
    IDXGIFactory4* factory = NULL; IDXGIAdapter* warp = NULL; gpu_context g; memset(&g,0,sizeof(g));
    OK(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
    OK(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
    OK(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&g.device));
    D3D12_COMMAND_QUEUE_DESC q = {0}; OK(g.device->lpVtbl->CreateCommandQueue(g.device,&q,&IID_ID3D12CommandQueue,(void**)&g.queue));
    OK(g.device->lpVtbl->CreateCommandAllocator(g.device,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&g.allocator));
    OK(g.device->lpVtbl->CreateCommandList(g.device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,g.allocator,NULL,&IID_ID3D12GraphicsCommandList,(void**)&g.list));
    OK(g.device->lpVtbl->CreateFence(g.device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&g.fence));
    g.event = CreateEventA(NULL,FALSE,FALSE,NULL); assert(g.event);
    exercise_frame(&g); unsupported_guards(&g); OK(g.list->lpVtbl->Close(g.list));
    ID3D12InfoQueue* info = NULL;
    if (SUCCEEDED(g.device->lpVtbl->QueryInterface(g.device,&IID_ID3D12InfoQueue,(void**)&info))) {
        UINT64 count = info->lpVtbl->GetNumStoredMessagesAllowedByRetrievalFilter(info); unsigned errors = 0;
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0; OK(info->lpVtbl->GetMessage(info,i,NULL,&size));
            D3D12_MESSAGE* message = (D3D12_MESSAGE*)malloc(size); assert(message); OK(info->lpVtbl->GetMessage(info,i,message,&size));
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                fprintf(stderr,"D3D12 frame validation: %s\n",message->pDescription); ++errors;
            }
            free(message);
        }
        assert(!errors); RELEASE(info); puts("native frame D3D12 debug errors=0");
    }
    CloseHandle(g.event); RELEASE(g.list); RELEASE(g.allocator); RELEASE(g.queue); RELEASE(g.fence);
    RELEASE(g.device); RELEASE(warp); RELEASE(factory); return 0;
}
