/* Real descriptor heaps and GPU rendering with the actual sampler helpers.
 * Reuse the existing WARP setup, shader compilation and resource helpers. */
#include <float.h>
#define main pipeline_recipes_fixture_main
#include "test_pipeline_recipes.c"
#undef main
typedef uint32_t u32;
typedef uint64_t u64;
#include "sampler_pages_constants_actual.inc"
static struct {
    ID3D12Device* dev;
    ID3D12DescriptorHeap* smp_cpu_heap;
    ID3D12DescriptorHeap* smp_heap;
    ID3D12DescriptorHeap* smp_pages[4];
    u32 smp_page, smp_page_count, smp_step, smp_ring_used;
    int smp_upload_reclaim_pending;
    u32 smp_ring_slots[SMP_RING_TABLES][SMP_TABLE_SIZE];
} g;
static struct { u64 advances, completions; } g_ld_sampler_pages;
#include "sampler_pages_helpers_actual.inc"

static void transition(test_gpu* gpu,ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b={0};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=resource;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=before;b.Transition.StateAfter=after;
    gpu->list->lpVtbl->ResourceBarrier(gpu->list,1,&b);
}
static void copy_pixels(test_gpu* gpu,ID3D12Resource* target,ID3D12Resource* readback) {
    transition(gpu,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
    src.pResource=target;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width=dst.PlacedFootprint.Footprint.Height=32;
    dst.PlacedFootprint.Footprint.Depth=1;dst.PlacedFootprint.Footprint.RowPitch=256;
    gpu->list->lpVtbl->CopyTextureRegion(gpu->list,&dst,0,0,0,&src,NULL);
    submit_wait(gpu);
}
static void draw(test_gpu* gpu,ID3D12PipelineState* pso,ID3D12RootSignature* root,
    ID3D12DescriptorHeap* srv_heap,D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    const D3D12_VERTEX_BUFFER_VIEW* vertex,D3D12_GPU_DESCRIPTOR_HANDLE sampler,
    unsigned left,unsigned right) {
    D3D12_VIEWPORT vp={0,0,32,32,0,1};D3D12_RECT sc={(LONG)left,0,(LONG)right,32};
    ID3D12DescriptorHeap* heaps[2]={srv_heap,g.smp_heap};
    D3D12_GPU_DESCRIPTOR_HANDLE texture;srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap,&texture);
    gpu->list->lpVtbl->SetDescriptorHeaps(gpu->list,2,heaps);
    gpu->list->lpVtbl->SetPipelineState(gpu->list,pso);
    gpu->list->lpVtbl->SetGraphicsRootSignature(gpu->list,root);
    /* Match sink_end_impl: both root tables are rebound after heap selection. */
    gpu->list->lpVtbl->SetGraphicsRootDescriptorTable(gpu->list,0,texture);
    gpu->list->lpVtbl->SetGraphicsRootDescriptorTable(gpu->list,1,sampler);
    gpu->list->lpVtbl->RSSetViewports(gpu->list,1,&vp);
    gpu->list->lpVtbl->RSSetScissorRects(gpu->list,1,&sc);
    gpu->list->lpVtbl->OMSetRenderTargets(gpu->list,1,&rtv,FALSE,NULL);
    gpu->list->lpVtbl->IASetVertexBuffers(gpu->list,0,1,vertex);
    gpu->list->lpVtbl->IASetPrimitiveTopology(gpu->list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu->list->lpVtbl->DrawInstanced(gpu->list,3,1,0,0);
}
static void check_pixel(const uint8_t* pixels,unsigned x,const float color[4]) {
    const uint8_t* p=pixels+16*256+x*4;
    for(unsigned channel=0;channel<4;channel++)
        assert(color[channel]>0.5f ? p[channel]>240 : p[channel]<8);
}
int main(void) {
    test_gpu gpu;gpu_init(&gpu);g.dev=gpu.device;g.smp_page_count=4;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors=4;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&g.smp_cpu_heap));
    g.smp_step=g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev,hd.Type);
    hd.NumDescriptors=SMP_RING_TABLES*SMP_TABLE_SIZE;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    for(unsigned page=0;page<4;page++)
        OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&g.smp_pages[page]));
    ld_sampler_pages_reset();
    const float colors[4][4]={{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,0,1}};
    D3D12_SAMPLER_DESC sd={0};sd.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sd.MaxAnisotropy=1;sd.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sd.MaxLOD=FLT_MAX;
    for(unsigned page=0;page<4;page++) {
        memcpy(sd.BorderColor,colors[page],sizeof(sd.BorderColor));
        g.dev->lpVtbl->CreateSampler(g.dev,&sd,smp_cpu(page));
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]={{0}};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=1;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;ranges[1].NumDescriptors=SMP_TABLE_SIZE;
    D3D12_ROOT_PARAMETER parameters[2]={{0}};
    for(unsigned i=0;i<2;i++) {
        parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[i].DescriptorTable.NumDescriptorRanges=1;parameters[i].DescriptorTable.pDescriptorRanges=&ranges[i];
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc={0};root_desc.NumParameters=2;root_desc.pParameters=parameters;
    root_desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* serialized=NULL;OK(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,NULL));
    ID3D12RootSignature* root=NULL;OK(g.dev->lpVtbl->CreateRootSignature(g.dev,0,
        serialized->lpVtbl->GetBufferPointer(serialized),serialized->lpVtbl->GetBufferSize(serialized),&IID_ID3D12RootSignature,(void**)&root));
    ID3DBlob* vs=compile_shader("float4 main(float2 p:POSITION):SV_Position{return float4(p,0,1);}","vs_5_0");
    ID3DBlob* ps=compile_shader("Texture2D t:register(t0);SamplerState s:register(s0);float4 main():SV_Target{return t.SampleLevel(s,float2(-1,-1),0);}","ps_5_0");
    D3D12_INPUT_ELEMENT_DESC input={0};input.SemanticName="POSITION";input.Format=DXGI_FORMAT_R32G32_FLOAT;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc=make_desc(root,vs->lpVtbl->GetBufferPointer(vs),vs->lpVtbl->GetBufferSize(vs),
        ps->lpVtbl->GetBufferPointer(ps),ps->lpVtbl->GetBufferSize(ps),&input);
    ID3D12PipelineState* pso=create_pso(g.dev,&desc);
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd={0};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width=rd.Height=32;rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;
    rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *target=NULL,*texture=NULL,*readback=NULL,*vb=NULL;
    OK(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&target));
    rd.Width=rd.Height=1;
    OK(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&texture));
    ID3D12DescriptorHeap *rtv_heap=NULL,*srv_heap=NULL;
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,trtv;rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap,&rtv);trtv=rtv;
    trtv.ptr+=g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev,hd.Type);
    g.dev->lpVtbl->CreateRenderTargetView(g.dev,target,NULL,rtv);g.dev->lpVtbl->CreateRenderTargetView(g.dev,texture,NULL,trtv);
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&srv_heap));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv={0};srv.Format=rd.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(srv_heap,&srv_cpu);
    g.dev->lpVtbl->CreateShaderResourceView(g.dev,texture,&srv,srv_cpu);
    const float white[4]={1,1,1,1};gpu.list->lpVtbl->ClearRenderTargetView(gpu.list,trtv,white,0,NULL);
    transition(&gpu,texture,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=8192;rd.Height=1;
    rd.Format=DXGI_FORMAT_UNKNOWN;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;rd.Flags=D3D12_RESOURCE_FLAG_NONE;
    hp.Type=D3D12_HEAP_TYPE_READBACK;
    OK(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback));
    const float vertices[6]={-1,-1,-1,3,3,-1};hp.Type=D3D12_HEAP_TYPE_UPLOAD;rd.Width=sizeof(vertices);
    OK(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)&vb));
    void* mapped=NULL;D3D12_RANGE none={0,0};OK(vb->lpVtbl->Map(vb,0,&none,&mapped));memcpy(mapped,vertices,sizeof(vertices));vb->lpVtbl->Unmap(vb,0,NULL);
    D3D12_VERTEX_BUFFER_VIEW view={0};view.BufferLocation=vb->lpVtbl->GetGPUVirtualAddress(vb);view.SizeInBytes=sizeof(vertices);view.StrideInBytes=8;
    for(unsigned page=0;page<4;page++) {
        u32 slots[SMP_TABLE_SIZE]={0};slots[0]=page;
        D3D12_GPU_DESCRIPTOR_HANDLE table=sampler_table(slots);assert(table.ptr);
        draw(&gpu,pso,root,srv_heap,rtv,&view,table,page*8,(page+1)*8);
        for(unsigned pattern=1;pattern<SMP_RING_TABLES;pattern++) {
            for(unsigned bit=0;bit<7;bit++)slots[bit+1]=(pattern>>bit)&1;
            assert(sampler_table(slots).ptr);
        }
        assert(g.smp_ring_used==SMP_RING_TABLES);
        assert(!sampler_table_needs_flush(slots));slots[8]=1;
        assert(sampler_table_needs_flush(slots));
        if(page<3)assert(ld_sampler_pages_advance());else assert(!ld_sampler_pages_advance());
    }
    assert(g_ld_sampler_pages.advances==3 && !gpu.fence_value);
    copy_pixels(&gpu,target,readback);
    uint8_t* pixels=NULL;D3D12_RANGE read={0,8192};OK(readback->lpVtbl->Map(readback,0,&read,(void**)&pixels));
    for(unsigned page=0;page<4;page++)check_pixel(pixels,page*8+4,colors[page]);
    readback->lpVtbl->Unmap(readback,0,&none);
    puts("four sampler pages preserve all earlier draws before one GPU submission");
    reset_list(&gpu);ld_sampler_pages_completed();
    const float magenta[4]={1,0,1,1};memcpy(sd.BorderColor,magenta,sizeof(sd.BorderColor));
    g.dev->lpVtbl->CreateSampler(g.dev,&sd,smp_cpu(0));
    /* The live settings transaction calls this only after its prepare fence. */
    ld_sampler_pages_reset();u32 slots[SMP_TABLE_SIZE]={0};
    transition(&gpu,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    draw(&gpu,pso,root,srv_heap,rtv,&view,sampler_table(slots),0,32);
    copy_pixels(&gpu,target,readback);OK(readback->lpVtbl->Map(readback,0,&read,(void**)&pixels));
    check_pixel(pixels,16,magenta);readback->lpVtbl->Unmap(readback,0,&none);
    puts("fenced sampler identity reset copies new descriptors instead of stale tables");
    for(unsigned page=0;page<4;page++)RELEASE(g.smp_pages[page]);
    RELEASE(g.smp_cpu_heap);RELEASE(srv_heap);RELEASE(rtv_heap);RELEASE(vb);RELEASE(readback);
    RELEASE(texture);RELEASE(target);RELEASE(pso);RELEASE(vs);RELEASE(ps);RELEASE(root);RELEASE(serialized);
    gpu_destroy(&gpu);puts("actual-source sampler page WARP lifetime/rendering passed");return 0;
}
