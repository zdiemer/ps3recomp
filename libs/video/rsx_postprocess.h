/* Optional presentation-only FXAA, sharpening and exposure. Source render
 * targets stay untouched: later guest texture reads see the game's image. */
#ifndef RSX_POSTPROCESS_H
#define RSX_POSTPROCESS_H
typedef struct {
    ID3D12RootSignature* root;
    ID3D12PipelineState* pipeline;
    ID3D12DescriptorHeap* heap;
    ID3D12Resource* output;
    UINT step, width, height;
} rsx_postprocess;
static void rsx_postprocess_destroy(rsx_postprocess* p) {
    if (p->output) p->output->lpVtbl->Release(p->output);
    if (p->heap) p->heap->lpVtbl->Release(p->heap);
    if (p->pipeline) p->pipeline->lpVtbl->Release(p->pipeline);
    if (p->root) p->root->lpVtbl->Release(p->root);
    memset(p,0,sizeof(*p));
}
static int rsx_postprocess_init(rsx_postprocess* p,ID3D12Device* dev,UINT w,UINT h) {
    static const char shader[] =
        "Texture2D<float4> src:register(t0); RWTexture2D<float4> dst:register(u0);\n"
        "SamplerState linearClamp:register(s0);\n"
        "cbuffer Settings:register(b0) { uint aa; float sharp; float exposure; };\n"
        "float lum(float3 c) { return dot(c,float3(0.299,0.587,0.114)); }\n"
        "float3 sampleAt(float2 uv) { return src.SampleLevel(linearClamp,uv,0).rgb; }\n"
        "[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {\n"
        " uint w,h; dst.GetDimensions(w,h); if(id.x>=w || id.y>=h) return;\n"
        " float2 px=1.0/float2(w,h), uv=(float2(id.xy)+0.5)*px;\n"
        " float4 original=src.SampleLevel(linearClamp,uv,0); float3 c=original.rgb;\n"
        " float3 nw=sampleAt(uv+px*float2(-1,-1)), ne=sampleAt(uv+px*float2(1,-1));\n"
        " float3 sw=sampleAt(uv+px*float2(-1,1)), se=sampleAt(uv+px*float2(1,1));\n"
        " float m=lum(c), a=lum(nw), b=lum(ne), d=lum(sw), e=lum(se);\n"
        " float lo=min(m,min(min(a,b),min(d,e))), hi=max(m,max(max(a,b),max(d,e)));\n"
        " if(aa!=0 && hi-lo>=max(0.0312,hi*0.125)) {\n"
        "   float2 dir=float2(-((a+b)-(d+e)),((a+d)-(b+e)));\n"
        "   float reduce=max((a+b+d+e)*(0.25*0.125),1.0/128.0);\n"
        "   dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),-8.0,8.0)*px;\n"
        "   float3 ca=0.5*(sampleAt(uv+dir*(-1.0/6.0))+sampleAt(uv+dir*(1.0/6.0)));\n"
        "   float3 cb=ca*0.5+0.25*(sampleAt(uv-dir*0.5)+sampleAt(uv+dir*0.5));\n"
        "   float lb=lum(cb); c=(lb<lo || lb>hi)?ca:cb;\n"
        " }\n"
        " if(sharp>0) {\n"
        "   float3 n=sampleAt(uv+float2(0,-px.y)), s=sampleAt(uv+float2(0,px.y));\n"
        "   float3 l=sampleAt(uv+float2(-px.x,0)), r=sampleAt(uv+float2(px.x,0));\n"
        "   float3 low=min(original.rgb,min(min(n,s),min(l,r)));\n"
        "   float3 high=max(original.rgb,max(max(n,s),max(l,r)));\n"
        "   c=clamp(c+sharp*(original.rgb-(n+s+l+r)*0.25),low,high);\n"
        " }\n"
        " dst[id.xy]=float4(saturate(c*exp2(exposure)),original.a);\n"
        "}\n";
    ID3DBlob *code=NULL,*errors=NULL,*signature=NULL;
    HRESULT hr=D3DCompile(shader,sizeof(shader)-1,"presentation",NULL,NULL,"main","cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if (FAILED(hr)) {
        fprintf(stderr,"[graphics] presentation shader: %s\n",errors ?
            (const char*)errors->lpVtbl->GetBufferPointer(errors):"compile failed");
    }
    if (errors) errors->lpVtbl->Release(errors);
    if (FAILED(hr)) return -1;
    D3D12_DESCRIPTOR_RANGE range={0};
    range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=1;
    D3D12_DESCRIPTOR_RANGE urange={0};
    urange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_UAV; urange.NumDescriptors=1;
    urange.OffsetInDescriptorsFromTableStart=1;
    D3D12_DESCRIPTOR_RANGE ranges[2]={range,urange};
    D3D12_ROOT_PARAMETER params[2]={0};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges=2; params[0].DescriptorTable.pDescriptorRanges=ranges;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues=3;
    D3D12_STATIC_SAMPLER_DESC sampler={0};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.MaxAnisotropy=1;
    D3D12_ROOT_SIGNATURE_DESC root={0};
    root.NumParameters=2; root.pParameters=params; root.NumStaticSamplers=1; root.pStaticSamplers=&sampler;
    hr=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&signature,NULL);
    if (SUCCEEDED(hr)) {
        hr=dev->lpVtbl->CreateRootSignature(dev,0,signature->lpVtbl->GetBufferPointer(signature),
            signature->lpVtbl->GetBufferSize(signature),&IID_ID3D12RootSignature,(void**)&p->root);
        signature->lpVtbl->Release(signature);
    }
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline={0}; pipeline.pRootSignature=p->root;
        pipeline.CS.pShaderBytecode=code->lpVtbl->GetBufferPointer(code);
        pipeline.CS.BytecodeLength=code->lpVtbl->GetBufferSize(code);
        hr=dev->lpVtbl->CreateComputePipelineState(dev,&pipeline,&IID_ID3D12PipelineState,(void**)&p->pipeline);
    }
    code->lpVtbl->Release(code);
    if (FAILED(hr)) goto fail;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=2; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr=dev->lpVtbl->CreateDescriptorHeap(dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&p->heap);
    if (FAILED(hr)) goto fail;
    p->step=dev->lpVtbl->GetDescriptorHandleIncrementSize(dev,hd.Type);
    D3D12_HEAP_PROPERTIES hp={0}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd={0}; rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width=w; rd.Height=h; rd.DepthOrArraySize=1; rd.MipLevels=1;
    rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count=1;
    rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hr=dev->lpVtbl->CreateCommittedResource(dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&p->output);
    if (FAILED(hr)) goto fail;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    p->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(p->heap,&cpu); cpu.ptr+=p->step;
    dev->lpVtbl->CreateUnorderedAccessView(dev,p->output,NULL,NULL,cpu);
    p->width=w; p->height=h; return 0;
fail:
    fprintf(stderr,"[graphics] presentation setup failed hr=0x%08lX\n",(unsigned long)hr);
    rsx_postprocess_destroy(p); return -1;
}
/* The caller must finish previous uses before rewriting this private table.
 * The renderer already waits for its present fence before the next frame. */
static ID3D12Resource* rsx_postprocess_run(rsx_postprocess* p,ID3D12Device* dev,
    ID3D12GraphicsCommandList* list,ID3D12Resource* source,UINT aa,float sharp,float exposure) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    p->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(p->heap,&cpu);
    p->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(p->heap,&gpu);
    dev->lpVtbl->CreateShaderResourceView(dev,source,NULL,cpu);
    D3D12_RESOURCE_BARRIER bars[2]={0};
    for (unsigned i=0;i<2;++i) {
        bars[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[i].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bars[i].Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    bars[0].Transition.pResource=source; bars[0].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bars[1].Transition.pResource=p->output; bars[1].Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list->lpVtbl->ResourceBarrier(list,2,bars);
    list->lpVtbl->SetDescriptorHeaps(list,1,&p->heap);
    list->lpVtbl->SetComputeRootSignature(list,p->root);
    list->lpVtbl->SetPipelineState(list,p->pipeline);
    list->lpVtbl->SetComputeRootDescriptorTable(list,0,gpu);
    struct { UINT aa; float sharp, exposure; } settings={aa,sharp,exposure};
    list->lpVtbl->SetComputeRoot32BitConstants(list,1,3,&settings,0);
    list->lpVtbl->Dispatch(list,(p->width+7)/8,(p->height+7)/8,1);
    for (unsigned i=0;i<2;++i) {
        bars[i].Transition.StateBefore=bars[i].Transition.StateAfter;
        bars[i].Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    list->lpVtbl->ResourceBarrier(list,2,bars);
    return p->output;
}
#endif
