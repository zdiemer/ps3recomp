/* Asset-free execution of the renderer's actual D3D12 clear implementation. */
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <dxgi1_4.h>
#include "../rsx_color_clear.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#define OK(x) do { HRESULT test_hr = (x); if (FAILED(test_hr)) { \
    fprintf(stderr, "line %d HRESULT %08lX\n", __LINE__, (unsigned long)test_hr); abort(); } } while (0)

static void exercise(ID3D12Device* dev, DXGI_FORMAT format)
{
    ID3D12CommandQueue* queue; ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list; ID3D12Fence* fence;
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    OK(dev->lpVtbl->CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&queue));
    OK(dev->lpVtbl->CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         &IID_ID3D12CommandAllocator, (void**)&alloc));
    OK(dev->lpVtbl->CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                    NULL, &IID_ID3D12GraphicsCommandList, (void**)&list));
    OK(dev->lpVtbl->CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence));
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = 4; td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = format; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* tex;
    OK(dev->lpVtbl->CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void**)&tex));
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* heap;
    OK(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &rtv);
    dev->lpVtbl->CreateRenderTargetView(dev, tex, NULL, rtv);
    rsx_color_clear clear = {0};
    const float seed[4] = {.25f,.5f,.75f,1}, zero[4] = {0,0,0,0}, red[4] = {.75f,.25f,.5f,.25f};
    D3D12_RECT full = rsx_clear_rect(4,4,0,0,0);
    D3D12_RECT center = rsx_clear_rect(4,4,(2u<<16)|1,(2u<<16)|1,1);
    D3D12_RECT upper_right = rsx_clear_rect(4,4,(2u<<16)|2,2u<<16,1);
    D3D12_RECT clipped = rsx_clear_rect(4,4,(3u<<16)|3,(3u<<16)|3,1);
    D3D12_RECT empty = rsx_clear_rect(4,4,2,4u<<16,1);
    assert(clipped.left == 3 && clipped.right == 4 && clipped.bottom == 4);
    OK(rsx_color_clear_record(&clear,dev,list,rtv,format,4,4,&full,15,seed));
    OK(rsx_color_clear_record(&clear,dev,list,rtv,format,4,4,&center,8,zero));
    OK(rsx_color_clear_record(&clear,dev,list,rtv,format,4,4,&upper_right,7,zero));
    OK(rsx_color_clear_record(&clear,dev,list,rtv,format,4,4,&clipped,15,red));
    OK(rsx_color_clear_record(&clear,dev,list,rtv,format,4,4,&empty,15,zero));
    D3D12_RESOURCE_BARRIER bar = {0}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = tex; bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&bar);
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 1024; bd.Height = bd.DepthOrArraySize = bd.MipLevels = bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback;
    OK(dev->lpVtbl->CreateCommittedResource(dev,&hp,D3D12_HEAP_FLAG_NONE,&bd,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback));
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = format;
    dst.PlacedFootprint.Footprint.Width = dst.PlacedFootprint.Footprint.Height = 4;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    OK(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[] = {(ID3D12CommandList*)list};
    queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    OK(queue->lpVtbl->Signal(queue,fence,1));
    HANDLE event = CreateEventA(NULL,FALSE,FALSE,NULL); assert(event);
    OK(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    assert(WaitForSingleObject(event,30000) == WAIT_OBJECT_0); CloseHandle(event);
    D3D12_RANGE range = {0,1024}; uint8_t* bytes;
    OK(readback->lpVtbl->Map(readback,0,&range,(void**)&bytes));
    unsigned fp16 = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (unsigned y=0;y<4;y++) for (unsigned x=0;x<4;x++) for (unsigned channel=0;channel<4;channel++) {
        float expected = seed[channel];
        if (channel == 3 && x>=1 && x<3 && y>=1 && y<3) expected = 0;
        if (channel != 3 && x>=2 && y<2) expected = 0;
        if (x==3 && y==3) expected = red[channel];
        if (fp16) {
            const uint16_t halves[] = {0,0x3400,0x3800,0x3A00,0x3C00};
            uint16_t actual; memcpy(&actual,bytes+y*256+x*8+channel*2,2);
            assert(actual == halves[(unsigned)(expected*4)]);
        } else {
            int actual = bytes[y*256+x*4+channel];
            assert(abs(actual-(int)lroundf(expected*255)) <= 1);
        }
    }
    readback->lpVtbl->Unmap(readback,0,NULL);
    rsx_color_clear_release(&clear);
    readback->lpVtbl->Release(readback); tex->lpVtbl->Release(tex); heap->lpVtbl->Release(heap);
    list->lpVtbl->Release(list); alloc->lpVtbl->Release(alloc); queue->lpVtbl->Release(queue); fence->lpVtbl->Release(fence);
    printf("D3D12 masked/scissored clear passed format=%u\n",(unsigned)format);
}

int main(void)
{
    IDXGIFactory4* factory; IDXGIAdapter* warp; ID3D12Device* dev;
    OK(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
    OK(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
    OK(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&dev));
    exercise(dev,DXGI_FORMAT_R8G8B8A8_UNORM);
    exercise(dev,DXGI_FORMAT_R16G16B16A16_FLOAT);
    dev->lpVtbl->Release(dev); warp->lpVtbl->Release(warp); factory->lpVtbl->Release(factory);
}
