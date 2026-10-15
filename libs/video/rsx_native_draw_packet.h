#ifndef PS3RECOMP_RSX_NATIVE_DRAW_PACKET_H
#define PS3RECOMP_RSX_NATIVE_DRAW_PACKET_H

/*
 * Immediate D3D12 recording packet for one already-resolved RSX draw.
 *
 * All interfaces, resources, descriptor handles, and GPU addresses are
 * borrowed. The caller fills the packet, then treats it as immutable through
 * begin/targets/bind/draw/end. No owner is retained by these helpers: the
 * caller must keep CPU-side objects valid through recording and keep GPU
 * resources, upload ranges, and descriptors valid until the submission fence
 * retires them. Transition and render-target counts must fit the fixed packet
 * arrays, and the caller must not reset/flush the list or recycle referenced
 * rings between phases. The caller must preserve original command order. This
 * is deliberately not an owned snapshot or a replay-safe packet format.
 */

#include <d3d12.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NATIVE_DRAW_MAX_RENDER_TARGETS 4u
#define RSX_NATIVE_DRAW_MAX_TRANSITIONS 16u

typedef struct rsx_native_draw_packet {
    ID3D12GraphicsCommandList* list;
    ID3D12GraphicsCommandList9* bias_list;

    ID3D12PipelineState* pipeline_state;
    ID3D12RootSignature* root_signature;

    ID3D12DescriptorHeap* descriptor_heaps[2];
    D3D12_GPU_DESCRIPTOR_HANDLE srv_table;
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table;
    D3D12_GPU_DESCRIPTOR_HANDLE vertex_texture_table;
    int has_vertex_texture_table;

    D3D12_CPU_DESCRIPTOR_HANDLE render_targets[RSX_NATIVE_DRAW_MAX_RENDER_TARGETS];
    UINT render_target_count;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil;
    int has_depth_stencil;
    int clear_depth_stencil;

    /* Resources transition RENDER_TARGET -> PIXEL_SHADER_RESOURCE before
     * this draw and back after it, in array order. */
    ID3D12Resource* transition_resources[RSX_NATIVE_DRAW_MAX_TRANSITIONS];
    UINT transition_count;

    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    UINT stencil_reference;

    int apply_depth_bias;
    FLOAT depth_bias;
    FLOAT slope_scaled_depth_bias;

    D3D12_VERTEX_BUFFER_VIEW vertex_buffer;
    int has_vertex_buffer;
    D3D12_INDEX_BUFFER_VIEW index_buffer;
    int indexed;

    D3D12_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_or_index_count;
    UINT instance_count;
    UINT start_vertex_or_index;
    INT base_vertex;
    UINT start_instance;

    D3D12_GPU_VIRTUAL_ADDRESS vertex_constant_buffer;
    int has_pixel_constant_buffer;
    D3D12_GPU_VIRTUAL_ADDRESS pixel_constant_buffer;
    int has_pull_shader_resource;
    D3D12_GPU_VIRTUAL_ADDRESS pull_shader_resource;
} rsx_native_draw_packet;

/* These functions only record commands using packet fields. They allocate no
 * memory, inspect no RSX/guest state, and do not submit or flush the list.
 * Caller order is begin, clear-state bookkeeping, targets, descriptor-table
 * and constant preparation, bind, draw, lifetime/stat updates, end. */
void rsx_native_draw_packet_begin(const rsx_native_draw_packet* packet);
void rsx_native_draw_packet_targets(const rsx_native_draw_packet* packet);
void rsx_native_draw_packet_bind(const rsx_native_draw_packet* packet);
void rsx_native_draw_packet_draw(const rsx_native_draw_packet* packet);
void rsx_native_draw_packet_end(const rsx_native_draw_packet* packet);

#ifdef __cplusplus
}
#endif

#endif
