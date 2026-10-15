/* Actual production recorders + real Windows/D3D12 C COM declarations.
 * Standalone in a VS developer environment, via test_native_draw_packet.py:
 * clang-cl /nologo /TC /W4 /WX /O2 /I<video> /I<temp>
 *   test_native_draw_packet.c /Fo<temp>/test.obj /Fe<temp>/test.exe
 * No D3D device, graphics driver, game, or command submission is created.
 */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rsx_native_draw_packet.h"

enum command {
    BARRIER, CLEAR_DEPTH, TARGETS, HEAPS, PSO, BIAS, ROOT_SIGNATURE,
    PULL_SRV, VERTEX_CBV, PIXEL_CBV, SRV_TABLE, SAMPLER_TABLE, VTEX_TABLE,
    VIEWPORT, SCISSOR, STENCIL, VERTEX_BUFFER, TOPOLOGY, INDEX_BUFFER,
    DRAW_INDEXED, DRAW, CALLER_CLEAR_STATE, CALLER_DESCRIPTOR_COPY,
    CALLER_RECENT_METADATA, CALLER_LIFETIME
};
static enum command commands[64];
static unsigned command_count, barrier_index;
static int returning_transitions;
static const rsx_native_draw_packet* expected;
static ID3D12GraphicsCommandListVtbl list_vtable;
static ID3D12GraphicsCommandList9Vtbl bias_vtable;
static ID3D12GraphicsCommandList list;
static ID3D12GraphicsCommandList9 bias_list;
static ID3D12Resource resources[RSX_NATIVE_DRAW_MAX_TRANSITIONS];
static ID3D12DescriptorHeap heaps[2];
static ID3D12PipelineState pipeline;
static ID3D12RootSignature root_signature;

static void record(enum command command) {
    assert(command_count < sizeof(commands)/sizeof(commands[0]));
    commands[command_count++] = command;
}
static void check_list(ID3D12GraphicsCommandList* self) {
    assert(self == expected->list && self == &list);
}
static void STDMETHODCALLTYPE barrier(ID3D12GraphicsCommandList* self,
                                      UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    check_list(self);
    assert(count == 1 && barriers && barrier_index < expected->transition_count);
    assert(barriers[0].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
    assert(barriers[0].Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE);
    assert(barriers[0].Transition.pResource == expected->transition_resources[barrier_index++]);
    assert(barriers[0].Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    assert(barriers[0].Transition.StateBefore == (returning_transitions ?
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET));
    assert(barriers[0].Transition.StateAfter == (returning_transitions ?
        D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    record(BARRIER);
}
static void STDMETHODCALLTYPE clear_depth(ID3D12GraphicsCommandList* self,
    D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CLEAR_FLAGS flags,
    FLOAT depth, UINT8 stencil, UINT count, const D3D12_RECT* rectangles) {
    check_list(self);
    assert(expected->has_depth_stencil && expected->clear_depth_stencil);
    assert(handle.ptr == expected->depth_stencil.ptr);
    assert(flags == (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL));
    assert(depth == 1.0f && stencil == 0 && count == 0 && rectangles == NULL);
    record(CLEAR_DEPTH);
}
static void STDMETHODCALLTYPE targets(ID3D12GraphicsCommandList* self,
    UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL contiguous,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    check_list(self);
    assert(count == expected->render_target_count && !contiguous && handles);
    for (UINT i = 0; i < count; ++i) assert(handles[i].ptr == expected->render_targets[i].ptr);
    if (expected->has_depth_stencil) assert(depth && depth->ptr == expected->depth_stencil.ptr);
    else assert(depth == NULL);
    record(TARGETS);
}
static void STDMETHODCALLTYPE descriptor_heaps(ID3D12GraphicsCommandList* self,
    UINT count, ID3D12DescriptorHeap* const* descriptors) {
    check_list(self);
    assert(count == 2 && descriptors);
    assert(descriptors[0] == expected->descriptor_heaps[0]);
    assert(descriptors[1] == expected->descriptor_heaps[1]);
    record(HEAPS);
}
static void STDMETHODCALLTYPE pso(ID3D12GraphicsCommandList* self, ID3D12PipelineState* state) {
    check_list(self); assert(state == expected->pipeline_state); record(PSO);
}
static void STDMETHODCALLTYPE bias(ID3D12GraphicsCommandList9* self,
                                    FLOAT depth, FLOAT clamp, FLOAT slope) {
    assert(self == &bias_list && self == expected->bias_list && expected->apply_depth_bias);
    assert(depth == expected->depth_bias && clamp == 0.0f && slope == expected->slope_scaled_depth_bias);
    record(BIAS);
}
static void STDMETHODCALLTYPE root(ID3D12GraphicsCommandList* self, ID3D12RootSignature* signature) {
    check_list(self); assert(signature == expected->root_signature); record(ROOT_SIGNATURE);
}
static void STDMETHODCALLTYPE pull(ID3D12GraphicsCommandList* self, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    check_list(self);
    assert(expected->has_pull_shader_resource && index == 5 && address == expected->pull_shader_resource);
    record(PULL_SRV);
}
static void STDMETHODCALLTYPE cbv(ID3D12GraphicsCommandList* self, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    check_list(self);
    if (index == 0) { assert(address == expected->vertex_constant_buffer); record(VERTEX_CBV); }
    else {
        assert(index == 4 && expected->has_pixel_constant_buffer && address == expected->pixel_constant_buffer);
        record(PIXEL_CBV);
    }
}
static void STDMETHODCALLTYPE table(ID3D12GraphicsCommandList* self, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    check_list(self);
    if (index == 1) { assert(handle.ptr == expected->srv_table.ptr); record(SRV_TABLE); }
    else if (index == 2) { assert(handle.ptr == expected->sampler_table.ptr); record(SAMPLER_TABLE); }
    else {
        assert(index == 3 && expected->has_vertex_texture_table && handle.ptr == expected->vertex_texture_table.ptr);
        record(VTEX_TABLE);
    }
}
static void STDMETHODCALLTYPE viewport(ID3D12GraphicsCommandList* self, UINT count, const D3D12_VIEWPORT* view) {
    check_list(self); assert(count == 1 && view && !memcmp(view, &expected->viewport, sizeof(*view))); record(VIEWPORT);
}
static void STDMETHODCALLTYPE scissor(ID3D12GraphicsCommandList* self, UINT count, const D3D12_RECT* rectangle) {
    check_list(self); assert(count == 1 && rectangle && !memcmp(rectangle, &expected->scissor, sizeof(*rectangle))); record(SCISSOR);
}
static void STDMETHODCALLTYPE stencil(ID3D12GraphicsCommandList* self, UINT reference) {
    check_list(self); assert(reference == expected->stencil_reference); record(STENCIL);
}
static void STDMETHODCALLTYPE vertex_buffer(ID3D12GraphicsCommandList* self, UINT start, UINT count,
                                            const D3D12_VERTEX_BUFFER_VIEW* views) {
    check_list(self); assert(expected->has_vertex_buffer && start == 0 && count == 1 && views);
    assert(!memcmp(views, &expected->vertex_buffer, sizeof(*views))); record(VERTEX_BUFFER);
}
static void STDMETHODCALLTYPE topology(ID3D12GraphicsCommandList* self, D3D12_PRIMITIVE_TOPOLOGY value) {
    check_list(self); assert(value == expected->topology); record(TOPOLOGY);
}
static void STDMETHODCALLTYPE index_buffer(ID3D12GraphicsCommandList* self, const D3D12_INDEX_BUFFER_VIEW* view) {
    check_list(self); assert(expected->indexed && view && !memcmp(view, &expected->index_buffer, sizeof(*view))); record(INDEX_BUFFER);
}
static void check_draw(UINT count, UINT instances, UINT start, UINT first_instance) {
    assert(count == expected->vertex_or_index_count && instances == expected->instance_count);
    assert(start == expected->start_vertex_or_index && first_instance == expected->start_instance);
}
static void STDMETHODCALLTYPE draw_indexed(ID3D12GraphicsCommandList* self, UINT count, UINT instances,
                                           UINT start, INT base, UINT first_instance) {
    check_list(self); assert(expected->indexed && base == expected->base_vertex);
    check_draw(count, instances, start, first_instance); record(DRAW_INDEXED);
}
static void STDMETHODCALLTYPE draw(ID3D12GraphicsCommandList* self, UINT count, UINT instances,
                                   UINT start, UINT first_instance) {
    check_list(self); assert(!expected->indexed); check_draw(count, instances, start, first_instance); record(DRAW);
}

/* The include is the exact marked production range, not a rewritten sink. It
 * must compile without the renderer's g, guest memory or allocation helpers. */
#define SRV_TABLE_SIZE 16u
#include "native_draw_packet_actual.inc"

static void run_packet(const rsx_native_draw_packet* packet) {
    rsx_native_draw_packet saved;
    memcpy(&saved, packet, sizeof(saved));
    expected = packet; command_count = 0; barrier_index = 0; returning_transitions = 0;
    rsx_native_draw_packet_begin(packet);
    assert(!memcmp(packet, &saved, sizeof(saved)));
    assert(barrier_index == packet->transition_count);
    record(CALLER_CLEAR_STATE);
    rsx_native_draw_packet_targets(packet);
    assert(!memcmp(packet, &saved, sizeof(saved)));
    record(CALLER_DESCRIPTOR_COPY);
    rsx_native_draw_packet_bind(packet);
    assert(!memcmp(packet, &saved, sizeof(saved)));
    record(CALLER_RECENT_METADATA);
    rsx_native_draw_packet_draw(packet);
    assert(!memcmp(packet, &saved, sizeof(saved)));
    record(CALLER_LIFETIME);
    returning_transitions = 1; barrier_index = 0;
    rsx_native_draw_packet_end(packet);
    assert(!memcmp(packet, &saved, sizeof(saved)));
    assert(barrier_index == packet->transition_count);
    unsigned at = 0;
#define EXPECT(command) assert(at < command_count && commands[at++] == (command))
    for (UINT i = 0; i < packet->transition_count; ++i) EXPECT(BARRIER);
    if (packet->has_depth_stencil && packet->clear_depth_stencil) EXPECT(CLEAR_DEPTH);
    EXPECT(CALLER_CLEAR_STATE); EXPECT(TARGETS); EXPECT(HEAPS); EXPECT(CALLER_DESCRIPTOR_COPY);
    EXPECT(PSO);
    if (packet->apply_depth_bias && packet->bias_list) EXPECT(BIAS);
    EXPECT(ROOT_SIGNATURE);
    if (packet->has_pull_shader_resource) EXPECT(PULL_SRV);
    EXPECT(VERTEX_CBV);
    if (packet->has_pixel_constant_buffer) EXPECT(PIXEL_CBV);
    EXPECT(SRV_TABLE); EXPECT(SAMPLER_TABLE);
    if (packet->has_vertex_texture_table) EXPECT(VTEX_TABLE);
    EXPECT(VIEWPORT); EXPECT(SCISSOR); EXPECT(STENCIL);
    if (packet->has_vertex_buffer) EXPECT(VERTEX_BUFFER);
    EXPECT(TOPOLOGY); EXPECT(CALLER_RECENT_METADATA);
    if (packet->indexed) { EXPECT(INDEX_BUFFER); EXPECT(DRAW_INDEXED); }
    else EXPECT(DRAW);
    EXPECT(CALLER_LIFETIME);
    for (UINT i = 0; i < packet->transition_count; ++i) EXPECT(BARRIER);
    assert(at == command_count);
#undef EXPECT
}

static void initialize_packet(rsx_native_draw_packet* p, unsigned mask, UINT mrt, UINT transitions, unsigned vb_mode) {
    memset(p, 0, sizeof(*p));
    p->list = &list; p->bias_list = mask & 8u ? &bias_list : NULL;
    p->pipeline_state = &pipeline; p->root_signature = &root_signature;
    p->descriptor_heaps[0] = &heaps[0]; p->descriptor_heaps[1] = &heaps[1];
    p->srv_table.ptr = 0x10100000001ULL; p->sampler_table.ptr = 0x20200000002ULL;
    p->vertex_texture_table.ptr = 0x30300000003ULL; p->has_vertex_texture_table = (mask & 128u) != 0;
    p->render_target_count = mrt;
    for (UINT i = 0; i < RSX_NATIVE_DRAW_MAX_RENDER_TARGETS; ++i) p->render_targets[i].ptr = 0x100000000ULL + 37u*i;
    p->depth_stencil.ptr = 0x400000004ULL; p->has_depth_stencil = (mask & 2u) != 0;
    p->clear_depth_stencil = (mask & 4u) != 0;
    p->transition_count = transitions;
    for (UINT i = 0; i < RSX_NATIVE_DRAW_MAX_TRANSITIONS; ++i) p->transition_resources[i] = &resources[i];
    p->viewport.TopLeftX = -5.5f; p->viewport.TopLeftY = 13.25f;
    p->viewport.Width = mask & 256u ? 0.0f : 1920.0f; p->viewport.Height = mask & 256u ? 0.0f : 1080.0f;
    p->viewport.MinDepth = 0.125f; p->viewport.MaxDepth = 0.875f;
    p->scissor.left = -17; p->scissor.top = 29;
    p->scissor.right = mask & 512u ? p->scissor.left : 1001;
    p->scissor.bottom = mask & 512u ? p->scissor.top : 791;
    p->stencil_reference = (mask + transitions) & 255u;
    p->apply_depth_bias = (mask & 16u) != 0; p->depth_bias = -13.5f; p->slope_scaled_depth_bias = 2.75f;
    p->has_vertex_buffer = vb_mode != 0;
    p->vertex_buffer.BufferLocation = 0x50500000005ULL; p->vertex_buffer.SizeInBytes = 123456;
    p->vertex_buffer.StrideInBytes = vb_mode == 2 ? 36 : 0;
    p->index_buffer.BufferLocation = 0x60600000006ULL; p->index_buffer.SizeInBytes = 9876;
    p->index_buffer.Format = mask & 32u ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    p->indexed = (mask & 1u) != 0;
    p->topology = mask & 64u ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    p->vertex_or_index_count = 123; p->instance_count = 3; p->start_vertex_or_index = 17;
    p->base_vertex = -31; p->start_instance = 11;
    p->vertex_constant_buffer = 0x70700000007ULL; p->pixel_constant_buffer = 0x80800000008ULL;
    p->has_pixel_constant_buffer = (mask & 64u) != 0;
    p->has_pull_shader_resource = (mask & 32u) != 0; p->pull_shader_resource = 0x90900000009ULL;
}

int main(void) {
    list_vtable.ResourceBarrier = barrier; list_vtable.ClearDepthStencilView = clear_depth;
    list_vtable.OMSetRenderTargets = targets; list_vtable.SetDescriptorHeaps = descriptor_heaps;
    list_vtable.SetPipelineState = pso; list_vtable.SetGraphicsRootSignature = root;
    list_vtable.SetGraphicsRootShaderResourceView = pull; list_vtable.SetGraphicsRootConstantBufferView = cbv;
    list_vtable.SetGraphicsRootDescriptorTable = table; list_vtable.RSSetViewports = viewport;
    list_vtable.RSSetScissorRects = scissor; list_vtable.OMSetStencilRef = stencil;
    list_vtable.IASetVertexBuffers = vertex_buffer; list_vtable.IASetPrimitiveTopology = topology;
    list_vtable.IASetIndexBuffer = index_buffer; list_vtable.DrawIndexedInstanced = draw_indexed;
    list_vtable.DrawInstanced = draw; bias_vtable.RSSetDepthBias = bias;
    list.lpVtbl = &list_vtable; bias_list.lpVtbl = &bias_vtable;
    unsigned cases = 0;
    rsx_native_draw_packet packet;
    for (unsigned mask = 0; mask < 1024; ++mask)
        for (UINT mrt = 1; mrt <= RSX_NATIVE_DRAW_MAX_RENDER_TARGETS; ++mrt)
            for (UINT transitions = 0; transitions <= RSX_NATIVE_DRAW_MAX_TRANSITIONS; ++transitions)
                for (unsigned vb_mode = 0; vb_mode < 3; ++vb_mode) {
                    initialize_packet(&packet, mask, mrt, transitions, vb_mode);
                    run_packet(&packet); ++cases;
                }
    /* Zero-count draws are still recorded, and null inputs are explicit no-ops. */
    initialize_packet(&packet, 1023, 4, 16, 2);
    packet.vertex_or_index_count = 0; packet.instance_count = 0; run_packet(&packet);
    packet.indexed = 0; run_packet(&packet);
    packet.list = NULL; command_count = 0;
    rsx_native_draw_packet_begin(&packet); rsx_native_draw_packet_targets(&packet);
    rsx_native_draw_packet_bind(&packet); rsx_native_draw_packet_draw(&packet); rsx_native_draw_packet_end(&packet);
    rsx_native_draw_packet_begin(NULL); rsx_native_draw_packet_targets(NULL);
    rsx_native_draw_packet_bind(NULL); rsx_native_draw_packet_draw(NULL); rsx_native_draw_packet_end(NULL);
    assert(command_count == 0);
    printf("%u actual-source native draw packet cases passed; phase ordering, arguments, immutability and null guards passed\n", cases);
    return 0;
}
