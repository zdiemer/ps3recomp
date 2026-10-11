/* Exercise the live uploader with synthetic guest bytes and a real WARP GPU. */
#include <assert.h>
#include "../rsx_live_draw.c"
#define OK(x) do { HRESULT hr_ = (x); assert(SUCCEEDED(hr_)); } while (0)
static u8 guest[2048];
uint8_t* vm_base = guest;
uint32_t vm_read32(uint64_t address)
{
    assert(address + 4 <= sizeof(guest));
    return ((u32)guest[address] << 24) | ((u32)guest[address+1] << 16) |
           ((u32)guest[address+2] << 8) | guest[address+3];
}
static const u8* resolve(void* user, u32 location, u32 offset, u32 size)
{
    (void)user; (void)location;
    return (u64)offset + size <= sizeof(guest) ? guest + offset : NULL;
}
static ID3D12Resource* buffer(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = type;
    D3D12_RESOURCE_DESC rd = {0}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 1024 * 1024; rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* result;
    OK(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        NULL,&IID_ID3D12Resource,(void**)&result));
    return result;
}
static void read_subresource(ID3D12Resource* tex, u32 sub, ID3D12Resource* readback,
                              u32 offset, DXGI_FORMAT fmt, u32 width, u32 height)
{
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = sub;
    dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = offset;
    dst.PlacedFootprint.Footprint.Format = fmt;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    g.list->lpVtbl->CopyTextureRegion(g.list,&dst,0,0,0,&src,NULL);
}
static void copy_source(ID3D12Resource* tex)
{
    D3D12_RESOURCE_BARRIER bar = {0}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = tex;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
}
static void cache_tests(void)
{
    u8 bytes[1024]; memset(bytes,0x5A,sizeof(bytes));
    const u64 original=rsx_texture_hash(bytes,sizeof(bytes));
    const u64 xxh_original=rsx_texture_hash_xxh3(bytes,sizeof(bytes));
    for (u32 i=0; i<sizeof(bytes); ++i) {
        bytes[i]^=0x80;
        assert(rsx_texture_hash(bytes,sizeof(bytes))!=original);
        assert(rsx_texture_hash_xxh3(bytes,sizeof(bytes))!=xxh_original);
        bytes[i]^=0x80;
    }
    // Hash tails at a protected boundary: no overread for short/unaligned data.
    u8* boundary=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(boundary); DWORD old; assert(VirtualProtect(boundary+4096,4096,PAGE_NOACCESS,&old));
    memset(boundary,0x73,4096);
    for (u32 n=0; n<=4096; ++n) {
        (void)rsx_texture_hash(boundary+4096-n,n);
        (void)rsx_texture_hash_xxh3(boundary+4096-n,n);
    }
    VirtualFree(boundary,0,MEM_RELEASE);
    rsx_dsp_texture t = {0}; t.format = 0x86; t.width = t.height = 64;
    t.dimension = 2; t.mipmaps = 7; t.remap = 0xAAE4;
    // Populate past the old 1024-entry limit and check all descriptors.
    for (u32 i = 0; i < 2048; ++i) {
        t.offset = i * 128;
        texcache_t* e = &g.textures[i];
        e->offset = t.offset; e->format = t.format;
        e->width = e->height = 64; e->dimension = 2;
        e->mipmaps = 7; e->remap = t.remap;
        texture_cache_link(i, texture_cache_bucket(&t));
    }
    for (u32 i = 0; i < 2048; ++i) {
        t.offset = i * 128;
        assert(texture_cache_find(&t,texture_cache_bucket(&t)) == i);
    }
    t.offset = 10 * 128;
    t.mipmaps = 1; assert(texture_cache_find(&t,texture_cache_bucket(&t)) == UINT32_MAX);
    t.mipmaps = 7; t.dimension = 3;
    assert(texture_cache_find(&t,texture_cache_bucket(&t)) == UINT32_MAX);
    t.dimension = 2;
    texture_cache_unlink(10);
    assert(texture_cache_find(&t,texture_cache_bucket(&t)) == UINT32_MAX);
    t.offset = 900000; g.textures[10].offset = t.offset;
    texture_cache_link(10,texture_cache_bucket(&t));
    assert(texture_cache_find(&t,texture_cache_bucket(&t)) == 10);
    // Force a collision and remove both the middle and head of its chain.
    texture_cache_link(3000,3); texture_cache_link(3001,3); texture_cache_link(3002,3);
    texture_cache_unlink(3001); assert(g.textures[3002].hash_next == 3001);
    texture_cache_unlink(3002); assert(g.texture_buckets[3] == 3001);
    memset(g.textures,0,sizeof(g.textures)); memset(g.texture_buckets,0,sizeof(g.texture_buckets));
    // A failed decode after saturation must be cached too, with one retry per
    // later frame. Otherwise every draw rescans all 4096 LRU candidates.
    g.n_textures=MAX_TEXTURES;
    for (u32 i=0; i<MAX_TEXTURES; ++i) {
        t.offset=i*128; t.format=0x86; t.width=t.height=64; t.dimension=2;
        texcache_t* e=&g.textures[i];
        e->offset=t.offset; e->format=t.format; e->width=e->height=64;
        e->dimension=2; e->mipmaps=t.mipmaps; e->remap=t.remap&0xFFFF;
        e->last_use_serial=i+1;
        texture_cache_link(i,texture_cache_bucket(&t));
    }
    t.offset=0x01E30000; t.format=0xB2; t.width=t.height=2048; t.pitch=4096;
    const u64 failures=g_ld_texture_decode_fail, evictions=g_ld_texture_cache_evictions;
    for (u32 i=0; i<1000; ++i) assert(texture_srv_slot(&t)==SRV_WHITE);
    assert(g_ld_texture_decode_fail==failures+1);
    assert(g_ld_texture_cache_evictions==evictions+1);
    assert(texture_cache_find(&t,texture_cache_bucket(&t))!=UINT32_MAX);
    ++g_ld_frames;
    for (u32 i=0; i<1000; ++i) assert(texture_srv_slot(&t)==SRV_WHITE);
    assert(g_ld_texture_decode_fail==failures+2);
    assert(g_ld_texture_cache_evictions==evictions+1);
    memset(g.textures,0,sizeof(g.textures)); memset(g.texture_buckets,0,sizeof(g.texture_buckets));
    g.n_textures=0; g_ld_frames=0;
    puts("Full texture cache: failed descriptor retained; retry bounded to once per frame");
}
static void fetch_tests(void)
{
    rsx_vertex_fetch_plan p={0}; rsx_vertex_layout_plan_init(&p.layout,7);
    rsx_vertex_ref refs[64]; u8 scalar[64*48], batch[64*48];
    for (u32 i=0; i<sizeof(guest); ++i) guest[i]=(u8)(i*37+19);
    for (u32 i=0; i<64; ++i) { refs[i].vertex_id=(i*7)%8; refs[i].base_index=0x100000; }
    p.guest_ptr=resolve;
    for (u32 a=0; a<3; ++a) {
        p.attr[a].stride=16; p.attr[a].stable_base=guest+a*128;
        p.attr[a].stable_last=7; p.attr[a].desc.offset=a*128;
        p.attr[a].default_value[0]=17; p.attr[a].default_value[3]=29;
    }
    for (u32 type=1; type<=7; ++type) for (u32 size=1; size<=4; ++size)
        for (u32 freq=0; freq<=2; ++freq) {
            for (u32 a=0; a<3; ++a) {
                p.attr[a].desc.type=a==1?0:type;
                p.attr[a].desc.size=size; p.attr[a].desc.frequency=freq;
                p.attr[a].elem_size=type==6?4:size*(type==2?4:type==1||type==3||type==5?2:1);
            }
            rsx_vertex_fetch_plan_prepare(&p,refs,64);
            assert(p.attr[0].stable_first==0 && p.attr[0].stable_last==(freq>1?3:7));
            for (u32 i=0; i<64; ++i) assert(rsx_vertex_fetch_one(&p,&refs[i],scalar+i*48));
            assert(rsx_vertex_fetch_many(&p,refs,64,batch));
            assert(memcmp(scalar,batch,sizeof(batch))==0);
        }
    puts("Batch vertex fetch matches scalar: types, sizes, divisors, disabled attributes, 20-bit wrap");
    {
        rsx_vertex_payload_cache cache={0};
        for (u32 a=0; a<3; ++a) {
            p.attr[a].desc.frequency=0; p.attr[a].desc.type=a==1?0:RSX_VTX_TYPE_FLOAT;
            p.attr[a].desc.size=4; p.attr[a].elem_size=16;
        }
        rsx_vertex_fetch_plan_prepare(&p,refs,64);
        assert(rsx_vertex_fetch_cached(&cache,&p,refs,64,batch));
        assert(cache.misses==1 && cache.hits==0);
        assert(rsx_vertex_fetch_cached(&cache,&p,refs,64,batch));
        assert(cache.hits==1);
        for (u32 i=0; i<128; ++i) {
            guest[i]^=0x80; // Same guest EA, different contents.
            const u64 misses=cache.misses;
            assert(rsx_vertex_fetch_cached(&cache,&p,refs,64,batch));
            assert(cache.misses==misses+1);
            for (u32 j=0; j<64; ++j) assert(rsx_vertex_fetch_one(&p,&refs[j],scalar+j*48));
            assert(!memcmp(scalar,batch,sizeof(batch)));
            guest[i]^=0x80;
        }
        // Changing references, format, defaults, or fallback divisors must also
        // agree with the independent scalar resolver, never reuse stale output.
        for (u32 trial=0; trial<128; ++trial) {
            refs[trial%64].vertex_id=trial%8;
            p.attr[0].desc.type=1+trial%7;
            p.attr[0].elem_size=rsx_vertex_attrib_size(p.attr[0].desc.type,4);
            p.attr[1].default_value[0]=(float)trial;
            p.attr[2].desc.frequency=trial%3;
            rsx_vertex_fetch_plan_prepare(&p,refs,64);
            assert(rsx_vertex_fetch_cached(&cache,&p,refs,64,batch));
            for (u32 j=0; j<64; ++j) assert(rsx_vertex_fetch_one(&p,&refs[j],scalar+j*48));
            assert(!memcmp(scalar,batch,sizeof(batch)));
        }
        assert(cache.bytes<=128ull*1024*1024);
        rsx_vertex_payload_cache_clear(&cache);
        assert(!cache.entries && !cache.bytes);
        puts("Vertex cache: full-span mutation, reused EA, references, formats, defaults, divisors match scalar");
    }
    {
        rsx_vertex_fetch_plan exact={0}; rsx_vertex_ref ref={0};
        rsx_vertex_layout_plan_init(&exact.layout,1);
        exact.attr[0].desc.type=RSX_VTX_TYPE_SINT16;
        exact.attr[0].stable_base=guest;
        for (u32 size=1; size<=4; ++size) {
            exact.attr[0].desc.size=size;
            for (u32 word=0; word<=65535; ++word) {
                for (u32 lane=0; lane<size; ++lane) {
                    const u16 v=(u16)(word+lane*17011);
                    guest[lane*2]=(u8)(v>>8); guest[lane*2+1]=(u8)v;
                }
                float expected[4], actual[4];
                assert(rsx_vertex_decode_element(RSX_VTX_TYPE_SINT16,size,guest,expected));
                assert(rsx_vertex_fetch_many(&exact,&ref,1,(u8*)actual));
                assert(memcmp(actual,expected,sizeof(actual))==0);
            }
        }
        u8* page=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        assert(page); DWORD old;
        assert(VirtualProtect(page+4096,4096,PAGE_NOACCESS,&old));
        for (u32 size=1; size<=4; ++size) {
            exact.attr[0].desc.size=size;
            exact.attr[0].stable_base=page+4096-size*2;
            memset((void*)exact.attr[0].stable_base,0xA5,size*2);
            float actual[4];
            assert(rsx_vertex_fetch_many(&exact,&ref,1,(u8*)actual));
        }
        VirtualFree(page,0,MEM_RELEASE);
        puts("SINT16 batch: every signed value/lane matches; protected tails do not overread");
    }
    rsx_vertex_remap remap={0};
    for (u32 trial=0; trial<256; ++trial) {
        rsx_vertex_ref input[64], actual[64], expected[64]; u32 map[64], used=0;
        for (u32 i=0; i<64; ++i) {
            input[i].vertex_id=trial%4==0?i:trial%4==1?(i*13)%17:trial%4==2?(i*13)%17*100003:(i*13)%17;
            input[i].base_index=trial%4==3?i%3:17;
            u32 j; for (j=0; j<used; ++j) if (memcmp(&expected[j],&input[i],sizeof(input[i]))==0) break;
            if (j==used) expected[used++]=input[i]; map[i]=j;
        }
        memcpy(actual,input,sizeof(input)); u32 unique=0;
        assert(rsx_vertex_remap_build_ex(&remap,actual,64,&unique,(trial/4)%2));
        assert(unique==used && memcmp(actual,expected,used*sizeof(*actual))==0);
        for (u32 i=0; i<64; ++i) assert(rsx_vertex_remap_index(&remap,i)==map[i]);
        if (trial==64) remap.generation=UINT_MAX; // generation wrap after dense/sparse reuse
    }
    rsx_vertex_remap_destroy(&remap);
    puts("Vertex remap matches ordered reference: ascending/dense/sparse/mixed bases and generation wrap");
}
static void reference_batch_tests(void)
{
    rsx_vertex_layout_plan layout; rsx_vertex_layout_plan_init(&layout,0);
    g.guest_ptr=resolve;
    batch_t arrays[2]={{UINT_MAX-2,5},{7,4}}, indices[2]={{0,9},{3,4}};
    for (unsigned wide=0; wide<2; ++wide) for (unsigned restart=0; restart<2; ++restart)
    for (unsigned flags=0; flags<4; ++flags) {
        memset(&g.rsx,0,sizeof(g.rsx));
        g.rsx.regs[0x181C/4]=128;
        g.rsx.regs[0x1820/4]=wide?0:0x10;
        g.rsx.regs[0x173C/4]=23;
        g.rsx.regs[0x1DAC/4]=restart;
        g.rsx.regs[0x1DB0/4]=wide?UINT_MAX:65535;
        const u32 input[]={7,2,2,wide?UINT_MAX:65535,3,8,wide?UINT_MAX:65535,5,7};
        for (u32 i=0; i<9; ++i) for (unsigned b=0; b<(wide?4:2); ++b)
            guest[128+i*(wide?4:2)+b]=(u8)(input[i]>>((wide?3:1)-b)*8);
        rsx_vertex_ref expected[32], unique[32]; u32 map[32],cuts[8];
        u32 count=0, used=0, ncuts=0;
        for (u32 b=0; b<2; ++b) for (u32 i=0; i<arrays[b].count; ++i)
            expected[count++]=(rsx_vertex_ref){arrays[b].first+i,0};
        for (u32 b=0; b<2; ++b) for (u32 i=0; i<indices[b].count; ++i) {
            const u32 id=input[indices[b].first+i];
            if (restart && id==(wide?UINT_MAX:65535)) { cuts[ncuts++]=count; continue; }
            expected[count++]=(rsx_vertex_ref){id,23};
        }
        for (u32 i=0; i<count; ++i) {
            u32 j=0; for (; j<used; ++j) if (!memcmp(&expected[i],&unique[j],sizeof(*unique))) break;
            if (j==used) unique[used++]=expected[i]; map[i]=j;
        }
        dc_reset(); dc.arr=arrays; dc.n_arr=2; dc.idx=indices; dc.n_idx=2;
        g_ld_cpu_work=flags;
        fetch_batches_hoisted(&layout,1);
        assert(dc.fetch_ok && dc.n_source_refs==count && dc.n_refs==used);
        assert(!memcmp(dc.refs,unique,used*sizeof(*unique)));
        assert(dc.n_cuts==ncuts && !memcmp(dc.cuts,cuts,ncuts*sizeof(*cuts)));
        for (u32 i=0; i<count; ++i) assert(rsx_vertex_remap_index(&dc.ref_remap,i)==map[i]);
        /* A bad source range remains fatal in both paths. */
        dc_reset(); dc.n_idx=1; indices[0].first=100000;
        fetch_batches_hoisted(&layout,1); assert(!dc.fetch_ok); indices[0].first=0;
    }
    dc.arr=dc.idx=NULL; dc_reset(); g_ld_cpu_work=0;
    assert(!reserve_compact_refs(UINT_MAX));
    puts("Reference batches: bulk/legacy agree with ordered oracle, 16/32-bit indices, restart cuts, wrap and bad ranges");
    rsx_dsp_texture t={0}; t.dimension=2; t.width=t.height=1; t.format=0xBE; t.pitch=4;
    for (unsigned old=0; old<2; ++old) for (unsigned next=0; next<2; ++next)
    for (unsigned mutation=0; mutation<2; ++mutation) {
        texcache_t e={0}; int readable;
        e.content_hash=texture_content_hash_kind(&t,&readable,old); e.content_hash_kind=old;
        assert(readable); if (mutation) guest[3]^=0x80;
        g_ld_cpu_work=next*4;
        const u64 hash=texture_content_hash(&t,&readable);
        assert(texture_cached_content_changed(&e,&t,hash,&readable)==mutation);
        if (!mutation) assert(e.content_hash==hash && e.content_hash_kind==next);
        if (mutation) guest[3]^=0x80;
    }
    g_ld_cpu_work=0;
    puts("Texture hash switching: unchanged content stays cached; mutation remains visible in either algorithm");
}
static void bias_tests(void)
{
    // Coplanar overlay: LESS fails without bias, negative bias brings it forward.
    D3D12_HEAP_PROPERTIES hp={0}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd={0}; rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width=rd.Height=16; rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;
    rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *color,*depth;
    OK(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&color));
    rd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    OK(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,NULL,&IID_ID3D12Resource,(void**)&depth));
    g.depth=depth;
    ID3D12DescriptorHeap *rtv,*dsv;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&rtv));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&dsv));
    D3D12_CPU_DESCRIPTOR_HANDLE rh,dh;
    rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv,&rh);
    dsv->lpVtbl->GetCPUDescriptorHandleForHeapStart(dsv,&dh);
    g.dev->lpVtbl->CreateRenderTargetView(g.dev,color,NULL,rh);
    g.dev->lpVtbl->CreateDepthStencilView(g.dev,depth,NULL,dh);
    ID3DBlob *root_blob,*vs,*red,*green;
    D3D12_ROOT_SIGNATURE_DESC root={0};
    OK(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,NULL));
    ID3D12RootSignature* sig;
    OK(g.dev->lpVtbl->CreateRootSignature(g.dev,0,root_blob->lpVtbl->GetBufferPointer(root_blob),
        root_blob->lpVtbl->GetBufferSize(root_blob),&IID_ID3D12RootSignature,(void**)&sig));
    const char* v="float4 main(uint id:SV_VertexID):SV_Position { float2 p=float2((id<<1)&2,id&2); return float4(p*2-1,0.5,1); }";
    const char* r="float4 main():SV_Target { return float4(1,0,0,1); }";
    const char* b="float4 main():SV_Target { return float4(0,1,0,1); }";
    OK(D3DCompile(v,strlen(v),NULL,NULL,NULL,"main","vs_5_0",0,0,&vs,NULL));
    OK(D3DCompile(r,strlen(r),NULL,NULL,NULL,"main","ps_5_0",0,0,&red,NULL));
    OK(D3DCompile(b,strlen(b),NULL,NULL,NULL,"main","ps_5_0",0,0,&green,NULL));
    ID3D12Resource* rb=buffer(D3D12_HEAP_TYPE_READBACK);
    for (int mode=0; mode<6; ++mode) {
        if (mode==3) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS16 feature={0};
            if (FAILED(g.dev->lpVtbl->CheckFeatureSupport(g.dev,D3D12_FEATURE_D3D12_OPTIONS16,
                    &feature,sizeof(feature))) || !feature.DynamicDepthBiasSupported) break;
            OK(g.list->lpVtbl->QueryInterface(g.list,&IID_ID3D12GraphicsCommandList9,(void**)&g_ld_bias_list));
        }
        OK(g.alloc->lpVtbl->Reset(g.alloc)); OK(g.list->lpVtbl->Reset(g.list,g.alloc,NULL));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={0}; pd.pRootSignature=sig;
        pd.VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs); pd.VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs);
        pd.PS.pShaderBytecode=red->lpVtbl->GetBufferPointer(red); pd.PS.BytecodeLength=red->lpVtbl->GetBufferSize(red);
        pd.SampleMask=UINT_MAX; pd.SampleDesc.Count=1;
        pd.NumRenderTargets=1; pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        render_state_t state={0}; state.color_mask=0x01010101; state.depth_test=state.depth_write=1; state.depth_func=0x201;
        apply_render_state(&pd,&state);
        ID3D12PipelineState *base,*overlay;
        OK(g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev,&pd,&IID_ID3D12PipelineState,(void**)&base));
        state.depth_write=0; float units=mode%3==1?-64.0f:mode%3==2?64.0f:0;
        memcpy(&state.bias_units,&units,4); apply_render_state(&pd,&state);
        pd.PS.pShaderBytecode=green->lpVtbl->GetBufferPointer(green); pd.PS.BytecodeLength=green->lpVtbl->GetBufferSize(green);
        OK(g.dev->lpVtbl->CreateGraphicsPipelineState(g.dev,&pd,&IID_ID3D12PipelineState,(void**)&overlay));
        D3D12_VIEWPORT vp={0,0,16,16,0,1}; D3D12_RECT sc={0,0,16,16}; const float black[4]={0};
        g.list->lpVtbl->RSSetViewports(g.list,1,&vp); g.list->lpVtbl->RSSetScissorRects(g.list,1,&sc);
        g.list->lpVtbl->OMSetRenderTargets(g.list,1,&rh,FALSE,&dh);
        g.list->lpVtbl->ClearRenderTargetView(g.list,rh,black,0,NULL);
        g.list->lpVtbl->ClearDepthStencilView(g.list,dh,D3D12_CLEAR_FLAG_DEPTH,1,0,0,NULL);
        g.list->lpVtbl->SetGraphicsRootSignature(g.list,sig);
        g.list->lpVtbl->IASetPrimitiveTopology(g.list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.list->lpVtbl->SetPipelineState(g.list,base); g.list->lpVtbl->DrawInstanced(g.list,3,1,0,0);
        g.list->lpVtbl->SetPipelineState(g.list,overlay);
        if (g_ld_bias_list) g_ld_bias_list->lpVtbl->RSSetDepthBias(g_ld_bias_list,units,0,0);
        g.list->lpVtbl->DrawInstanced(g.list,3,1,0,0);
        D3D12_RESOURCE_BARRIER bar={0}; bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource=color; bar.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; bar.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
        read_subresource(color,0,rb,0,DXGI_FORMAT_R8G8B8A8_UNORM,16,16);
        bar.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; bar.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
        OK(g.list->lpVtbl->Close(g.list)); ID3D12CommandList* lists[]={(ID3D12CommandList*)g.list};
        g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);
        OK(g.queue->lpVtbl->Signal(g.queue,g.fence,mode+2));
        OK(g.fence->lpVtbl->SetEventOnCompletion(g.fence,mode+2,g.fence_event));
        assert(WaitForSingleObject(g.fence_event,10000)==WAIT_OBJECT_0);
        u8* bytes; OK(rb->lpVtbl->Map(rb,0,NULL,(void**)&bytes));
        u8* pixel=bytes+8*256+8*4;
        assert(pixel[0]==(mode%3==1?0:255) && pixel[1]==(mode%3==1?255:0));
        rb->lpVtbl->Unmap(rb,0,NULL); overlay->lpVtbl->Release(overlay); base->lpVtbl->Release(base);
    }
    if (g_ld_bias_list) { g_ld_bias_list->lpVtbl->Release(g_ld_bias_list); g_ld_bias_list=NULL; }
    rb->lpVtbl->Release(rb); sig->lpVtbl->Release(sig); root_blob->lpVtbl->Release(root_blob);
    vs->lpVtbl->Release(vs); red->lpVtbl->Release(red); green->lpVtbl->Release(green);
    color->lpVtbl->Release(color); depth->lpVtbl->Release(depth); g.depth=NULL;
    rtv->lpVtbl->Release(rtv); dsv->lpVtbl->Release(dsv);
    puts("Coplanar decal GPU readback: zero/positive bias occluded, negative bias visible");
}
int main(void)
{
    cache_tests();
    fetch_tests();
    reference_batch_tests();
    IDXGIFactory4* factory; IDXGIAdapter* warp;
    OK(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
    OK(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
    OK(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&g.dev));
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    OK(g.dev->lpVtbl->CreateCommandQueue(g.dev,&qd,&IID_ID3D12CommandQueue,(void**)&g.queue));
    OK(g.dev->lpVtbl->CreateCommandAllocator(g.dev,D3D12_COMMAND_LIST_TYPE_DIRECT,&IID_ID3D12CommandAllocator,(void**)&g.alloc));
    OK(g.dev->lpVtbl->CreateCommandList(g.dev,0,D3D12_COMMAND_LIST_TYPE_DIRECT,g.alloc,NULL,&IID_ID3D12GraphicsCommandList,(void**)&g.list));
    OK(g.dev->lpVtbl->CreateFence(g.dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&g.fence));
    g.fence_event = CreateEvent(NULL,FALSE,FALSE,NULL); assert(g.fence_event);
    g.upload = buffer(D3D12_HEAP_TYPE_UPLOAD);
    OK(g.upload->lpVtbl->Map(g.upload,0,NULL,(void**)&g.upload_mapped));
    g.guest_ptr = resolve;
    ID3D12Resource* readback = buffer(D3D12_HEAP_TYPE_READBACK);

    rsx_dsp_texture t = {0}; t.dimension = 2; t.width = t.height = 8;
    t.format = 0x86; t.mipmaps = 4; t.cubemap = 1;
    assert(texture_source_span(&t) == 6 * 128); // 8,4,2,1 BC mip chain + padding
    for (u32 face = 0; face < 6; ++face) {
        memset(guest + face * 128,0x11 + face,56);
        memset(guest + face * 128 + 56,0xEE,72); // poison face padding
    }
    ID3D12Resource* cube = decode_guest_texture(&t,0xAAE4); assert(cube);
    D3D12_RESOURCE_DESC desc; cube->lpVtbl->GetDesc(cube,&desc);
    assert(desc.DepthOrArraySize == 6 && desc.MipLevels == 2);
    copy_source(cube);
    for (u32 face = 0; face < 6; ++face)
        read_subresource(cube,face*desc.MipLevels,readback,face*512,DXGI_FORMAT_BC1_UNORM,8,8);

    memset(&t,0,sizeof(t)); t.dimension = 2; t.width = t.height = t.mipmaps = 1;
    t.format = 0xBE; t.pitch = 4; t.offset = 1024;
    guest[1024] = 0; guest[1025] = 0x35; guest[1026] = 0x79; guest[1027] = 0xB2;
    assert(texture_source_span(&t) == 4);
    ID3D12Resource* rgb = decode_guest_texture(&t,0xAAE4); assert(rgb);
    copy_source(rgb);
    read_subresource(rgb,0,readback,4096,DXGI_FORMAT_R8G8B8A8_UNORM,1,1);
    OK(g.list->lpVtbl->Close(g.list));
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);
    OK(g.queue->lpVtbl->Signal(g.queue,g.fence,1));
    OK(g.fence->lpVtbl->SetEventOnCompletion(g.fence,1,g.fence_event));
    assert(WaitForSingleObject(g.fence_event,10000) == WAIT_OBJECT_0);
    u8* bytes; OK(readback->lpVtbl->Map(readback,0,NULL,(void**)&bytes));
    for (u32 face = 0; face < 6; ++face)
        for (u32 b = 0; b < 16; ++b) assert(bytes[face*512+b] == 0x11 + face);
    assert(bytes[4096] == 0x35 && bytes[4097] == 0x79 && bytes[4098] == 0xB2 && bytes[4099] == 255);
    readback->lpVtbl->Unmap(readback,0,NULL);
    bias_tests();
    rgb->lpVtbl->Release(rgb); cube->lpVtbl->Release(cube); readback->lpVtbl->Release(readback);
    g.upload->lpVtbl->Unmap(g.upload,0,NULL); g.upload->lpVtbl->Release(g.upload);
    g.list->lpVtbl->Release(g.list); g.alloc->lpVtbl->Release(g.alloc);
    g.queue->lpVtbl->Release(g.queue); g.fence->lpVtbl->Release(g.fence);
    CloseHandle(g.fence_event); g.dev->lpVtbl->Release(g.dev);
    warp->lpVtbl->Release(warp); factory->lpVtbl->Release(factory);
    puts("Live textures: cache identity/collisions, six padded BC faces and opaque D8R8G8B8 GPU readback passed.");
}
