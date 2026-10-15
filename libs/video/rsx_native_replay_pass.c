#if defined(_WIN32)

#define _CRT_SECURE_NO_WARNINGS
#include "rsx_native_replay_pass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RSX_NATIVE_REPLAY_MAX_SAMPLER_TABLES 128u
#define RSX_NATIVE_REPLAY_MAX_DRAWS 8192u
#define RSX_NATIVE_REPLAY_MAX_ALLOCATION (512ull * 1024ull * 1024ull)
#define RSX_NATIVE_REPLAY_MAX_DIMENSION 16384u

typedef enum replay_state {
    REPLAY_CREATED = 0,
    REPLAY_CAPTURING = 1,
    REPLAY_RECORDED = 2,
    REPLAY_COMPARE_RECORDED = 3,
    REPLAY_FAILED = 4
} replay_state;

typedef struct replay_draw {
    rsx_native_draw_packet packet;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_table;
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table;
    D3D12_GPU_DESCRIPTOR_HANDLE vtex_table;
} replay_draw;

typedef struct replay_color_readback {
    ID3D12Resource* resource;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT original_footprint;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT replay_footprint;
    UINT rows;
    UINT64 row_bytes;
    UINT64 total_bytes;
} replay_color_readback;

struct rsx_native_replay_pass {
    ID3D12Device* device;
    replay_state state;
    char error[160];

    uint32_t max_draws;
    replay_draw* draws;
    ID3D12Resource** sampled_refs;
    uint32_t sampled_ref_count;
    uint32_t sampled_ref_capacity;
    rsx_native_draw_payload_store payload;

    uint32_t color_count;
    ID3D12Resource* original_color[RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS];
    ID3D12Resource* replay_color[RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS];
    D3D12_CPU_DESCRIPTOR_HANDLE source_rtv[RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS];
    D3D12_CPU_DESCRIPTOR_HANDLE replay_rtv[RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS];
    ID3D12Resource* original_depth;
    ID3D12Resource* replay_depth;
    D3D12_CPU_DESCRIPTOR_HANDLE source_dsv;
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view;
    D3D12_CPU_DESCRIPTOR_HANDLE replay_dsv;
    int source_signature_set;

    ID3D12DescriptorHeap* srv_heap;
    ID3D12DescriptorHeap* sampler_heap;
    ID3D12DescriptorHeap* rtv_heap;
    ID3D12DescriptorHeap* dsv_heap;
    UINT srv_step;
    UINT sampler_step;
    UINT rtv_step;
    UINT dsv_step;
    uint32_t srv_used;
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_keys[
        RSX_NATIVE_REPLAY_MAX_SAMPLER_TABLES][RSX_NATIVE_REPLAY_TABLE_SIZE];
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_tables[
        RSX_NATIVE_REPLAY_MAX_SAMPLER_TABLES];
    uint32_t sampler_count;

    ID3D12Resource* upload;
    ID3D12Resource* readback[RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS];
    replay_color_readback color_readback[RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS];
    uint64_t readback_bytes;
    uint64_t owned_gpu_bytes;
};

static int replay_fail(rsx_native_replay_pass* pass, const char* message)
{
    if (pass) {
        pass->state = REPLAY_FAILED;
        if (message) {
            strncpy(pass->error, message, sizeof(pass->error) - 1u);
            pass->error[sizeof(pass->error) - 1u] = '\0';
        }
    }
    return 0;
}

static int replay_make_heap(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT count,
    D3D12_DESCRIPTOR_HEAP_FLAGS flags,
    ID3D12DescriptorHeap** out)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = flags;
    return SUCCEEDED(device->lpVtbl->CreateDescriptorHeap(
        device, &desc, &IID_ID3D12DescriptorHeap, (void**)out));
}

static D3D12_CPU_DESCRIPTOR_HANDLE replay_cpu_handle(
    ID3D12DescriptorHeap* heap, UINT step, UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    memset(&handle, 0, sizeof(handle));
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &handle);
    handle.ptr += (SIZE_T)step * index;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE replay_gpu_handle(
    ID3D12DescriptorHeap* heap, UINT step, UINT index)
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    memset(&handle, 0, sizeof(handle));
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &handle);
    handle.ptr += (UINT64)step * index;
    return handle;
}

static void replay_transition(
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier;
    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->lpVtbl->ResourceBarrier(list, 1, &barrier);
}

static int replay_same_sampler_table(
    const D3D12_CPU_DESCRIPTOR_HANDLE left[RSX_NATIVE_REPLAY_TABLE_SIZE],
    const D3D12_CPU_DESCRIPTOR_HANDLE right[RSX_NATIVE_REPLAY_TABLE_SIZE])
{
    uint32_t i;
    for (i = 0; i < RSX_NATIVE_REPLAY_TABLE_SIZE; ++i)
        if (left[i].ptr != right[i].ptr)
            return 0;
    return 1;
}

static void replay_copy_descriptor_table(
    ID3D12Device* device,
    ID3D12DescriptorHeap* heap,
    UINT step,
    UINT descriptor_index,
    const D3D12_CPU_DESCRIPTOR_HANDLE source[RSX_NATIVE_REPLAY_TABLE_SIZE],
    D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    D3D12_CPU_DESCRIPTOR_HANDLE destination = replay_cpu_handle(
        heap, step, descriptor_index);
    UINT destination_size = RSX_NATIVE_REPLAY_TABLE_SIZE;
    UINT source_sizes[RSX_NATIVE_REPLAY_TABLE_SIZE];
    UINT i;
    for (i = 0; i < RSX_NATIVE_REPLAY_TABLE_SIZE; ++i)
        source_sizes[i] = 1;
    /* Source descriptors can be scattered among the renderer's CPU heaps.
     * One range per source preserves slot order without assuming adjacency. */
    device->lpVtbl->CopyDescriptors(
        device, 1, &destination, &destination_size,
        RSX_NATIVE_REPLAY_TABLE_SIZE, source, source_sizes, type);
}

static int replay_copy_sampler_table(
    rsx_native_replay_pass* pass,
    const D3D12_CPU_DESCRIPTOR_HANDLE source[RSX_NATIVE_REPLAY_TABLE_SIZE],
    D3D12_GPU_DESCRIPTOR_HANDLE* out)
{
    uint32_t i;
    for (i = 0; i < pass->sampler_count; ++i) {
        if (replay_same_sampler_table(source, pass->sampler_keys[i])) {
            *out = pass->sampler_tables[i];
            return 1;
        }
    }
    if (pass->sampler_count >= RSX_NATIVE_REPLAY_MAX_SAMPLER_TABLES)
        return replay_fail(pass, "sampler_table_limit");
    {
        const uint32_t index = pass->sampler_count;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = replay_gpu_handle(
            pass->sampler_heap, pass->sampler_step,
            index * RSX_NATIVE_REPLAY_TABLE_SIZE);
        memcpy(pass->sampler_keys[index], source,
               sizeof(pass->sampler_keys[index]));
        replay_copy_descriptor_table(
            pass->device, pass->sampler_heap, pass->sampler_step,
            index * RSX_NATIVE_REPLAY_TABLE_SIZE, source,
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        pass->sampler_tables[index] = gpu;
        pass->sampler_count++;
        *out = gpu;
    }
    return 1;
}

rsx_native_replay_pass* rsx_native_replay_pass_create(
    ID3D12Device* device,
    uint32_t max_draws,
    size_t max_payload_bytes)
{
    rsx_native_replay_pass* pass;
    UINT64 srv_count;
    if (!device || !max_draws || max_draws > RSX_NATIVE_REPLAY_MAX_DRAWS ||
        !max_payload_bytes || max_payload_bytes > RSX_NATIVE_REPLAY_MAX_ALLOCATION)
        return NULL;
    srv_count = (UINT64)max_draws * RSX_NATIVE_REPLAY_TABLE_SIZE * 2u;
    if (!srv_count || srv_count > UINT32_MAX)
        return NULL;
    pass = (rsx_native_replay_pass*)calloc(1, sizeof(*pass));
    if (!pass)
        return NULL;
    pass->device = device;
    device->lpVtbl->AddRef(device);
    pass->max_draws = max_draws;
    pass->draws = (replay_draw*)calloc(max_draws, sizeof(*pass->draws));
    pass->sampled_ref_capacity = max_draws * RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES;
    if (pass->sampled_ref_capacity / RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES != max_draws) {
        replay_fail(pass, "sampled_ref_count_overflow");
        rsx_native_replay_pass_destroy(pass);
        return NULL;
    }
    pass->sampled_refs = (ID3D12Resource**)calloc(
        pass->sampled_ref_capacity, sizeof(*pass->sampled_refs));
    if (!pass->draws || !pass->sampled_refs ||
        !rsx_native_draw_payload_init(&pass->payload, max_payload_bytes, max_draws) ||
        !replay_make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            (UINT)srv_count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            &pass->srv_heap) ||
        !replay_make_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            RSX_NATIVE_REPLAY_MAX_SAMPLER_TABLES * RSX_NATIVE_REPLAY_TABLE_SIZE,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &pass->sampler_heap)) {
        replay_fail(pass, "create_storage_or_shader_heap");
        rsx_native_replay_pass_destroy(pass);
        return NULL;
    }
    pass->srv_step = device->lpVtbl->GetDescriptorHandleIncrementSize(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    pass->sampler_step = device->lpVtbl->GetDescriptorHandleIncrementSize(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (!pass->srv_step || !pass->sampler_step) {
        replay_fail(pass, "invalid_descriptor_increment");
        rsx_native_replay_pass_destroy(pass);
        return NULL;
    }
    pass->state = REPLAY_CREATED;
    return pass;
}

int rsx_native_replay_pass_begin(
    rsx_native_replay_pass* pass,
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* const color_resources[RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS],
    uint32_t color_count,
    ID3D12Resource* depth_resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* depth_view)
{
    uint32_t i;
    UINT64 allocation_total = 0;
    if (!pass || pass->state != REPLAY_CREATED || !list || !color_resources ||
        !color_count || color_count > RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS ||
        (!!depth_resource != !!depth_view))
        return replay_fail(pass, "invalid_begin_arguments");
    if (!replay_make_heap(pass->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            color_count, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &pass->rtv_heap) ||
        (depth_resource && !replay_make_heap(pass->device,
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &pass->dsv_heap)))
        return replay_fail(pass, "create_output_descriptor_heap");
    pass->rtv_step = pass->device->lpVtbl->GetDescriptorHandleIncrementSize(
        pass->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (depth_resource)
        pass->dsv_step = pass->device->lpVtbl->GetDescriptorHandleIncrementSize(
            pass->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    if (!pass->rtv_step || (depth_resource && !pass->dsv_step))
        return replay_fail(pass, "invalid_output_descriptor_increment");

    /* Create and validate every clone before recording the first source copy.
     * That keeps a failed begin from leaving a partially-copied replay pass. */
    for (i = 0; i < color_count + (depth_resource ? 1u : 0u); ++i) {
        ID3D12Resource* source = i < color_count
            ? color_resources[i] : depth_resource;
        ID3D12Resource** clone = i < color_count
            ? &pass->replay_color[i] : &pass->replay_depth;
        D3D12_RESOURCE_DESC desc;
        D3D12_HEAP_PROPERTIES heap;
        D3D12_RESOURCE_ALLOCATION_INFO allocation;
        uint32_t prior;
        if (!source)
            return replay_fail(pass, "null_output_resource");
        if (i < color_count) {
            for (prior = 0; prior < i; ++prior)
                if (color_resources[prior] == source)
                    return replay_fail(pass, "aliased_color_outputs");
        } else {
            for (prior = 0; prior < color_count; ++prior)
                if (color_resources[prior] == depth_resource)
                    return replay_fail(pass, "depth_color_output_alias");
        }
        source->lpVtbl->GetDesc(source, &desc);
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.Width > RSX_NATIVE_REPLAY_MAX_DIMENSION ||
            desc.Height > RSX_NATIVE_REPLAY_MAX_DIMENSION ||
            desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1)
            return replay_fail(pass, "unsupported_output_shape");
        (void)pass->device->lpVtbl->GetResourceAllocationInfo(
            pass->device, &allocation, 0, 1, &desc);
        if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX ||
            allocation.SizeInBytes > RSX_NATIVE_REPLAY_MAX_ALLOCATION - allocation_total)
            return replay_fail(pass, "output_allocation_limit");
        allocation_total += allocation.SizeInBytes;
        memset(&heap, 0, sizeof(heap));
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(pass->device->lpVtbl->CreateCommittedResource(
                pass->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                (void**)clone)))
            return replay_fail(pass, "create_output_clone");
        source->lpVtbl->AddRef(source);
        if (i < color_count)
            pass->original_color[i] = source;
        else
            pass->original_depth = source;
    }
    pass->color_count = color_count;
    pass->owned_gpu_bytes = allocation_total;
    if (depth_resource) {
        pass->depth_view = *depth_view;
        pass->device->lpVtbl->CreateDepthStencilView(
            pass->device, pass->replay_depth, &pass->depth_view,
            replay_cpu_handle(pass->dsv_heap, pass->dsv_step, 0));
        pass->replay_dsv = replay_cpu_handle(pass->dsv_heap, pass->dsv_step, 0);
    }
    for (i = 0; i < color_count; ++i) {
        pass->device->lpVtbl->CreateRenderTargetView(
            pass->device, pass->replay_color[i], NULL,
            replay_cpu_handle(pass->rtv_heap, pass->rtv_step, i));
        pass->replay_rtv[i] = replay_cpu_handle(pass->rtv_heap, pass->rtv_step, i);
    }

    /* Source state is restored before returning; clone state becomes the
     * corresponding render/depth state expected by draw-packet recorders. */
    for (i = 0; i < color_count; ++i) {
        replay_transition(list, pass->original_color[i],
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->lpVtbl->CopyResource(
            list, pass->replay_color[i], pass->original_color[i]);
    }
    if (pass->original_depth) {
        replay_transition(list, pass->original_depth,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->lpVtbl->CopyResource(
            list, pass->replay_depth, pass->original_depth);
    }
    for (i = 0; i < color_count; ++i) {
        replay_transition(list, pass->original_color[i],
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        replay_transition(list, pass->replay_color[i],
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (pass->original_depth) {
        replay_transition(list, pass->original_depth,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        replay_transition(list, pass->replay_depth,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    pass->state = REPLAY_CAPTURING;
    return 1;
}

int rsx_native_replay_pass_capture(
    rsx_native_replay_pass* pass,
    const rsx_native_draw_packet* packet,
    const rsx_native_draw_payload_input* input,
    const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    const D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    const D3D12_CPU_DESCRIPTOR_HANDLE vtex_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    ID3D12Resource* const sampled_resources[RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES])
{
    replay_draw* draw;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    uint32_t i;
    if (!pass || pass->state != REPLAY_CAPTURING || !packet || !input ||
        !srv_cpu || !sampler_cpu || !sampled_resources ||
        pass->payload.record_count >= pass->max_draws ||
        packet->transition_count != 0 ||
        packet->render_target_count != pass->color_count ||
        !packet->pipeline_state || !packet->root_signature ||
        (!!packet->has_depth_stencil != !!pass->original_depth) ||
        (packet->has_vertex_texture_table && !vtex_cpu) ||
        (!!input->pixel_constants.size != !!packet->has_pixel_constant_buffer) ||
        (!!(packet->has_vertex_buffer || packet->has_pull_shader_resource) !=
            !!input->vertex.size) ||
        (packet->indexed && !input->index.size) ||
        (!packet->indexed && input->index.size) ||
        (packet->has_vertex_buffer && !packet->vertex_buffer.SizeInBytes) ||
        (packet->has_vertex_buffer &&
            packet->vertex_buffer.SizeInBytes > input->vertex.size) ||
        (packet->indexed && !packet->index_buffer.SizeInBytes) ||
        (packet->indexed &&
            packet->index_buffer.SizeInBytes > input->index.size) ||
        !input->vertex_constants.size)
        return replay_fail(pass, "invalid_draw_or_packet_signature");
    if (!pass->source_signature_set) {
        for (i = 0; i < pass->color_count; ++i)
            pass->source_rtv[i] = packet->render_targets[i];
        if (pass->original_depth)
            pass->source_dsv = packet->depth_stencil;
        pass->source_signature_set = 1;
    } else {
        for (i = 0; i < pass->color_count; ++i)
            if (pass->source_rtv[i].ptr != packet->render_targets[i].ptr)
                return replay_fail(pass, "render_target_descriptor_changed");
        if (pass->original_depth &&
            pass->source_dsv.ptr != packet->depth_stencil.ptr)
            return replay_fail(pass, "depth_descriptor_changed");
    }
    for (i = 0; i < RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES; ++i) {
        uint32_t c;
        ID3D12Resource* resource = sampled_resources[i];
        if (!resource) continue;
        if (resource == pass->original_depth || resource == pass->replay_depth)
            return replay_fail(pass, "sampled_depth_output_alias");
        for (c = 0; c < pass->color_count; ++c)
            if (resource == pass->original_color[c] || resource == pass->replay_color[c])
                return replay_fail(pass, "sampled_color_output_alias");
    }
    if (!rsx_native_draw_payload_append(&pass->payload, input, NULL))
        return replay_fail(pass, "payload_capacity_or_allocation");
    draw = &pass->draws[pass->payload.record_count - 1u];
    memset(draw, 0, sizeof(*draw));
    draw->packet = *packet;
    packet->pipeline_state->lpVtbl->AddRef(packet->pipeline_state);
    packet->root_signature->lpVtbl->AddRef(packet->root_signature);

    if ((uint64_t)pass->srv_used + RSX_NATIVE_REPLAY_TABLE_SIZE *
            (packet->has_vertex_texture_table ? 2u : 1u) >
        (uint64_t)pass->max_draws * RSX_NATIVE_REPLAY_TABLE_SIZE * 2u)
        return replay_fail(pass, "srv_table_limit");
    srv_gpu = replay_gpu_handle(pass->srv_heap, pass->srv_step, pass->srv_used);
    replay_copy_descriptor_table(
        pass->device, pass->srv_heap, pass->srv_step, pass->srv_used,
        srv_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    draw->srv_table = srv_gpu;
    pass->srv_used += RSX_NATIVE_REPLAY_TABLE_SIZE;
    if (packet->has_vertex_texture_table) {
        draw->vtex_table = replay_gpu_handle(
            pass->srv_heap, pass->srv_step, pass->srv_used);
        replay_copy_descriptor_table(
            pass->device, pass->srv_heap, pass->srv_step, pass->srv_used,
            vtex_cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        pass->srv_used += RSX_NATIVE_REPLAY_TABLE_SIZE;
    }
    if (!replay_copy_sampler_table(pass, sampler_cpu, &draw->sampler_table))
        return 0;

    for (i = 0; i < RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES; ++i) {
        ID3D12Resource* resource = sampled_resources[i];
        if (!resource) continue;
        if (pass->sampled_ref_count >= pass->sampled_ref_capacity)
            return replay_fail(pass, "sampled_reference_limit");
        resource->lpVtbl->AddRef(resource);
        pass->sampled_refs[pass->sampled_ref_count++] = resource;
    }
    return 1;
}

int rsx_native_replay_pass_record(
    rsx_native_replay_pass* pass,
    ID3D12GraphicsCommandList* list,
    ID3D12GraphicsCommandList9* bias_list)
{
    D3D12_HEAP_PROPERTIES heap;
    D3D12_RESOURCE_DESC desc;
    void* mapped = NULL;
    D3D12_RANGE read_range = {0, 0};
    D3D12_RANGE write_range;
    D3D12_RESOURCE_ALLOCATION_INFO upload_allocation;
    uint32_t i;
    UINT64 gpu_address;
    if (!pass || pass->state != REPLAY_CAPTURING || !list ||
        !pass->payload.record_count)
        return replay_fail(pass, "invalid_replay_state_or_empty_pass");
    if (!rsx_native_draw_payload_seal(&pass->payload))
        return replay_fail(pass, "payload_seal_failed");
    memset(&heap, 0, sizeof(heap));
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    memset(&desc, 0, sizeof(desc));
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = pass->payload.bytes_used;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    (void)pass->device->lpVtbl->GetResourceAllocationInfo(
        pass->device, &upload_allocation, 0, 1, &desc);
    if (!upload_allocation.SizeInBytes ||
        upload_allocation.SizeInBytes == UINT64_MAX ||
        upload_allocation.SizeInBytes >
            RSX_NATIVE_REPLAY_MAX_ALLOCATION - pass->owned_gpu_bytes)
        return replay_fail(pass, "replay_upload_allocation_limit");
    if (FAILED(pass->device->lpVtbl->CreateCommittedResource(
            pass->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
            (void**)&pass->upload)))
        return replay_fail(pass, "create_replay_upload");
    pass->owned_gpu_bytes += upload_allocation.SizeInBytes;
    if (FAILED(pass->upload->lpVtbl->Map(
            pass->upload, 0, &read_range, &mapped)) || !mapped)
        return replay_fail(pass, "map_replay_upload");
    memcpy(mapped, pass->payload.bytes, pass->payload.bytes_used);
    write_range.Begin = 0;
    write_range.End = pass->payload.bytes_used;
    pass->upload->lpVtbl->Unmap(pass->upload, 0, &write_range);
    gpu_address = pass->upload->lpVtbl->GetGPUVirtualAddress(pass->upload);

    for (i = 0; i < pass->payload.record_count; ++i) {
        rsx_native_draw_payload_record payload_record;
        rsx_native_draw_packet packet = pass->draws[i].packet;
        uint32_t c;
        if (!rsx_native_draw_payload_get_record(
                &pass->payload, i, &payload_record))
            return replay_fail(pass, "payload_record_missing");
        packet.list = list;
        packet.bias_list = bias_list;
        packet.descriptor_heaps[0] = pass->srv_heap;
        packet.descriptor_heaps[1] = pass->sampler_heap;
        packet.srv_table = pass->draws[i].srv_table;
        packet.sampler_table = pass->draws[i].sampler_table;
        packet.vertex_texture_table = pass->draws[i].vtex_table;
        packet.has_vertex_texture_table =
            pass->draws[i].packet.has_vertex_texture_table;
        for (c = 0; c < pass->color_count; ++c)
            packet.render_targets[c] = pass->replay_rtv[c];
        if (packet.has_depth_stencil)
            packet.depth_stencil = pass->replay_dsv;
        if (packet.has_vertex_buffer && payload_record.vertex_size)
            packet.vertex_buffer.BufferLocation = gpu_address + payload_record.vertex_offset;
        else
            packet.vertex_buffer.BufferLocation = 0;
        if (packet.indexed)
            packet.index_buffer.BufferLocation = gpu_address + payload_record.index_offset;
        else
            packet.index_buffer.BufferLocation = 0;
        packet.vertex_constant_buffer = gpu_address + payload_record.vertex_constants_offset;
        if (packet.has_pixel_constant_buffer)
            packet.pixel_constant_buffer = gpu_address + payload_record.pixel_constants_offset;
        else
            packet.pixel_constant_buffer = 0;
        if (packet.has_pull_shader_resource)
            packet.pull_shader_resource = gpu_address + payload_record.vertex_offset;
        else
            packet.pull_shader_resource = 0;
        rsx_native_draw_packet_begin(&packet);
        rsx_native_draw_packet_targets(&packet);
        rsx_native_draw_packet_bind(&packet);
        rsx_native_draw_packet_draw(&packet);
        rsx_native_draw_packet_end(&packet);
    }
    pass->state = REPLAY_RECORDED;
    return 1;
}

int rsx_native_replay_pass_record_compare(
    rsx_native_replay_pass* pass,
    ID3D12GraphicsCommandList* list)
{
    uint32_t i;
    if (!pass || pass->state != REPLAY_RECORDED || !list)
        return replay_fail(pass, "invalid_compare_record_state");
    for (i = 0; i < pass->color_count; ++i) {
        D3D12_RESOURCE_DESC desc;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT original_fp;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT replay_fp;
        UINT rows;
        UINT64 row_bytes, total_bytes;
        UINT64 replay_offset;
        D3D12_HEAP_PROPERTIES heap;
        D3D12_RESOURCE_DESC buffer;
        D3D12_RESOURCE_ALLOCATION_INFO readback_allocation;
        ID3D12Resource* readback = NULL;
        D3D12_TEXTURE_COPY_LOCATION src;
        D3D12_TEXTURE_COPY_LOCATION dst;
        D3D12_RESOURCE_BARRIER bars[2];
        pass->original_color[i]->lpVtbl->GetDesc(pass->original_color[i], &desc);
        pass->device->lpVtbl->GetCopyableFootprints(
            pass->device, &desc, 0, 1, 0, &original_fp, &rows,
            &row_bytes, &total_bytes);
        if (!rows || !row_bytes || !total_bytes || total_bytes > RSX_NATIVE_REPLAY_MAX_ALLOCATION)
            return replay_fail(pass, "invalid_readback_footprint");
        replay_offset = (total_bytes + 511u) & ~511ull;
        pass->device->lpVtbl->GetCopyableFootprints(
            pass->device, &desc, 0, 1, replay_offset, &replay_fp,
            NULL, NULL, NULL);
        if (replay_offset > RSX_NATIVE_REPLAY_MAX_ALLOCATION - total_bytes ||
            pass->readback_bytes > RSX_NATIVE_REPLAY_MAX_ALLOCATION - replay_offset - total_bytes)
            return replay_fail(pass, "readback_limit");
        memset(&heap, 0, sizeof(heap));
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        memset(&buffer, 0, sizeof(buffer));
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = replay_offset + total_bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        (void)pass->device->lpVtbl->GetResourceAllocationInfo(
            pass->device, &readback_allocation, 0, 1, &buffer);
        if (!readback_allocation.SizeInBytes ||
            readback_allocation.SizeInBytes == UINT64_MAX ||
            readback_allocation.SizeInBytes >
                RSX_NATIVE_REPLAY_MAX_ALLOCATION - pass->owned_gpu_bytes)
            return replay_fail(pass, "readback_allocation_limit");
        if (FAILED(pass->device->lpVtbl->CreateCommittedResource(
                pass->device, &heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                (void**)&readback)))
            return replay_fail(pass, "create_readback_resource");
        pass->owned_gpu_bytes += readback_allocation.SizeInBytes;
        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));
        src.pResource = pass->original_color[i];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = original_fp;
        memset(bars, 0, sizeof(bars));
        bars[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[0].Transition.pResource = pass->original_color[i];
        bars[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bars[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bars[1].Transition.pResource = pass->replay_color[i];
        bars[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->lpVtbl->ResourceBarrier(list, 2, bars);
        list->lpVtbl->CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
        src.pResource = pass->replay_color[i];
        dst.PlacedFootprint = replay_fp;
        list->lpVtbl->CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
        bars[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bars[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bars[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bars[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->lpVtbl->ResourceBarrier(list, 2, bars);
        pass->readback[i] = readback;
        pass->color_readback[i].resource = readback;
        pass->color_readback[i].original_footprint = original_fp;
        pass->color_readback[i].replay_footprint = replay_fp;
        pass->color_readback[i].rows = rows;
        pass->color_readback[i].row_bytes = row_bytes;
        pass->color_readback[i].total_bytes = total_bytes;
        pass->readback_bytes += replay_offset + total_bytes;
    }
    pass->state = REPLAY_COMPARE_RECORDED;
    return 1;
}

int rsx_native_replay_pass_compare(
    rsx_native_replay_pass* pass,
    rsx_native_replay_compare_result* result)
{
    uint32_t i;
    rsx_native_replay_compare_result local;
    if (!pass || pass->state != REPLAY_COMPARE_RECORDED || !result)
        return replay_fail(pass, "invalid_compare_state");
    memset(&local, 0, sizeof(local));
    for (i = 0; i < pass->color_count; ++i) {
        replay_color_readback* rb = &pass->color_readback[i];
        D3D12_RANGE range;
        const uint8_t* mapped = NULL;
        UINT row;
        if (!rb->resource)
            return replay_fail(pass, "readback_missing");
        range.Begin = 0;
        range.End = (SIZE_T)(rb->total_bytes + rb->replay_footprint.Offset);
        if (FAILED(rb->resource->lpVtbl->Map(
                rb->resource, 0, &range, (void**)&mapped)) || !mapped)
            return replay_fail(pass, "map_readback");
        for (row = 0; row < rb->rows; ++row) {
            const uint8_t* a = mapped + rb->original_footprint.Offset +
                (SIZE_T)row * rb->original_footprint.Footprint.RowPitch;
            const uint8_t* b = mapped + rb->replay_footprint.Offset +
                (SIZE_T)row * rb->replay_footprint.Footprint.RowPitch;
            UINT64 col;
            for (col = 0; col < rb->row_bytes; ++col) {
                if (a[col] != b[col])
                    local.mismatch_bytes++;
            }
            local.compared_bytes += rb->row_bytes;
        }
        {
            D3D12_RANGE no_write = {0, 0};
            rb->resource->lpVtbl->Unmap(rb->resource, 0, &no_write);
        }
        local.compared_targets++;
    }
    *result = local;
    return 1;
}

uint32_t rsx_native_replay_pass_draw_count(
    const rsx_native_replay_pass* pass)
{
    return pass ? pass->payload.record_count : 0;
}

const char* rsx_native_replay_pass_error(
    const rsx_native_replay_pass* pass)
{
    return pass && pass->error[0] ? pass->error : "";
}

void rsx_native_replay_pass_destroy(rsx_native_replay_pass* pass)
{
    uint32_t i;
    if (!pass) return;
    for (i = 0; i < pass->payload.record_count; ++i) {
        ID3D12PipelineState* pso = pass->draws ? pass->draws[i].packet.pipeline_state : NULL;
        ID3D12RootSignature* rootsig = pass->draws ? pass->draws[i].packet.root_signature : NULL;
        if (pso) pso->lpVtbl->Release(pso);
        if (rootsig) rootsig->lpVtbl->Release(rootsig);
    }
    for (i = 0; i < pass->sampled_ref_count; ++i)
        if (pass->sampled_refs[i])
            pass->sampled_refs[i]->lpVtbl->Release(pass->sampled_refs[i]);
    for (i = 0; i < RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS; ++i) {
        if (pass->original_color[i]) pass->original_color[i]->lpVtbl->Release(pass->original_color[i]);
        if (pass->replay_color[i]) pass->replay_color[i]->lpVtbl->Release(pass->replay_color[i]);
        if (pass->readback[i]) pass->readback[i]->lpVtbl->Release(pass->readback[i]);
    }
    if (pass->original_depth) pass->original_depth->lpVtbl->Release(pass->original_depth);
    if (pass->replay_depth) pass->replay_depth->lpVtbl->Release(pass->replay_depth);
    if (pass->upload) pass->upload->lpVtbl->Release(pass->upload);
    if (pass->srv_heap) pass->srv_heap->lpVtbl->Release(pass->srv_heap);
    if (pass->sampler_heap) pass->sampler_heap->lpVtbl->Release(pass->sampler_heap);
    if (pass->rtv_heap) pass->rtv_heap->lpVtbl->Release(pass->rtv_heap);
    if (pass->dsv_heap) pass->dsv_heap->lpVtbl->Release(pass->dsv_heap);
    if (pass->device) pass->device->lpVtbl->Release(pass->device);
    rsx_native_draw_payload_destroy(&pass->payload);
    free(pass->draws);
    free(pass->sampled_refs);
    free(pass);
}

#else
/* Keep the SDK's video/*.c source glob warning-clean on non-Windows builds. */
typedef int rsx_native_replay_pass_non_windows_translation_unit;
#endif /* _WIN32 */
