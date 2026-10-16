/* Actual pipeline recipe module on D3D12 WARP. */
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
#include <winioctl.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_pipeline_recipes.h"

#define OK(expression) do { HRESULT h_=(expression); if(FAILED(h_)){fprintf(stderr,"line %d HRESULT 0x%08lX\n",__LINE__,(unsigned long)h_);abort();} } while(0)
#define RELEASE(x) do { if(x){(x)->lpVtbl->Release(x);(x)=NULL;} } while(0)

typedef struct test_gpu {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE event;
    UINT64 fence_value;
} test_gpu;

typedef struct counting_device {
    ID3D12Device iface;
    ID3D12DeviceVtbl vtbl;
    ID3D12Device* real;
    ULONG refs;
    unsigned creates;
} counting_device;

static ULONG STDMETHODCALLTYPE proxy_addref(ID3D12Device* self) {
    counting_device* p=(counting_device*)self; return ++p->refs;
}
static ULONG STDMETHODCALLTYPE proxy_release(ID3D12Device* self) {
    counting_device* p=(counting_device*)self; assert(p->refs); return --p->refs;
}
static HRESULT STDMETHODCALLTYPE proxy_create_pso(ID3D12Device* self,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID iid, void** result) {
    counting_device* p=(counting_device*)self; ++p->creates;
    return p->real->lpVtbl->CreateGraphicsPipelineState(p->real,desc,iid,result);
}
static void proxy_init(counting_device* p,ID3D12Device* real) {
    memset(p,0,sizeof(*p));p->real=real;p->refs=1;p->vtbl=*real->lpVtbl;
    p->vtbl.AddRef=proxy_addref;p->vtbl.Release=proxy_release;
    p->vtbl.CreateGraphicsPipelineState=proxy_create_pso;p->iface.lpVtbl=&p->vtbl;
}

static void gpu_init(test_gpu* g) {
    IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;ID3D12Debug* debug=NULL;memset(g,0,sizeof(*g));
    if(SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug,(void**)&debug))){debug->lpVtbl->EnableDebugLayer(debug);RELEASE(debug);}
    OK(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
    OK(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
    OK(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&g->device));
    D3D12_COMMAND_QUEUE_DESC q={0};OK(g->device->lpVtbl->CreateCommandQueue(g->device,&q,&IID_ID3D12CommandQueue,(void**)&g->queue));
    OK(g->device->lpVtbl->CreateCommandAllocator(g->device,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&g->allocator));
    OK(g->device->lpVtbl->CreateCommandList(g->device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,g->allocator,NULL,&IID_ID3D12GraphicsCommandList,(void**)&g->list));
    OK(g->device->lpVtbl->CreateFence(g->device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&g->fence));
    g->event=CreateEventW(NULL,FALSE,FALSE,NULL);assert(g->event);
    RELEASE(warp);RELEASE(factory);
}
static void submit_wait(test_gpu* g) {
    OK(g->list->lpVtbl->Close(g->list));ID3D12CommandList* lists[]={(ID3D12CommandList*)g->list};
    g->queue->lpVtbl->ExecuteCommandLists(g->queue,1,lists);++g->fence_value;
    OK(g->queue->lpVtbl->Signal(g->queue,g->fence,g->fence_value));
    OK(g->fence->lpVtbl->SetEventOnCompletion(g->fence,g->fence_value,g->event));
    assert(WaitForSingleObject(g->event,30000)==WAIT_OBJECT_0);
    assert(g->fence->lpVtbl->GetCompletedValue(g->fence)>=g->fence_value);
}
static void reset_list(test_gpu* g) {
    assert(g->fence->lpVtbl->GetCompletedValue(g->fence)>=g->fence_value);
    OK(g->allocator->lpVtbl->Reset(g->allocator));OK(g->list->lpVtbl->Reset(g->list,g->allocator,NULL));
}
static void gpu_destroy(test_gpu* g) {
    if(g->list)g->list->lpVtbl->Close(g->list);if(g->event)CloseHandle(g->event);
    RELEASE(g->list);RELEASE(g->allocator);RELEASE(g->queue);RELEASE(g->fence);RELEASE(g->device);
}

static ID3D12RootSignature* make_root(ID3D12Device* device,int constants,ID3DBlob** serialized) {
    D3D12_ROOT_PARAMETER parameter={0};D3D12_ROOT_SIGNATURE_DESC desc={0};
    desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if(constants){parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.ShaderRegister=0;parameter.Constants.RegisterSpace=0;parameter.Constants.Num32BitValues=1;
        parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;desc.NumParameters=1;desc.pParameters=&parameter;}
    OK(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,serialized,NULL));
    ID3D12RootSignature* root=NULL;OK(device->lpVtbl->CreateRootSignature(device,0,
        (*serialized)->lpVtbl->GetBufferPointer(*serialized),(*serialized)->lpVtbl->GetBufferSize(*serialized),
        &IID_ID3D12RootSignature,(void**)&root));return root;
}
static ID3DBlob* compile_shader(const char* source,const char* target) {
    ID3DBlob* blob=NULL;ID3DBlob* errors=NULL;
    HRESULT hr=D3DCompile(source,strlen(source),NULL,NULL,NULL,"main",target,0,0,&blob,&errors);
    if(FAILED(hr)&&errors)fprintf(stderr,"shader error: %s\n",(const char*)errors->lpVtbl->GetBufferPointer(errors));
    RELEASE(errors);OK(hr);return blob;
}
static D3D12_GRAPHICS_PIPELINE_STATE_DESC make_desc(ID3D12RootSignature* root,
    const void* vs,size_t vs_size,const void* ps,size_t ps_size,
    D3D12_INPUT_ELEMENT_DESC* inputs) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;memset(&d,0,sizeof(d));d.pRootSignature=root;
    d.VS.pShaderBytecode=vs;d.VS.BytecodeLength=vs_size;d.PS.pShaderBytecode=ps;d.PS.BytecodeLength=ps_size;
    d.InputLayout.pInputElementDescs=inputs;d.InputLayout.NumElements=1;d.SampleMask=UINT_MAX;
    d.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;d.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable=TRUE;d.DepthStencilState.DepthEnable=FALSE;
    d.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;d.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    d.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_ONE;d.BlendState.RenderTarget[0].DestBlend=D3D12_BLEND_ZERO;
    d.BlendState.RenderTarget[0].BlendOp=D3D12_BLEND_OP_ADD;d.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ONE;
    d.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_ZERO;d.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;
    d.BlendState.RenderTarget[0].LogicOp=D3D12_LOGIC_OP_NOOP;d.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;d.NumRenderTargets=1;
    d.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;return d;
}
static ID3D12PipelineState* create_pso(ID3D12Device* device,const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc) {
    ID3D12PipelineState* pso=NULL;HRESULT hr=device->lpVtbl->CreateGraphicsPipelineState(device,desc,&IID_ID3D12PipelineState,(void**)&pso);
    if(FAILED(hr)){ID3D12InfoQueue* q=NULL;if(SUCCEEDED(device->lpVtbl->QueryInterface(device,&IID_ID3D12InfoQueue,(void**)&q))){
        UINT64 n=q->lpVtbl->GetNumStoredMessages(q);for(UINT64 i=0;i<n;i++){SIZE_T bytes=0;q->lpVtbl->GetMessage(q,i,NULL,&bytes);
            D3D12_MESSAGE* m=(D3D12_MESSAGE*)malloc(bytes);if(m&&SUCCEEDED(q->lpVtbl->GetMessage(q,i,m,&bytes)))fprintf(stderr,"D3D12: %s\n",m->pDescription);free(m);}
        RELEASE(q);}}
    OK(hr);return pso;
}
static wchar_t* archive_path(void) {
    wchar_t temp[MAX_PATH];DWORD n=GetTempPathW(MAX_PATH,temp);assert(n&&n<MAX_PATH);
    wchar_t* path=(wchar_t*)calloc((size_t)n+96,sizeof(wchar_t));assert(path);
    swprintf(path,(size_t)n+96,L"%shaze-pipeline-recipes-%lu-%llu.bin",temp,
        (unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());return path;
}
static uint8_t* read_file(const wchar_t* path,size_t* size) {
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);assert(f!=INVALID_HANDLE_VALUE);
    LARGE_INTEGER n;assert(GetFileSizeEx(f,&n)&&n.QuadPart>=0&&(uint64_t)n.QuadPart<SIZE_MAX);
    uint8_t* p=(uint8_t*)malloc((size_t)n.QuadPart);assert(p);DWORD got=0;assert(ReadFile(f,p,(DWORD)n.QuadPart,&got,NULL)&&got==(DWORD)n.QuadPart);
    CloseHandle(f);*size=(size_t)n.QuadPart;return p;
}
static void write_file(const wchar_t* path,const uint8_t* bytes,size_t size) {
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);assert(f!=INVALID_HANDLE_VALUE);
    DWORD wrote=0;assert(size<=UINT32_MAX&&WriteFile(f,bytes,(DWORD)size,&wrote,NULL)&&wrote==(DWORD)size);assert(FlushFileBuffers(f));CloseHandle(f);
}
static uint32_t crc32_test(const uint8_t* p,size_t n) {
    uint32_t crc=0xFFFFFFFFu;for(size_t i=0;i<n;i++){crc^=p[i];for(unsigned b=0;b<8;b++)crc=(crc>>1)^(0xEDB88320u&(uint32_t)-(int32_t)(crc&1u));}return ~crc;
}
static uint32_t read_u32(const uint8_t* p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void write_u32(uint8_t* p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}

static void render_and_check(test_gpu* g,ID3D12PipelineState* pso,ID3D12RootSignature* root,int expect_blue) {
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC rd={0};
    rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=32;rd.Height=32;rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;
    rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* target=NULL;OK(g->device->lpVtbl->CreateCommittedResource(g->device,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&target));
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;ID3D12DescriptorHeap* rtv_heap=NULL;
    OK(g->device->lpVtbl->CreateDescriptorHeap(g->device,&hd,&IID_ID3D12DescriptorHeap,(void**)&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap,&rtv);
    g->device->lpVtbl->CreateRenderTargetView(g->device,target,NULL,rtv);
    D3D12_RESOURCE_DESC rb=rd;rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=8192;rb.Height=rb.DepthOrArraySize=rb.MipLevels=rb.SampleDesc.Count=1;rb.Format=DXGI_FORMAT_UNKNOWN;
    rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;rb.Flags=D3D12_RESOURCE_FLAG_NONE;D3D12_HEAP_PROPERTIES rhp={0};rhp.Type=D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* readback=NULL;OK(g->device->lpVtbl->CreateCommittedResource(g->device,&rhp,D3D12_HEAP_FLAG_NONE,&rb,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback));
    static const float vertices[6]={-1.0f,-1.0f,-1.0f,3.0f,3.0f,-1.0f};D3D12_HEAP_PROPERTIES uhp={0};uhp.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC vbdesc=rb;vbdesc.Width=sizeof(vertices);ID3D12Resource* vb=NULL;
    OK(g->device->lpVtbl->CreateCommittedResource(g->device,&uhp,D3D12_HEAP_FLAG_NONE,&vbdesc,D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)&vb));
    void* mapped=NULL;D3D12_RANGE no_read={0,0};OK(vb->lpVtbl->Map(vb,0,&no_read,&mapped));memcpy(mapped,vertices,sizeof(vertices));D3D12_RANGE write={0,sizeof(vertices)};vb->lpVtbl->Unmap(vb,0,&write);
    float clear[4]={0,0,1,1};g->list->lpVtbl->ClearRenderTargetView(g->list,rtv,clear,0,NULL);
    D3D12_VIEWPORT vp={0,0,32,32,0,1};D3D12_RECT sc={0,0,32,32};g->list->lpVtbl->RSSetViewports(g->list,1,&vp);g->list->lpVtbl->RSSetScissorRects(g->list,1,&sc);
    g->list->lpVtbl->OMSetRenderTargets(g->list,1,&rtv,FALSE,NULL);g->list->lpVtbl->SetGraphicsRootSignature(g->list,root);g->list->lpVtbl->SetPipelineState(g->list,pso);
    D3D12_VERTEX_BUFFER_VIEW view={0};view.BufferLocation=vb->lpVtbl->GetGPUVirtualAddress(vb);view.SizeInBytes=sizeof(vertices);view.StrideInBytes=8;
    g->list->lpVtbl->IASetVertexBuffers(g->list,0,1,&view);g->list->lpVtbl->IASetPrimitiveTopology(g->list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g->list->lpVtbl->DrawInstanced(g->list,3,1,0,0);
    D3D12_RESOURCE_BARRIER b={0};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=target;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;g->list->lpVtbl->ResourceBarrier(g->list,1,&b);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp={0};fp.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;fp.Footprint.Width=fp.Footprint.Height=32;fp.Footprint.Depth=1;fp.Footprint.RowPitch=256;
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=target;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    g->list->lpVtbl->CopyTextureRegion(g->list,&dst,0,0,0,&src,NULL);submit_wait(g);
    D3D12_RANGE read={0,8192};uint8_t* pixels=NULL;OK(readback->lpVtbl->Map(readback,0,&read,(void**)&pixels));
    const uint8_t* center=pixels+16*256+16*4;
    assert(center[0]>240&&center[1]<8&&center[2]==(expect_blue?255:0)&&center[3]>240);
    D3D12_RANGE none={0,0};readback->lpVtbl->Unmap(readback,0,&none);RELEASE(vb);RELEASE(readback);RELEASE(rtv_heap);RELEASE(target);reset_list(g);
}

static void corrupt_second_record(const wchar_t* source,const wchar_t* output) {
    size_t n=0;uint8_t* b=read_file(source,&n);assert(n>64);uint32_t root_size=read_u32(b+24),count=read_u32(b+20);assert(count>=2);
    size_t at=40u+root_size;uint32_t first=read_u32(b+at);at+=16u+first;assert(at+16<n);uint32_t second=read_u32(b+at);assert(second&&at+16u+second<=n);
    b[at+16u+(second/2u)]^=0x5Au;write_u32(b+at+4u,crc32_test(b+at+16u,second));
    write_u32(b+32u,crc32_test(b+40u,n-40u));write_file(output,b,n);free(b);
}
static int write_oversized_sparse(const wchar_t* path) {
    HANDLE f=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,NULL);
    if(f==INVALID_HANDLE_VALUE)return 0;
    DWORD returned=0;
    if(!DeviceIoControl(f,FSCTL_SET_SPARSE,NULL,0,NULL,0,&returned,NULL)){CloseHandle(f);DeleteFileW(path);return 0;}
    LARGE_INTEGER end;end.QuadPart=(LONGLONG)RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES+1;
    if(!SetFilePointerEx(f,end,NULL,FILE_BEGIN)||!SetEndOfFile(f)){CloseHandle(f);DeleteFileW(path);return 0;}
    CloseHandle(f);return 1;
}

static void rejected_load(counting_device* proxy,ID3D12RootSignature* root,
    const void* root_bytes,size_t root_size,const wchar_t* path,const char* label) {
    rsx_pipeline_recipes* cache=rsx_pipeline_recipes_create(&proxy->iface,root,root_bytes,root_size,path);assert(cache);
    unsigned before=proxy->creates;rsx_pipeline_recipes_stats stats={0};
    assert(!rsx_pipeline_recipes_load(cache,&stats));assert(proxy->creates==before);
    rsx_pipeline_recipes_destroy(cache);printf("rejected archive %s before driver create\n",label);
}

static void test_large_archive(test_gpu* g,counting_device* proxy,ID3D12RootSignature* root,
    const void* root_bytes,size_t root_size,ID3DBlob* pixel_shader) {
    const uint32_t recipe_count=12;
    const size_t private_size=1750u*1024u;
    const char* source="float4 main(float2 p:POSITION):SV_Position { return float4(p,0,1); }\n";
    ID3DBlob* small_vs=compile_shader(source,"vs_5_0");
    uint8_t* private_data=(uint8_t*)malloc(private_size);assert(private_data);
    memset(private_data,0xA5,private_size);
    ID3DBlob* large_vs=NULL;
    HRESULT pad_hr=D3DSetBlobPart(small_vs->lpVtbl->GetBufferPointer(small_vs),
        small_vs->lpVtbl->GetBufferSize(small_vs),D3D_BLOB_PRIVATE_DATA,0,
        private_data,private_size,&large_vs);
    if(FAILED(pad_hr))fprintf(stderr,"D3DSetBlobPart private-data padding failed: 0x%08lX\n",(unsigned long)pad_hr);
    OK(pad_hr);free(private_data);RELEASE(small_vs);
    size_t vs_size=large_vs->lpVtbl->GetBufferSize(large_vs);
    assert(vs_size>private_size&&vs_size<4u*1024u*1024u);
    D3D12_INPUT_ELEMENT_DESC input={0};char semantic[]="POSITION";input.SemanticName=semantic;input.Format=DXGI_FORMAT_R32G32_FLOAT;
    input.InputSlotClass=D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC base=make_desc(root,large_vs->lpVtbl->GetBufferPointer(large_vs),vs_size,
        pixel_shader->lpVtbl->GetBufferPointer(pixel_shader),pixel_shader->lpVtbl->GetBufferSize(pixel_shader),&input);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC* descs=(D3D12_GRAPHICS_PIPELINE_STATE_DESC*)calloc(recipe_count,sizeof(*descs));assert(descs);
    wchar_t* path=archive_path();DeleteFileW(path);
    rsx_pipeline_recipes* cache=rsx_pipeline_recipes_create(&proxy->iface,root,root_bytes,root_size,path);assert(cache);
    for(uint32_t i=0;i<recipe_count;i++){
        descs[i]=base;descs[i].SampleMask=UINT_MAX-i*0x01010101u;
        ID3D12PipelineState* pso=create_pso(&proxy->iface,&descs[i]);assert(rsx_pipeline_recipes_remember(cache,&descs[i],pso));RELEASE(pso);
    }
    assert(rsx_pipeline_recipes_save_atomic(cache));rsx_pipeline_recipes_destroy(cache);
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    assert(file!=INVALID_HANDLE_VALUE);LARGE_INTEGER archive_size;assert(GetFileSizeEx(file,&archive_size));CloseHandle(file);
    assert(archive_size.QuadPart>20ll*1024ll*1024ll&&archive_size.QuadPart<256ll*1024ll*1024ll);
    cache=rsx_pipeline_recipes_create(&proxy->iface,root,root_bytes,root_size,path);assert(cache);
    unsigned before=proxy->creates;rsx_pipeline_recipes_stats stats={0};assert(rsx_pipeline_recipes_load(cache,&stats));
    assert(proxy->creates==before+recipe_count&&stats.loaded==recipe_count&&stats.prewarmed==recipe_count&&stats.prewarm_failed==0);
    for(uint32_t i=0;i<recipe_count;i++){before=proxy->creates;ID3D12PipelineState* found=rsx_pipeline_recipes_find(cache,&descs[i]);assert(found&&proxy->creates==before);if(i==0)render_and_check(g,found,root,0);RELEASE(found);}
    printf("large aggregate archive roundtrip passed: recipes=%u bytes=%lld padded_stage=%zu\n",recipe_count,archive_size.QuadPart,vs_size);
    rsx_pipeline_recipes_destroy(cache);free(descs);DeleteFileW(path);free(path);RELEASE(large_vs);
}

int main(void) {
    test_gpu g;gpu_init(&g);counting_device proxy;proxy_init(&proxy,g.device);
    ID3DBlob *root_blob=NULL,*other_root_blob=NULL,*vs_blob=compile_shader(
        "float4 main(float2 p:POSITION):SV_Position{return float4(p,0,1);}","vs_5_0");
    ID3DBlob *ps_blob=compile_shader("float4 main():SV_Target{return float4(1,0,0,1);}","ps_5_0");
    ID3D12RootSignature* root=make_root(g.device,0,&root_blob);ID3D12RootSignature* other_root=make_root(g.device,1,&other_root_blob);
    size_t vs_n=vs_blob->lpVtbl->GetBufferSize(vs_blob),ps_n=ps_blob->lpVtbl->GetBufferSize(ps_blob),root_n=root_blob->lpVtbl->GetBufferSize(root_blob);
    uint8_t* vs_source=(uint8_t*)malloc(vs_n);uint8_t* ps_source=(uint8_t*)malloc(ps_n);uint8_t* root_source=(uint8_t*)malloc(root_n);
    assert(vs_source&&ps_source&&root_source);memcpy(vs_source,vs_blob->lpVtbl->GetBufferPointer(vs_blob),vs_n);memcpy(ps_source,ps_blob->lpVtbl->GetBufferPointer(ps_blob),ps_n);
    memcpy(root_source,root_blob->lpVtbl->GetBufferPointer(root_blob),root_n);
    char semantic[16]="POSITION";D3D12_INPUT_ELEMENT_DESC input={0};input.SemanticName=semantic;input.Format=DXGI_FORMAT_R32G32_FLOAT;
    input.InputSlot=0;input.AlignedByteOffset=0;input.InputSlotClass=D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc=make_desc(root,vs_source,vs_n,ps_source,ps_n,&input);
    ID3D12PipelineState* pso1=create_pso(g.device,&desc);D3D12_GRAPHICS_PIPELINE_STATE_DESC desc2=desc;
    desc2.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_RED|D3D12_COLOR_WRITE_ENABLE_GREEN;
    ID3D12PipelineState* pso2=create_pso(g.device,&desc2);
    wchar_t* path=archive_path();DeleteFileW(path);
    rsx_pipeline_recipes* cache=rsx_pipeline_recipes_create(&proxy.iface,root,root_source,root_n,path);assert(cache);
    assert(rsx_pipeline_recipes_remember(cache,&desc,pso1));assert(rsx_pipeline_recipes_remember(cache,&desc2,pso2));
    memset(vs_source,0xCC,vs_n);memset(ps_source,0xDD,ps_n);memset(semantic,'X',8);memset(root_source,0xEE,root_n);
    RELEASE(pso1);RELEASE(pso2);assert(rsx_pipeline_recipes_save_atomic(cache));rsx_pipeline_recipes_destroy(cache);
    free(vs_source);free(ps_source);free(root_source);

    rsx_pipeline_recipes* loaded=rsx_pipeline_recipes_create(&proxy.iface,root,
        root_blob->lpVtbl->GetBufferPointer(root_blob),root_n,path);assert(loaded);
    unsigned before=proxy.creates;rsx_pipeline_recipes_stats stats={0};assert(rsx_pipeline_recipes_load(loaded,&stats));
    if(proxy.creates!=before+2||stats.prewarmed!=2||stats.prewarm_failed!=0||stats.loaded!=2){
        fprintf(stderr,"load stats creates=%u before=%u loaded=%u prewarmed=%u failed=%u corrupt=%u\n",proxy.creates,before,stats.loaded,stats.prewarmed,stats.prewarm_failed,stats.corrupt_archives);abort();}
    uint8_t* clean_vs=(uint8_t*)malloc(vs_n);uint8_t* clean_ps=(uint8_t*)malloc(ps_n);assert(clean_vs&&clean_ps);
    memcpy(clean_vs,vs_blob->lpVtbl->GetBufferPointer(vs_blob),vs_n);memcpy(clean_ps,ps_blob->lpVtbl->GetBufferPointer(ps_blob),ps_n);
    char clean_semantic[]="POSITION";input.SemanticName=clean_semantic;desc=make_desc(root,clean_vs,vs_n,clean_ps,ps_n,&input);
    before=proxy.creates;ID3D12PipelineState* found=rsx_pipeline_recipes_find(loaded,&desc);
    assert(found&&proxy.creates==before);render_and_check(&g,found,root,0);RELEASE(found);
    char variant_semantic[]="POSITION";D3D12_INPUT_ELEMENT_DESC variant_input=input;variant_input.SemanticName=variant_semantic;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC loaded_desc2=desc;loaded_desc2.InputLayout.pInputElementDescs=&variant_input;
    loaded_desc2.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_RED|D3D12_COLOR_WRITE_ENABLE_GREEN;
    before=proxy.creates;ID3D12PipelineState* found2=rsx_pipeline_recipes_find(loaded,&loaded_desc2);
    assert(found2&&proxy.creates==before);render_and_check(&g,found2,root,1);RELEASE(found2);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC variant=desc;variant.BlendState.RenderTarget[0].RenderTargetWriteMask^=D3D12_COLOR_WRITE_ENABLE_RED;
    assert(!rsx_pipeline_recipes_find(loaded,&variant));variant=desc;variant.DepthStencilState.DepthEnable=TRUE;
    assert(!rsx_pipeline_recipes_find(loaded,&variant));variant=desc;variant.InputLayout.pInputElementDescs=&input;input.Format=DXGI_FORMAT_R32G32B32_FLOAT;
    assert(!rsx_pipeline_recipes_find(loaded,&variant));input.Format=DXGI_FORMAT_R32G32_FLOAT;
    variant=desc;variant.VS.pShaderBytecode=clean_vs;((uint8_t*)clean_vs)[vs_n/2]^=1;
    assert(!rsx_pipeline_recipes_find(loaded,&variant));((uint8_t*)clean_vs)[vs_n/2]^=1;
    variant=desc;variant.pRootSignature=other_root;assert(!rsx_pipeline_recipes_find(loaded,&variant));
    uint8_t stage_byte=0;variant=desc;variant.GS.pShaderBytecode=&stage_byte;variant.GS.BytecodeLength=1;assert(!rsx_pipeline_recipes_find(loaded,&variant));
    variant=desc;variant.HS.pShaderBytecode=&stage_byte;variant.HS.BytecodeLength=1;assert(!rsx_pipeline_recipes_find(loaded,&variant));
    variant=desc;variant.DS.pShaderBytecode=&stage_byte;variant.DS.BytecodeLength=1;assert(!rsx_pipeline_recipes_find(loaded,&variant));
    D3D12_SO_DECLARATION_ENTRY so={0};variant=desc;variant.StreamOutput.pSODeclaration=&so;variant.StreamOutput.NumEntries=1;
    assert(!rsx_pipeline_recipes_find(loaded,&variant));
    assert(proxy.creates==before);puts("recipe key mutations and unsupported stages rejected without driver creation");
    rsx_pipeline_recipes_destroy(loaded);

    size_t path_chars=wcslen(path);wchar_t* copy=(wchar_t*)calloc(path_chars+5,sizeof(wchar_t));assert(copy);
    memcpy(copy,path,path_chars*sizeof(wchar_t));memcpy(copy+path_chars,L".bad",5*sizeof(wchar_t));
    size_t archive_n=0;uint8_t* archive=read_file(path,&archive_n);assert(read_u32(archive+20)==2);
    write_file(copy,archive,archive_n-1);rejected_load(&proxy,root,root_blob->lpVtbl->GetBufferPointer(root_blob),root_n,copy,"truncated");
    uint8_t* version=(uint8_t*)malloc(archive_n);assert(version);memcpy(version,archive,archive_n);write_u32(version+8,99);write_file(copy,version,archive_n);
    rejected_load(&proxy,root,root_blob->lpVtbl->GetBufferPointer(root_blob),root_n,copy,"version");
    corrupt_second_record(path,copy);rejected_load(&proxy,root,root_blob->lpVtbl->GetBufferPointer(root_blob),root_n,copy,"second-record checksum");
    if(write_oversized_sparse(copy)){rejected_load(&proxy,root,root_blob->lpVtbl->GetBufferPointer(root_blob),root_n,copy,"oversized sparse archive");}
    else puts("oversized sparse-file test unavailable");
    ID3DBlob* bad_root_blob=NULL;ID3D12RootSignature* bad_root=make_root(g.device,1,&bad_root_blob);
    rsx_pipeline_recipes* root_mismatch=rsx_pipeline_recipes_create(&proxy.iface,bad_root,bad_root_blob->lpVtbl->GetBufferPointer(bad_root_blob),
        bad_root_blob->lpVtbl->GetBufferSize(bad_root_blob),path);assert(root_mismatch);before=proxy.creates;
    assert(!rsx_pipeline_recipes_load(root_mismatch,NULL)&&proxy.creates==before);rsx_pipeline_recipes_destroy(root_mismatch);
    puts("root signature mismatch rejected before driver creation");

    test_large_archive(&g,&proxy,root,root_blob->lpVtbl->GetBufferPointer(root_blob),root_n,ps_blob);

    free(archive);free(version);free(copy);free(clean_vs);free(clean_ps);DeleteFileW(path);free(path);
    RELEASE(vs_blob);RELEASE(ps_blob);RELEASE(bad_root);RELEASE(bad_root_blob);RELEASE(root);RELEASE(other_root);RELEASE(root_blob);RELEASE(other_root_blob);
    assert(proxy.refs==1);gpu_destroy(&g);puts("actual-source pipeline recipe WARP roundtrip passed");return 0;
}
