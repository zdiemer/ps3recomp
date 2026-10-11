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
}
int main(void)
{
    cache_tests();
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
    rgb->lpVtbl->Release(rgb); cube->lpVtbl->Release(cube); readback->lpVtbl->Release(readback);
    g.upload->lpVtbl->Unmap(g.upload,0,NULL); g.upload->lpVtbl->Release(g.upload);
    g.list->lpVtbl->Release(g.list); g.alloc->lpVtbl->Release(g.alloc);
    g.queue->lpVtbl->Release(g.queue); g.fence->lpVtbl->Release(g.fence);
    CloseHandle(g.fence_event); g.dev->lpVtbl->Release(g.dev);
    warp->lpVtbl->Release(warp); factory->lpVtbl->Release(factory);
    puts("Live textures: cache identity/collisions, six padded BC faces and opaque D8R8G8B8 GPU readback passed.");
}
