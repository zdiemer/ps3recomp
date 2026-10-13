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
/* Copy the complete depth plane: partial depth-resource copies are illegal. */
static u32 read_depth(ID3D12Resource* tex, ID3D12Resource* rb, u32 offset)
{
    D3D12_RESOURCE_DESC desc; tex->lpVtbl->GetDesc(tex,&desc);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
    src.pResource=tex; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=rb; dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    g.dev->lpVtbl->GetCopyableFootprints(g.dev,&desc,0,1,offset,&dst.PlacedFootprint,NULL,NULL,NULL);
    D3D12_RESOURCE_BARRIER bar={0}; bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource=tex; bar.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore=D3D12_RESOURCE_STATE_DEPTH_WRITE;
    bar.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
    g.list->lpVtbl->CopyTextureRegion(g.list,&dst,0,0,0,&src,NULL);
    bar.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter=D3D12_RESOURCE_STATE_DEPTH_WRITE;
    g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
    return dst.PlacedFootprint.Footprint.RowPitch;
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
            assert(rsx_vertex_fetch_many_ex(&p,refs,64,batch,0));
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
    {
        /* Raw F32 bits include NaN payloads, signed zeros, infinities and
         * subnormals: this path must perform no floating-point arithmetic. */
        static const u32 special[] = {0,0x80000000,0x3F800000,0xBF800000,
            0x7F800000,0xFF800000,0x7FC12345,0x7F812345,0xFFC12345,0xFF812345,
            1,0x80000001,0x007FFFFF,0x00800000,0x7F7FFFFF,0xFF7FFFFF};
        const u32 types[] = {RSX_VTX_TYPE_FLOAT,RSX_VTX_TYPE_UNORM8,RSX_VTX_TYPE_UINT8};
        rsx_vertex_fetch_plan exact={0}; rsx_vertex_ref ref={0};
        rsx_vertex_layout_plan_init(&exact.layout,1);
        u32 random=0xC34193DA;
        for (u32 t=0; t<3; ++t) for (u32 size=1; size<=4; ++size) {
            exact.attr[0].desc.type=types[t]; exact.attr[0].desc.size=size;
            for (u32 trial=0; trial<4096; ++trial) {
                u8* source=guest+(trial%16); // Include unaligned reads.
                for (u32 lane=0; lane<4; ++lane) {
                    random=random*1664525u+1013904223u;
                    const u32 word=trial<16?special[(trial+lane)%16]:random;
                    source[lane*4]=(u8)(word>>24); source[lane*4+1]=(u8)(word>>16);
                    source[lane*4+2]=(u8)(word>>8); source[lane*4+3]=(u8)word;
                }
                if (types[t]!=RSX_VTX_TYPE_FLOAT)
                    for (u32 lane=0; lane<4; ++lane) source[lane]=(u8)(trial+lane*73);
                exact.attr[0].stable_base=source;
                float expected[4], actual[4];
                assert(rsx_vertex_decode_element(types[t],size,source,expected));
                assert(rsx_vertex_fetch_many_ex(&exact,&ref,1,(u8*)actual,1));
                assert(memcmp(expected,actual,sizeof(actual))==0);
            }
        }
        u8* page=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        assert(page); DWORD old;
        assert(VirtualProtect(page+4096,4096,PAGE_NOACCESS,&old));
        for (u32 t=0; t<3; ++t) for (u32 size=1; size<=4; ++size) {
            const u32 bytes=size*(types[t]==RSX_VTX_TYPE_FLOAT?4:1);
            exact.attr[0].desc.type=types[t]; exact.attr[0].desc.size=size;
            exact.attr[0].stable_base=page+4096-bytes;
            memset((void*)exact.attr[0].stable_base,0xF3,bytes);
            /* Destination ends at a protected page as well. */
            float expected[4];
            assert(rsx_vertex_decode_element(types[t],size,exact.attr[0].stable_base,expected));
            assert(rsx_vertex_fetch_many_ex(&exact,&ref,1,page+4096-16,1));
            assert(!memcmp(expected,page+4096-16,16));
        }
        VirtualFree(page,0,MEM_RELEASE);
        /* A reused prepared plan whose references escape its stable interval
         * must resolve from guest memory, including 20-bit wrapping. */
        exact.guest_ptr=resolve; exact.attr[0].stable_base=guest;
        exact.attr[0].desc.type=RSX_VTX_TYPE_FLOAT; exact.attr[0].desc.size=4;
        exact.attr[0].stride=16; exact.attr[0].elem_size=16;
        for (u32 i=0; i<16; ++i) {
            ref.vertex_id=i+1; ref.base_index=0x100000;
            float expected[4],actual[4];
            assert(rsx_vertex_fetch_one(&exact,&ref,(u8*)expected));
            assert(rsx_vertex_fetch_many_ex(&exact,&ref,1,(u8*)actual,1));
            assert(!memcmp(expected,actual,sizeof(actual)));
        }
        puts("Specialized F32/byte batches: 49152 exact comparisons, protected tails, reused plans");
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
    rsx_dispatch_init(&g.rsx, NULL);
    assert(rsx_dsp_reg(&g.rsx, 0x1D78) == 1u);
    render_state_t decoded;
    decode_render_state(&decoded);
    assert(decoded.depth_clip == 1u);
    const u64 clip_key = ld_hash_structural_render_state(&decoded, 123u);
    rsx_dispatch_method(&g.rsx, 0x1D78, 0u);
    decode_render_state(&decoded);
    assert(decoded.depth_clip == 0u);
    assert(ld_hash_structural_render_state(&decoded, 123u) != clip_key);
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
    for (int mode=0; mode<14; ++mode) {
        if (mode==3) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS16 feature={0};
            if (FAILED(g.dev->lpVtbl->CheckFeatureSupport(g.dev,D3D12_FEATURE_D3D12_OPTIONS16,
                    &feature,sizeof(feature))) || !feature.DynamicDepthBiasSupported) { mode=5; continue; }
            OK(g.list->lpVtbl->QueryInterface(g.list,&IID_ID3D12GraphicsCommandList9,(void**)&g_ld_bias_list));
        }
        if (mode==6 && g_ld_bias_list) {
            g_ld_bias_list->lpVtbl->Release(g_ld_bias_list); g_ld_bias_list=NULL;
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
        state.depth_write=0; float units=mode<6 ? (mode%3==1?-64.0f:mode%3==2?64.0f:0) : 0;
        ID3DBlob* clip_vs = NULL;
        if (mode>=6) {
            const char* z = mode<8 ? "-0.25" : mode<10 ? "1.25" : "0.25";
            char shader[256];
            snprintf(shader,sizeof(shader),
                "float4 main(uint id:SV_VertexID):SV_Position { float2 p=float2((id<<1)&2,id&2); return float4(p*2-1,%s,%s); }",
                z, mode>=12 ? "-1" : "1");
            OK(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"main","vs_5_0",0,0,&clip_vs,NULL));
            pd.VS.pShaderBytecode=clip_vs->lpVtbl->GetBufferPointer(clip_vs);
            pd.VS.BytecodeLength=clip_vs->lpVtbl->GetBufferSize(clip_vs);
            rsx_dispatch_method(&g.rsx,0x1D78,(u32)mode&1u);
            decode_render_state(&decoded);
            state.depth_clip=decoded.depth_clip;
            state.depth_func=0x207; /* isolate clipping from depth rejection */
        }
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
        const int overlay_visible = mode<6 ? mode%3==1 :
            mode<10 ? !(mode&1) : mode<12;
        assert(pixel[0]==(overlay_visible?0:255) && pixel[1]==(overlay_visible?255:0));
        rb->lpVtbl->Unmap(rb,0,NULL); overlay->lpVtbl->Release(overlay); base->lpVtbl->Release(base);
        if (clip_vs) clip_vs->lpVtbl->Release(clip_vs);
    }
    if (g_ld_bias_list) { g_ld_bias_list->lpVtbl->Release(g_ld_bias_list); g_ld_bias_list=NULL; }
    rb->lpVtbl->Release(rb); sig->lpVtbl->Release(sig); root_blob->lpVtbl->Release(root_blob);
    vs->lpVtbl->Release(vs); red->lpVtbl->Release(red); green->lpVtbl->Release(green);
    color->lpVtbl->Release(color); depth->lpVtbl->Release(depth); g.depth=NULL;
    rtv->lpVtbl->Release(rtv); dsv->lpVtbl->Release(dsv);
    puts("Coplanar decal GPU readback: zero/positive bias occluded, negative bias visible");
    puts("Guest depth clip reset/state/key and GPU readback: near/far clipped only when requested; valid depths preserved; negative W always clipped");
}
static void resolution_tests(void)
{
    rsx_render_resolution r;
    const char* invalid[] = {"0x720","1280x0","1920","1920x1080junk","-1x720",
        "8193x720","999999999999x720","1920X1080"," 1920x1080","1920x1080 "};
    for (u32 i=0;i<sizeof(invalid)/sizeof(*invalid);++i)
        assert(rsx_render_resolution_parse(&r,1280,720,invalid[i])<0);
    assert(rsx_render_resolution_parse(&r,1280,720,NULL)==0 && r.host_w==1280 && r.host_h==720);
    assert(rsx_render_resolution_parse(&r,1280,720,"1777x997")==0);
    assert(rsx_render_extent(1280,1280,r.host_w)==1777);
    assert(rsx_render_extent(720,720,r.host_h)==997);
    r.reference_w=1024; r.reference_h=576;
    u32 rw,rh;
    rsx_render_surface_extent(&r,1024,576,&rw,&rh); assert(rw==1777 && rh==997);
    rsx_render_surface_extent(&r,1280,720,&rw,&rh); assert(rw==1777 && rh==997);
    rsx_render_surface_extent(&r,512,288,&rw,&rh); assert(rw==889 && rh==499);
    rsx_render_resolution_parse(&r,1280,720,NULL); r.reference_w=1024; r.reference_h=576;
    rsx_render_surface_extent(&r,1024,576,&rw,&rh); assert(rw==1024 && rh==576);
    for (u32 i=0;i<=1280;++i) assert(rsx_render_edge(i,1280,1777)<=1777);
    D3D12_DESCRIPTOR_HEAP_DESC hd={0}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=LD_SWAP_BUFFERS+MAX_SURFACES;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&g.rtv_heap));
    g.rtv_step=g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev,hd.Type);
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=SRV_CPU_HEAP_SLOTS;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&g.srv_cpu_heap));
    g.srv_step=g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev,hd.Type);
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors=1+MAX_SURFACES;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&g.dsv_heap));
    g.dsv_step=g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev,hd.Type);
    g.width=g.height=8; g.ready=1;
    const char* sizes[]={"8x8","12x12","16x16","11x13","12x12","native"};
    ID3D12Resource* rb=buffer(D3D12_HEAP_TYPE_READBACK);
    for (u32 mode=0;mode<6;++mode) {
        assert(rsx_render_resolution_parse(&g_ld_resolution,8,8,sizes[mode])==0);
        g_ld_resolution.reference_w=g_ld_resolution.reference_h=6;
        g_ld_resolution.shadow_guest=4;
        g_ld_resolution.shadow_host=mode==4 ? 3 : mode==5 ? 10 : 0;
        OK(g.alloc->lpVtbl->Reset(g.alloc)); OK(g.list->lpVtbl->Reset(g.list,g.alloc,NULL));
        g.n_surfaces=g.n_zdepths=0;
        u32 source=surface_get(0,0x1000,8,8,DXGI_FORMAT_R8G8B8A8_UNORM);
        u32 dest=surface_get(0,0x2000,8,8,DXGI_FORMAT_R8G8B8A8_UNORM);
        assert(source<MAX_SURFACES && dest<MAX_SURFACES);
        assert(surface_get(0,0x1000,8,8,DXGI_FORMAT_R8G8B8A8_UNORM)==source);
        const surface_t* sf=&g.surfaces[source];
        D3D12_RESOURCE_DESC desc; sf->tex->lpVtbl->GetDesc(sf->tex,&desc);
        assert(desc.Width==g_ld_resolution.host_w && desc.Height==g_ld_resolution.host_h);
        u32 scene=surface_get(0,0x4000,6,6,DXGI_FORMAT_R8G8B8A8_UNORM);
        assert(g.surfaces[scene].w==(mode==5 ? 6 : desc.Width) &&
            g.surfaces[scene].h==(mode==5 ? 6 : desc.Height));
        u32 atlas_w,atlas_h;
        rsx_render_surface_extent(&g_ld_resolution,4,4,&atlas_w,&atlas_h);
        u32 atlas=surface_get(0,0x5000,4,4,DXGI_FORMAT_R8G8B8A8_UNORM);
        assert(g.surfaces[atlas].w==atlas_w && g.surfaces[atlas].h==atlas_h);
        u32 atlas_z=zdepth_get(0,0x6000,4,4); assert(atlas_z);
        assert(g.zdepths[atlas_z-1].snapshot_w==atlas_w && g.zdepths[atlas_z-1].snapshot_h==atlas_h);
        u32 ox,oy; assert(surface_containing(0,0x1000+32+4,32,7,7,&ox,&oy)==(int)source && ox==1 && oy==1);
        assert(surface_containing(0,0x1000+32+4,32,8,8,&ox,&oy)<0);
        u32 zslot=zdepth_get(0,0x3000,8,8); assert(zslot);
        assert(g.zdepths[zslot-1].snapshot_w==desc.Width && g.zdepths[zslot-1].snapshot_h==desc.Height);
        rsx_dispatch_init(&g.rsx,NULL);
        rsx_dispatch_method(&g.rsx,0x0200,4u<<16);
        rsx_dispatch_method(&g.rsx,0x0204,4u<<16);
        rsx_dispatch_method(&g.rsx,0x0208,8);
        rsx_dispatch_method(&g.rsx,0x020C,16);
        rsx_dispatch_method(&g.rsx,0x0210,0x5000);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_HORIZONTAL,4u<<16);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_VERTICAL,4u<<16);
        rsx_dispatch_method(&g.rsx,0x1D90,0xFF000000); sink_clear(NULL,&g.rsx,0xF0);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_HORIZONTAL,(2u<<16)|1u);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_VERTICAL,(2u<<16)|1u);
        rsx_dispatch_method(&g.rsx,0x1D90,0xFF00FF00); sink_clear(NULL,&g.rsx,0xF0);
        D3D12_RESOURCE_BARRIER atlas_bar={0}; atlas_bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        atlas_bar.Transition.pResource=g.surfaces[atlas].tex;
        atlas_bar.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        atlas_bar.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
        atlas_bar.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list,1,&atlas_bar);
        read_subresource(g.surfaces[atlas].tex,0,rb,65536,DXGI_FORMAT_R8G8B8A8_UNORM,atlas_w,atlas_h);
        /* Actual opening-scene Z16 register payload, including the old
         * interpretation as a negative control. Verify scissor scaling too. */
        g.depth=g.zdepths[atlas_z-1].tex;
        rsx_dispatch_method(&g.rsx,0x0214,0x6000);
        rsx_dispatch_method(&g.rsx,0x0208,8|(1u<<5));
        rsx_dispatch_method(&g.rsx,M_SCISSOR_HORIZONTAL,4u<<16);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_VERTICAL,4u<<16);
        rsx_dispatch_method(&g.rsx,M_ZSTENCIL_CLEAR,0x0000FFFF);
        g_ld_depth_clear=0; sink_clear(NULL,&g.rsx,RSX_CLEAR_DEPTH);
        const u32 old_depth_pitch=read_depth(g.depth,rb,131072);
        g_ld_depth_clear=1; sink_clear(NULL,&g.rsx,RSX_CLEAR_DEPTH);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_HORIZONTAL,(2u<<16)|1u);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_VERTICAL,(2u<<16)|1u);
        rsx_dispatch_method(&g.rsx,M_ZSTENCIL_CLEAR,0xABCD8000);
        sink_clear(NULL,&g.rsx,RSX_CLEAR_DEPTH);
        rsx_dispatch_method(&g.rsx,M_ZSTENCIL_CLEAR,0x1234567A);
        sink_clear(NULL,&g.rsx,RSX_CLEAR_STENCIL); // must preserve depth
        const u32 new_depth_pitch=read_depth(g.depth,rb,196608);
        rsx_dispatch_method(&g.rsx,0x0194,0xFEED0000);
        rsx_dispatch_method(&g.rsx,0x0200,8u<<16); // surface clip
        rsx_dispatch_method(&g.rsx,0x0204,8u<<16);
        rsx_dispatch_method(&g.rsx,0x0208,8); // A8R8G8B8
        rsx_dispatch_method(&g.rsx,0x020C,32);
        rsx_dispatch_method(&g.rsx,0x0210,0x1000);
        rsx_dispatch_method(&g.rsx,0x0214,0x3000);
        rsx_dispatch_method(&g.rsx,0x0208,8|(2u<<5));
        rsx_dispatch_method(&g.rsx,M_SCISSOR_HORIZONTAL,8u<<16);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_VERTICAL,8u<<16);
        rsx_dispatch_method(&g.rsx,M_ZSTENCIL_CLEAR,0xFFFFFF5A);
        sink_clear(NULL,&g.rsx,RSX_CLEAR_DEPTH|RSX_CLEAR_STENCIL);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_HORIZONTAL,(3u<<16)|2u);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_VERTICAL,(4u<<16)|1u);
        rsx_dispatch_method(&g.rsx,M_ZSTENCIL_CLEAR,0x400000A5);
        sink_clear(NULL,&g.rsx,RSX_CLEAR_DEPTH|RSX_CLEAR_STENCIL);
        const u32 scene_depth_pitch=read_depth(g.zdepths[zslot-1].tex,rb,262144);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_HORIZONTAL,8u<<16);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_VERTICAL,8u<<16);
        rsx_dispatch_method(&g.rsx,0x1D90,0xFF0000FF); // blue
        sink_clear(NULL,&g.rsx,0xF0);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_HORIZONTAL,(3u<<16)|2u);
        rsx_dispatch_method(&g.rsx,M_SCISSOR_VERTICAL,(4u<<16)|1u);
        rsx_dispatch_method(&g.rsx,0x1D90,0xFFFF0000); // masked red, preserves blue/alpha
        sink_clear(NULL,&g.rsx,0x10);
        assert(ld_blit_gpu(0,0x1000,32,0,0x2000,32,8,8)==1);
        D3D12_RESOURCE_BARRIER bar={0}; bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource=g.surfaces[dest].tex; bar.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET; bar.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
        read_subresource(g.surfaces[dest].tex,0,rb,0,DXGI_FORMAT_R8G8B8A8_UNORM,sf->w,sf->h);
        OK(g.list->lpVtbl->Close(g.list)); ID3D12CommandList* lists[]={(ID3D12CommandList*)g.list};
        g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);
        OK(g.queue->lpVtbl->Signal(g.queue,g.fence,100+mode));
        OK(g.fence->lpVtbl->SetEventOnCompletion(g.fence,100+mode,g.fence_event));
        assert(WaitForSingleObject(g.fence_event,10000)==WAIT_OBJECT_0);
        u8* bytes; OK(rb->lpVtbl->Map(rb,0,NULL,(void**)&bytes));
        for(u32 y=0;y<sf->h;++y)for(u32 x=0;x<sf->w;++x) {
            int inside=x>=2*sf->w/8 && x<5*sf->w/8 && y>=sf->h/8 && y<5*sf->h/8;
            const u8* pixel=bytes+y*256+x*4;
            assert(pixel[0]==(inside?255:0) && pixel[1]==0 && pixel[2]==255 && pixel[3]==255);
        }
        for(u32 y=0;y<atlas_h;++y)for(u32 x=0;x<atlas_w;++x) {
            const int inside=x>=atlas_w/4 && x<3*atlas_w/4 && y>=atlas_h/4 && y<3*atlas_h/4;
            const u8* pixel=bytes+65536+y*256+x*4;
            assert(pixel[0]==0 && pixel[1]==(inside?255:0) && pixel[2]==0 && pixel[3]==255);
            float old_depth,new_depth;
            memcpy(&old_depth,bytes+131072+y*old_depth_pitch+x*4,4);
            memcpy(&new_depth,bytes+196608+y*new_depth_pitch+x*4,4);
            assert(fabsf(old_depth-255.0f/16777215.0f)<1e-7f);
            assert(fabsf(new_depth-(inside?32768.0f/65535.0f:1.0f))<1e-7f);
        }
        for(u32 y=0;y<sf->h;++y)for(u32 x=0;x<sf->w;++x) {
            const int inside=x>=2*sf->w/8 && x<5*sf->w/8 && y>=sf->h/8 && y<5*sf->h/8;
            float depth; memcpy(&depth,bytes+262144+y*scene_depth_pitch+x*4,4);
            assert(fabsf(depth-(inside?4194304.0f/16777215.0f:1.0f))<1e-7f);
        }
        rb->lpVtbl->Unmap(rb,0,NULL);
        for(u32 i=0;i<g.n_surfaces;++i)g.surfaces[i].tex->lpVtbl->Release(g.surfaces[i].tex);
        for(u32 i=0;i<g.n_zdepths;++i) {
            g.zdepths[i].tex->lpVtbl->Release(g.zdepths[i].tex);
            g.zdepths[i].snapshot->lpVtbl->Release(g.zdepths[i].snapshot);
        }
        memset(g.surfaces,0,sizeof(g.surfaces)); memset(g.zdepths,0,sizeof(g.zdepths));
        g.depth=NULL;
    }
    g.n_surfaces=g.n_zdepths=0; g.ready=0; memset(&g_ld_resolution,0,sizeof(g_ld_resolution));
    rsx_color_clear_release(&g.color_clear);
    rb->lpVtbl->Release(rb); g.rtv_heap->lpVtbl->Release(g.rtv_heap);
    g.srv_cpu_heap->lpVtbl->Release(g.srv_cpu_heap); g.dsv_heap->lpVtbl->Release(g.dsv_heap);
    g.rtv_heap=g.srv_cpu_heap=g.dsv_heap=NULL;
    puts("Custom resolution GPU readbacks: native, 1.5x, 2x, fractional asymmetric targets and independent shadow atlases; masked scissor clears, guest address bounds, depth dimensions and GPU blits passed.");
    puts("Z16/Z24S8 GPU clears: far depth, partial scissor, unused high Z16 bits, stencil-only depth preservation and legacy negative control passed at six resolutions.");
}
static void graphics_sampler_tests(void)
{
    rsx_dsp_texture t={0}; t.dimension=2; t.format=0x86; t.mipmaps=8;
    t.filter=0x02060000; t.control0=0x80000000u|(7u*256u<<7);
    for (u32 af=1;af<=16;af*=2) {
        g_ld_anisotropy=af; g_ld_mip_bias=-0.5f;
        D3D12_SAMPLER_DESC sd=decode_sampler(&t);
        assert(sd.MaxAnisotropy==af && sd.MipLODBias==-0.5f);
        assert(sd.Filter==(af==1 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_ANISOTROPIC));
    }
    const u64 quality_key=sampler_key(&t);
    t.filter=0x01010000; assert(decode_sampler(&t).MaxAnisotropy==1 && decode_sampler(&t).MipLODBias==0);
    t.filter=0x02060000; t.format=0xB2; assert(decode_sampler(&t).MaxAnisotropy==1 && decode_sampler(&t).MipLODBias==0 && sampler_key(&t)!=quality_key);
    t.format=0x86|TEX_FMT_UNNORM; assert(decode_sampler(&t).MaxAnisotropy==1 && sampler_key(&t)!=quality_key);
    t.format=0x86|TEX_FMT_LINEAR; assert(decode_sampler(&t).MaxAnisotropy==1 && sampler_key(&t)!=quality_key);
    t.format=0x86; t.mipmaps=1; assert(decode_sampler(&t).MaxAnisotropy==1 && sampler_key(&t)!=quality_key);
    g_ld_anisotropy=1; g_ld_mip_bias=0;
    puts("Graphics samplers: 1/2/4/8/16x AF and negative mip bias; point/UI/depth/single-mip exclusions passed.");
}
static void postprocess_tests(void)
{
    const u32 w=19,h=13; // Non-multiple dispatch dimensions exercise bounds.
    rsx_postprocess p={0}; assert(rsx_postprocess_init(&p,g.dev,w,h)==0);
    ID3D12Resource* rb=buffer(D3D12_HEAP_TYPE_READBACK);
    D3D12_HEAP_PROPERTIES hp={0}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd={0}; rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width=w; rd.Height=h; rd.DepthOrArraySize=rd.MipLevels=1;
    rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count=1;
    rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* image;
    OK(g.dev->lpVtbl->CreateCommittedResource(g.dev,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&image));
    for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x) {
        u8* px=g.upload_mapped+y*256+x*4;
        px[0]=px[1]=px[2]=x<=y ? 64:192; px[3]=137;
        if (x==15) px[0]=px[1]=px[2]=128;
        if (x==16) px[0]=px[1]=px[2]=144;
    }
    for(u32 mode=0;mode<4;++mode) {
        OK(g.alloc->lpVtbl->Reset(g.alloc)); OK(g.list->lpVtbl->Reset(g.list,g.alloc,NULL));
        D3D12_RESOURCE_BARRIER bar={0}; bar.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (!mode) {
            D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
            src.pResource=g.upload; src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint.Format=rd.Format; src.PlacedFootprint.Footprint.Width=w;
            src.PlacedFootprint.Footprint.Height=h; src.PlacedFootprint.Footprint.Depth=1;
            src.PlacedFootprint.Footprint.RowPitch=256;
            dst.pResource=image; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            g.list->lpVtbl->CopyTextureRegion(g.list,&dst,0,0,0,&src,NULL);
            bar.Transition.pResource=image; bar.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
            bar.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET; g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
        }
        ID3D12Resource* out=rsx_postprocess_run(&p,g.dev,g.list,image,mode==1,mode==2 ? 1.0f:0,mode==3 ? -1.0f:0);
        bar.Transition.pResource=out; bar.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
        bar.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE; g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
        read_subresource(out,0,rb,0,rd.Format,w,h);
        bar.Transition.pResource=image; g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
        read_subresource(image,0,rb,65536,rd.Format,w,h);
        bar.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE; bar.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
        bar.Transition.pResource=out; g.list->lpVtbl->ResourceBarrier(g.list,1,&bar);
        OK(g.list->lpVtbl->Close(g.list)); ID3D12CommandList* lists[]={(ID3D12CommandList*)g.list};
        g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);
        OK(g.queue->lpVtbl->Signal(g.queue,g.fence,200+mode));
        OK(g.fence->lpVtbl->SetEventOnCompletion(g.fence,200+mode,g.fence_event));
        assert(WaitForSingleObject(g.fence_event,10000)==WAIT_OBJECT_0);
        u8* bytes; OK(rb->lpVtbl->Map(rb,0,NULL,(void**)&bytes));
        u32 smoothed=0,sharpened=0;
        for(u32 y=0;y<h;++y)for(u32 x=0;x<w;++x) {
            u8* px=bytes+y*256+x*4; const u8 expected=g.upload_mapped[y*256+x*4];
            assert(!memcmp(bytes+65536+y*256+x*4,g.upload_mapped+y*256+x*4,4)); // Guest image unchanged.
            assert(px[3]==137);
            if(mode==0) assert(abs((int)px[0]-expected)<=1);
            if(mode==2) {
                assert(px[0]>=64 && px[0]<=192);
                if(abs((int)px[0]-expected)>2) ++sharpened;
            }
            if(mode==3) assert(abs((int)px[0]-expected/2)<=1);
            if(mode==1 && px[0]>65 && px[0]<191) ++smoothed;
        }
        if(mode==1) assert(smoothed>8);
        if(mode==2) assert(sharpened>=h);
        rb->lpVtbl->Unmap(rb,0,NULL);
    }
    rsx_postprocess_destroy(&p); image->lpVtbl->Release(image); rb->lpVtbl->Release(rb);
    puts("Presentation GPU readbacks: FXAA smooths diagonal edges, sharpening increases detail within local bounds, exposure halves RGB, alpha and guest image preserved.");
}
static void shadow_sampler_tests(void)
{
    rsx_dsp_texture t={0};t.enabled=1;t.dimension=2;t.format=0xB2;
    t.wrap=0x60030303;t.filter=0x02022000;t.control0=0x80000000;
    const unsigned saved=g_ld_shadow_compare;g_ld_shadow_compare=0;
    const u64 raw_key=sampler_key(&t);
    assert(!ld_texture_shadow(&t));
    assert(decode_sampler(&t).Filter==D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT);
    g_ld_shadow_compare=1;
    assert(ld_texture_shadow(&t));
    D3D12_SAMPLER_DESC sd=decode_sampler(&t);
    assert(sd.Filter==D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    assert(sd.ComparisonFunc==D3D12_COMPARISON_FUNC_GREATER_EQUAL);
    assert(sd.AddressU==D3D12_TEXTURE_ADDRESS_MODE_CLAMP && sd.AddressV==sd.AddressU && sd.AddressW==sd.AddressU);
    assert(sd.MinLOD==0 && sd.MaxLOD==0);
    const u64 key=sampler_key(&t);assert(key!=raw_key);
    const D3D12_COMPARISON_FUNC expected[8]={D3D12_COMPARISON_FUNC_NEVER,
        D3D12_COMPARISON_FUNC_LESS,D3D12_COMPARISON_FUNC_EQUAL,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,D3D12_COMPARISON_FUNC_GREATER,
        D3D12_COMPARISON_FUNC_NOT_EQUAL,D3D12_COMPARISON_FUNC_GREATER_EQUAL,
        D3D12_COMPARISON_FUNC_ALWAYS};
    for(u32 c=1;c<7;++c) {
        t.wrap=(t.wrap&0x0FFFFFFFu)|(c<<28);
        assert(decode_sampler(&t).ComparisonFunc==expected[c]);
        if(c!=6)assert(sampler_key(&t)!=key);
    }
    t.wrap=0x60030303;
    for(u32 bit=7;bit<=30;++bit) {
        t.control0=0x80000000u|(1u<<bit);assert(sampler_key(&t)!=key);
    }
    t.control0=0x80000004;assert(!ld_texture_shadow(&t));
    t.control0=0x80000000;t.wrap^=1u<<16;assert(sampler_key(&t)!=key);
    t.wrap=0x60030303;t.cubemap=1;assert(!ld_texture_shadow(&t));
    t.cubemap=0;t.dimension=3;assert(!ld_texture_shadow(&t));
    t.dimension=2;t.format=0x85;assert(!ld_texture_shadow(&t));
    t.format=0xB2;t.wrap=0x00030303;assert(!ld_texture_shadow(&t));
    t.wrap=0x70030303;assert(!ld_texture_shadow(&t));
    g_ld_shadow_compare=saved;
    puts("Shadow sampler: Haze GEQUAL/linear state, six RSX comparisons, exact LOD/R identity, raw/cube/color/alpha-kill exclusions passed");
}
static void topology_copy_tests(void)
{
    const u32 saved_flags=g_ld_cpu_work;
    dc_reset();
    const rsx_vertex_remap saved_remap=dc.ref_remap;
    u8* pages=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(pages); DWORD old;
    assert(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old));
    unsigned cases=0;
    for (u32 count=0; count<=257; ++count) {
        u32* map=(u32*)(pages+4096-count*sizeof(u32));
        for (u32 i=0; i<count; ++i) map[i]=(i*17u)%29u;
        for (unsigned remapped=0; remapped<2; ++remapped)
        for (unsigned short_capacity=0; short_capacity<2; ++short_capacity)
        for (unsigned fast=0; fast<2; ++fast) {
            u32 output[260];
            for (u32 i=0; i<260; ++i) output[i]=0xA5A5A5A5u;
            dc.n_source_refs=count; dc.refs_remapped=remapped;
            dc.ref_remap.occurrence_to_unique=map;
            const u32 capacity=short_capacity && count>4 ? count-4 : count;
            dc.ref_remap.occurrence_capacity=capacity;
            g_ld_cpu_work=fast?1024:0;
            write_topology_indices(PRIM_TRIANGLES,output+1);
            const u32 written=count-count%3;
            for (u32 i=0; i<written; ++i)
                assert(output[i+1]==(remapped && i<capacity ? map[i] : i));
            assert(output[0]==0xA5A5A5A5u);
            for (u32 i=written+1; i<260; ++i) assert(output[i]==0xA5A5A5A5u);
            ++cases;
        }
    }
    dc.ref_remap=saved_remap; dc_reset(); g_ld_cpu_work=saved_flags;
    VirtualFree(pages,0,MEM_RELEASE);
    printf("Triangle indices: %u ordered-oracle cases, duplicate maps, identity/short-capacity fallback, remainder and protected tails passed\n",cases);
}
static void topology_copy_benchmark(void)
{
    if (!getenv("LD_TOPOLOGY_BENCH")) return;
    const u32 count=1000007, written=count-count%3;
    u32* map=malloc((size_t)count*sizeof(*map));
    u32* output=malloc((size_t)count*sizeof(*output)); assert(map && output);
    for (u32 i=0; i<count; ++i) map[i]=(i*17u)%65536u;
    const u32 saved_flags=g_ld_cpu_work;
    dc_reset(); const rsx_vertex_remap saved_remap=dc.ref_remap;
    dc.refs_remapped=1; dc.n_source_refs=count;
    dc.ref_remap.occurrence_to_unique=map; dc.ref_remap.occurrence_capacity=count;
    LARGE_INTEGER frequency; assert(QueryPerformanceFrequency(&frequency));
    for (unsigned pair=0; pair<3; ++pair) for (unsigned fast=0; fast<2; ++fast) {
        g_ld_cpu_work=fast?1024:0;
        write_topology_indices(PRIM_TRIANGLES,output);
        LARGE_INTEGER begin,end; QueryPerformanceCounter(&begin);
        for (unsigned repeat=0; repeat<32; ++repeat)
            write_topology_indices(PRIM_TRIANGLES,output);
        QueryPerformanceCounter(&end);
        assert(!memcmp(output,map,(size_t)written*sizeof(*map)));
        printf("Triangle index microbench pair=%u bulk=%u indices=%u mean_ms=%.6f\n",
            pair,fast,written,(double)(end.QuadPart-begin.QuadPart)*1000/frequency.QuadPart/32);
    }
    dc.ref_remap=saved_remap; dc_reset(); g_ld_cpu_work=saved_flags;
    free(output); free(map);
}
static u64 override_epoch=1;
static u8 override_pixels[4*4*4];
static unsigned override_calls;
static int override_present=1;
static u64 test_override_revision(u32 format,u32 width,u32 height,void* user)
{
    assert(user==&override_epoch);
    return (format&0x9F)==0x88 && width==4 && height==4 ? override_epoch : 0;
}
static const u8* test_override_pixels(u32 format,u32 w,u32 h,const u8* src,size_t size,void* user)
{
    assert(user==&override_epoch && (format&0x9F)==0x88 && w==4 && h==4);
    assert(src==guest+1536 && size==16 && !memcmp(src,guest+1536,16));
    ++override_calls;
    return override_present ? override_pixels : NULL;
}
static void override_tests(ID3D12Resource* rb)
{
    OK(g.alloc->lpVtbl->Reset(g.alloc));
    OK(g.list->lpVtbl->Reset(g.list,g.alloc,NULL));
    g.upload_used=0;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=SRV_HEAP_SLOTS;
    OK(g.dev->lpVtbl->CreateDescriptorHeap(g.dev,&hd,&IID_ID3D12DescriptorHeap,(void**)&g.srv_cpu_heap));
    g.srv_step=g.dev->lpVtbl->GetDescriptorHandleIncrementSize(g.dev,hd.Type);
    rsx_dsp_texture t={0};
    t.dimension=2; t.width=t.height=4; t.mipmaps=1; t.format=0x88; t.offset=1536; t.remap=0xAAE4;
    memset(guest+1536,0x73,16);
    rsx_live_draw_set_texture_override(test_override_revision,test_override_pixels,&override_epoch);
    // Multi-level and cubemap descriptors are ineligible for this bounded API.
    t.cubemap=1; assert(!texture_override_revision(&t)); t.cubemap=0;
    t.mipmaps=2; assert(!texture_override_revision(&t)); t.mipmaps=1;
    for (u32 p=0;p<16;p++) { override_pixels[p*4]=255; override_pixels[p*4+3]=123; }
    const u32 slot=texture_srv_slot(&t)-SRV_TEXTURE_BASE;
    texcache_t* entry=&g.textures[slot];
    assert(entry->override_rgba && override_calls==1);
    ID3D12Resource* first=entry->tex;
    assert(texture_srv_slot(&t)==SRV_TEXTURE_BASE+slot && entry->tex==first && override_calls==1);
    copy_source(first); read_subresource(first,0,rb,8192,DXGI_FORMAT_R8G8B8A8_UNORM,4,4);
    // Revision changes refresh within the same frame without guest mutation.
    ++override_epoch;
    for (u32 p=0;p<16;p++) { override_pixels[p*4]=0; override_pixels[p*4+1]=255; }
    texture_srv_slot(&t);
    assert(entry->tex!=first && entry->override_revision==2 && override_calls==2);
    copy_source(entry->tex); read_subresource(entry->tex,0,rb,12288,DXGI_FORMAT_R8G8B8A8_UNORM,4,4);
    // A missing pack reverts to the original BC3 resource and descriptor format.
    ++override_epoch; override_present=0;
    texture_srv_slot(&t);
    D3D12_RESOURCE_DESC desc; entry->tex->lpVtbl->GetDesc(entry->tex,&desc);
    assert(!entry->override_rgba && desc.Format==DXGI_FORMAT_BC3_UNORM && override_calls==3);
    copy_source(entry->tex); read_subresource(entry->tex,0,rb,16384,DXGI_FORMAT_BC3_UNORM,4,4);
    OK(g.list->lpVtbl->Close(g.list));
    ID3D12CommandList* lists[]={(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue,1,lists);
    ID3D12Fence* override_fence;
    OK(g.dev->lpVtbl->CreateFence(g.dev,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&override_fence));
    OK(g.queue->lpVtbl->Signal(g.queue,override_fence,1));
    OK(override_fence->lpVtbl->SetEventOnCompletion(override_fence,1,g.fence_event));
    assert(WaitForSingleObject(g.fence_event,10000)==WAIT_OBJECT_0);
    u8* bytes; OK(rb->lpVtbl->Map(rb,0,NULL,(void**)&bytes));
    assert(bytes[8192]==255 && bytes[8193]==0 && bytes[8195]==123);
    assert(bytes[12288]==0 && bytes[12289]==255 && bytes[12291]==123);
    assert(!memcmp(bytes+16384,guest+1536,16));
    rb->lpVtbl->Unmap(rb,0,NULL);
    override_fence->lpVtbl->Release(override_fence);
    rsx_live_draw_set_texture_override(NULL,NULL,NULL);
    entry->tex->lpVtbl->Release(entry->tex);
    for (u32 i=0;i<g.n_retired_textures;i++) g.retired_textures[i]->lpVtbl->Release(g.retired_textures[i]);
    g.n_retired_textures=0;
    memset(g.textures,0,sizeof(g.textures)); memset(g.texture_buckets,0,sizeof(g.texture_buckets)); g.n_textures=0;
    g.srv_cpu_heap->lpVtbl->Release(g.srv_cpu_heap); g.srv_cpu_heap=NULL;
    puts("Texture overrides: RGBA GPU pixels/alpha, cached reuse, same-frame revision, original fallback and eligibility passed.");
}
static void texture_content_cache_tests(ID3D12Resource* readback)
{
    const u32 saved_upload_used = g.upload_used;
    ID3D12Fence* fence = NULL;
    OK(g.dev->lpVtbl->CreateFence(g.dev, 0, D3D12_FENCE_FLAG_NONE,
                                  &IID_ID3D12Fence, (void**)&fence));
    OK(g.alloc->lpVtbl->Reset(g.alloc));
    OK(g.list->lpVtbl->Reset(g.list, g.alloc, NULL));
    ld_texture_content_clear();
    g.upload_used = 0;

    u8 bytes_a[8] = {0x31,0x42,0x53,0x64,0x75,0x86,0x97,0xA8};
    u8 bytes_same[8]; memcpy(bytes_same, bytes_a, sizeof(bytes_a));
    u8 bytes_changed[8]; memcpy(bytes_changed, bytes_a, sizeof(bytes_a)); bytes_changed[0] ^= 0x80;
    tex_level_t a = {4,4,bytes_a,8,1}, same = {4,4,bytes_same,8,1};
    tex_level_t changed_level = {4,4,bytes_changed,8,1};
    ID3D12Resource* first = create_texture_mipped_cached(DXGI_FORMAT_BC1_UNORM, &a, 1);
    assert(first);
    const u32 uploaded = g.upload_used;
    ID3D12Resource* relocated = create_texture_mipped_cached(DXGI_FORMAT_BC1_UNORM, &same, 1);
    assert(relocated == first && g.upload_used == uploaded);
    relocated->lpVtbl->Release(relocated);
    ID3D12Resource* changed = create_texture_mipped_cached(DXGI_FORMAT_BC1_UNORM, &changed_level, 1);
    assert(changed && changed != first && g.upload_used > uploaded);

    u8 mip_data[16]; memset(mip_data, 0xC3, sizeof(mip_data));
    tex_level_t mips[2] = {{4,4,mip_data,8,1},{2,2,mip_data+8,8,1}};
    ID3D12Resource* mip = create_texture_mipped_cached(DXGI_FORMAT_BC1_UNORM, mips, 2);
    u8 bc4_data[8]; memcpy(bc4_data, bytes_a, sizeof(bc4_data));
    tex_level_t bc4_level = {4,4,bc4_data,8,1};
    ID3D12Resource* bc4 = create_texture_mipped_cached(DXGI_FORMAT_BC4_UNORM, &bc4_level, 1);
    assert(mip && bc4 && mip != first && bc4 != first && bc4 != mip);

    u8 collision[8]; memcpy(collision, bytes_a, sizeof(collision)); collision[7] ^= 1;
    tex_level_t collision_level = {4,4,collision,8,1};
    u64 first_hash = 0;
    int found_first = 0;
    for (u32 i = 0; i < LD_TEXTURE_CONTENT_ENTRIES; ++i)
        if (g_ld_texture_content.entries[i].resource == first) {
            first_hash = g_ld_texture_content.entries[i].hash;
            found_first = 1;
        }
    assert(found_first && !ld_texture_content_find(DXGI_FORMAT_BC1_UNORM,
        &collision_level, 1, collision, sizeof(collision), first_hash));
    tex_level_t layout = {4,4,bytes_a,4,2};
    assert(!ld_texture_content_find(DXGI_FORMAT_BC1_UNORM, &layout, 1,
        bytes_a, sizeof(bytes_a), first_hash));

    /* Clearing drops cache ownership while caller references stay live. */
    ld_texture_content_clear();
    D3D12_RESOURCE_DESC desc; first->lpVtbl->GetDesc(first, &desc);
    assert(desc.Format == DXGI_FORMAT_BC1_UNORM && desc.Width == 4 && desc.MipLevels == 1);
    copy_source(first);
    read_subresource(first, 0, readback, 8192, DXGI_FORMAT_BC1_UNORM, 4, 4);
    OK(g.list->lpVtbl->Close(g.list));
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)g.list};
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    OK(g.queue->lpVtbl->Signal(g.queue, fence, 1));
    OK(fence->lpVtbl->SetEventOnCompletion(fence, 1, g.fence_event));
    assert(WaitForSingleObject(g.fence_event, 10000) == WAIT_OBJECT_0);
    u8* pixels = NULL;
    OK(readback->lpVtbl->Map(readback, 0, NULL, (void**)&pixels));
    assert(!memcmp(pixels + 8192, bytes_a, sizeof(bytes_a)));
    readback->lpVtbl->Unmap(readback, 0, NULL);
    first->lpVtbl->Release(first); changed->lpVtbl->Release(changed);
    mip->lpVtbl->Release(mip); bc4->lpVtbl->Release(bc4);

    /* The fixed-capacity LRU evicts one cache reference without invalidating
     * a caller's separate reference to the oldest resource. */
    OK(g.alloc->lpVtbl->Reset(g.alloc));
    OK(g.list->lpVtbl->Reset(g.list, g.alloc, NULL));
    ld_texture_content_clear();
    g.upload_used = 0;
    ID3D12Resource* retained[LD_TEXTURE_CONTENT_ENTRIES + 1] = {0};
    u8 lru_bytes[LD_TEXTURE_CONTENT_ENTRIES + 1][8];
    for (u32 i = 0; i < LD_TEXTURE_CONTENT_ENTRIES + 1; ++i) {
        for (u32 b = 0; b < 8; ++b)
            lru_bytes[i][b] = (u8)((i >> (8u * (b & 3u))) ^ (b * 17u));
        tex_level_t level = {4,4,lru_bytes[i],8,1};
        retained[i] = create_texture_mipped_cached(DXGI_FORMAT_BC1_UNORM, &level, 1);
        assert(retained[i]);
    }
    assert(g_ld_texture_content.count == LD_TEXTURE_CONTENT_ENTRIES);
    tex_level_t evicted_level = {4,4,lru_bytes[0],8,1};
    assert(!ld_texture_content_find(DXGI_FORMAT_BC1_UNORM, &evicted_level, 1,
        lru_bytes[0], sizeof(lru_bytes[0]), rsx_texture_hash_xxh3(lru_bytes[0], 8)));
    D3D12_RESOURCE_DESC retained_desc;
    retained[0]->lpVtbl->GetDesc(retained[0], &retained_desc);
    assert(retained_desc.Format == DXGI_FORMAT_BC1_UNORM && retained_desc.Width == 4);
    ld_texture_content_clear();
    copy_source(retained[0]);
    read_subresource(retained[0], 0, readback, 12288, DXGI_FORMAT_BC1_UNORM, 4, 4);
    OK(g.list->lpVtbl->Close(g.list));
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    OK(g.queue->lpVtbl->Signal(g.queue, fence, 2));
    OK(fence->lpVtbl->SetEventOnCompletion(fence, 2, g.fence_event));
    assert(WaitForSingleObject(g.fence_event, 10000) == WAIT_OBJECT_0);
    OK(readback->lpVtbl->Map(readback, 0, NULL, (void**)&pixels));
    assert(!memcmp(pixels + 12288, lru_bytes[0], sizeof(lru_bytes[0])));
    readback->lpVtbl->Unmap(readback, 0, NULL);
    for (u32 i = 0; i < LD_TEXTURE_CONTENT_ENTRIES + 1; ++i)
        retained[i]->lpVtbl->Release(retained[i]);

    /* A failed capacity preflight leaves upload state unchanged and can retry. */
    OK(g.alloc->lpVtbl->Reset(g.alloc));
    OK(g.list->lpVtbl->Reset(g.list, g.alloc, NULL));
    g.upload_used = UPLOAD_SIZE - 100;
    assert(!create_texture_mipped_cached(DXGI_FORMAT_BC1_UNORM, &a, 1));
    assert(g.upload_used == UPLOAD_SIZE - 100 && !g_ld_texture_content.count);
    g.upload_used = 0;
    ID3D12Resource* retry = create_texture_mipped_cached(DXGI_FORMAT_BC1_UNORM, &a, 1);
    assert(retry && g.upload_used > 0 && g_ld_texture_content.count == 1);
    copy_source(retry);
    read_subresource(retry, 0, readback, 8192, DXGI_FORMAT_BC1_UNORM, 4, 4);
    ld_texture_content_clear();
    OK(g.list->lpVtbl->Close(g.list));
    g.queue->lpVtbl->ExecuteCommandLists(g.queue, 1, lists);
    OK(g.queue->lpVtbl->Signal(g.queue, fence, 3));
    OK(fence->lpVtbl->SetEventOnCompletion(fence, 3, g.fence_event));
    assert(WaitForSingleObject(g.fence_event, 10000) == WAIT_OBJECT_0);
    OK(readback->lpVtbl->Map(readback, 0, NULL, (void**)&pixels));
    assert(!memcmp(pixels + 8192, bytes_a, sizeof(bytes_a)));
    readback->lpVtbl->Unmap(readback, 0, NULL);
    retry->lpVtbl->Release(retry);
    fence->lpVtbl->Release(fence);
    g.upload_used = saved_upload_used;
    /* Leave the shared fixture's command list open for the following cases. */
    OK(g.alloc->lpVtbl->Reset(g.alloc));
    OK(g.list->lpVtbl->Reset(g.list, g.alloc, NULL));
    puts("Texture content cache: relocation hit, byte/format/mip/layout distinction, collision rejection, caller lifetime and upload-capacity retry passed.");
}

int main(void)
{
    shadow_sampler_tests();
    graphics_sampler_tests();
    cache_tests();
    fetch_tests();
    reference_batch_tests();
    topology_copy_tests();
    topology_copy_benchmark();
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

    texture_content_cache_tests(readback);

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
    override_tests(readback);
    bias_tests();
    resolution_tests();
    postprocess_tests();
    rgb->lpVtbl->Release(rgb); cube->lpVtbl->Release(cube); readback->lpVtbl->Release(readback);
    g.upload->lpVtbl->Unmap(g.upload,0,NULL); g.upload->lpVtbl->Release(g.upload);
    g.list->lpVtbl->Release(g.list); g.alloc->lpVtbl->Release(g.alloc);
    g.queue->lpVtbl->Release(g.queue); g.fence->lpVtbl->Release(g.fence);
    CloseHandle(g.fence_event); g.dev->lpVtbl->Release(g.dev);
    warp->lpVtbl->Release(warp); factory->lpVtbl->Release(factory);
    puts("Live textures: cache identity/collisions, six padded BC faces and opaque D8R8G8B8 GPU readback passed.");
}
