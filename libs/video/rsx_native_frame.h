#ifndef PS3RECOMP_RSX_NATIVE_FRAME_H
#define PS3RECOMP_RSX_NATIVE_FRAME_H

/* Bounded ordered recorder for a complete native D3D12 frame candidate.
 * Capture is explicit: any unsupported operation must poison the frame via
 * mark_unsupported; callers must not silently omit commands or writes. */

#include <d3d12.h>
#include <dxgi1_6.h>
#include <stddef.h>
#include <stdint.h>
#include "rsx_native_draw_packet.h"
#include "rsx_native_draw_payload.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NATIVE_FRAME_MAX_RESOURCES 256u
#define RSX_NATIVE_FRAME_MAX_EVENTS 16384u
#define RSX_NATIVE_FRAME_MAX_TABLES 4u
#define RSX_NATIVE_FRAME_MAX_BINDINGS 16u
#define RSX_NATIVE_FRAME_MAX_ROOT_CONSTANTS 16u
#define RSX_NATIVE_FRAME_MAX_GPU_BYTES (UINT64_C(2) * 1024u * 1024u * 1024u)

typedef struct rsx_native_frame rsx_native_frame;

typedef enum rsx_native_frame_state {
    RSX_NATIVE_FRAME_OPEN = 1,
    RSX_NATIVE_FRAME_SEALED = 2,
    RSX_NATIVE_FRAME_FAILED = 3,
    RSX_NATIVE_FRAME_REPLAY_RECORDED = 4,
    RSX_NATIVE_FRAME_COMPARE_RECORDED = 5
} rsx_native_frame_state;

typedef enum rsx_native_frame_event_kind {
    RSX_NATIVE_FRAME_DRAW = 1,
    RSX_NATIVE_FRAME_CLEAR_COLOR = 2,
    RSX_NATIVE_FRAME_CLEAR_DEPTH = 3,
    RSX_NATIVE_FRAME_COPY = 4,
    RSX_NATIVE_FRAME_COMPUTE = 5,
    RSX_NATIVE_FRAME_UPLOAD = 6
} rsx_native_frame_event_kind;

typedef struct rsx_native_frame_resource_ref {
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES state;
    /* Mutable resources are cloned once on first use and the same clone is
     * used by every subsequent event. Immutable inputs must remain unchanged
     * through replay. */
    uint32_t mutable_resource;
} rsx_native_frame_resource_ref;

typedef struct rsx_native_frame_color_target {
    rsx_native_frame_resource_ref ref;
    D3D12_RENDER_TARGET_VIEW_DESC view;
} rsx_native_frame_color_target;

typedef struct rsx_native_frame_depth_target {
    rsx_native_frame_resource_ref ref;
    D3D12_DEPTH_STENCIL_VIEW_DESC view;
} rsx_native_frame_depth_target;

typedef enum rsx_native_frame_binding_kind {
    RSX_NATIVE_FRAME_BINDING_SRV = 1,
    RSX_NATIVE_FRAME_BINDING_UAV = 2
} rsx_native_frame_binding_kind;

typedef struct rsx_native_frame_binding {
    uint32_t enabled;
    uint32_t kind;
    rsx_native_frame_resource_ref ref;
    /* For disabled bindings, this CPU handle is copied into frame-owned
     * descriptor storage during capture and is not used after the call. */
    D3D12_CPU_DESCRIPTOR_HANDLE immutable_descriptor;
    union {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
    } view;
} rsx_native_frame_binding;

typedef struct rsx_native_frame_compute_table {
    uint32_t root_index;
    uint32_t binding_count;
    rsx_native_frame_binding bindings[RSX_NATIVE_FRAME_MAX_BINDINGS];
} rsx_native_frame_compute_table;

typedef struct rsx_native_frame_compute_constants {
    uint32_t root_index;
    uint32_t destination_offset;
    uint32_t value_count;
    uint32_t values[RSX_NATIVE_FRAME_MAX_ROOT_CONSTANTS];
} rsx_native_frame_compute_constants;

typedef struct rsx_native_frame_compute_desc {
    ID3D12PipelineState* pipeline_state;
    ID3D12RootSignature* root_signature;
    uint32_t table_count;
    rsx_native_frame_compute_table tables[RSX_NATIVE_FRAME_MAX_TABLES];
    uint32_t constants_count;
    rsx_native_frame_compute_constants constants[RSX_NATIVE_FRAME_MAX_ROOT_CONSTANTS];
    UINT groups_x, groups_y, groups_z;
} rsx_native_frame_compute_desc;

typedef struct rsx_native_frame_upload_desc {
    rsx_native_frame_resource_ref destination;
    UINT subresource;
    DXGI_FORMAT format;
    UINT width, height, depth;
    size_t source_row_pitch;
    const void* source_bytes;
    size_t source_size;
} rsx_native_frame_upload_desc;

typedef struct rsx_native_frame_stats {
    rsx_native_frame_state state;
    uint32_t event_count;
    uint32_t resource_count;
    uint32_t draw_count, color_clear_count, depth_clear_count;
    uint32_t copy_count, compute_count, upload_count;
    uint32_t unsupported_count;
    uint64_t owned_gpu_bytes;
    uint64_t owned_upload_bytes;
    uint64_t compared_bytes;
    uint64_t mismatch_bytes;
    uint32_t color_parity_checked;
    uint32_t depth_parity_checked; /* Currently zero: depth readback is pending. */
    uint32_t caller_coverage_verified;
    const char* first_failure;
} rsx_native_frame_stats;

/* Limits are hard-capped by the constants above. A frame and its retained
 * resources must not be destroyed until replay and comparison GPU work has
 * completed. Device and command lists are borrowed. */
rsx_native_frame* rsx_native_frame_create(ID3D12Device* device,
    uint32_t max_events, uint64_t max_owned_upload_bytes);
void rsx_native_frame_destroy(rsx_native_frame* frame);
void rsx_native_frame_mark_unsupported(rsx_native_frame* frame,
    const char* reason);
/* Caller sets this only after every frame-critical operation class is hooked
 * or has an explicit fail-closed invalidator. Capture itself cannot discover
 * commands issued outside its API. */
int rsx_native_frame_set_coverage_verified(rsx_native_frame* frame);

int rsx_native_frame_capture_draw(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* live_list,
    const rsx_native_draw_packet* packet,
    const rsx_native_draw_payload_store* payload,
    const rsx_native_frame_color_target* colors, UINT color_count,
    const rsx_native_frame_depth_target* depth,
    const rsx_native_frame_binding srv_bindings[RSX_NATIVE_FRAME_MAX_BINDINGS],
    const D3D12_CPU_DESCRIPTOR_HANDLE sampler_descriptors[RSX_NATIVE_FRAME_MAX_BINDINGS],
    const rsx_native_frame_binding vertex_texture_bindings[RSX_NATIVE_FRAME_MAX_BINDINGS]);

int rsx_native_frame_capture_clear_color(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* live_list,
    const rsx_native_frame_color_target* target, DXGI_FORMAT format,
    UINT width, UINT height, const D3D12_RECT* rect, UINT channel_mask,
    const FLOAT color[4]);
int rsx_native_frame_capture_clear_depth(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* live_list,
    const rsx_native_frame_depth_target* target, const D3D12_RECT* rect,
    FLOAT depth, UINT8 stencil, D3D12_CLEAR_FLAGS flags);

int rsx_native_frame_capture_copy(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* live_list,
    rsx_native_frame_resource_ref source, UINT source_subresource,
    const D3D12_BOX* source_box,
    rsx_native_frame_resource_ref destination, UINT destination_subresource,
    UINT dst_x, UINT dst_y, UINT dst_z);
int rsx_native_frame_capture_copy_resource(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* live_list,
    rsx_native_frame_resource_ref source,
    rsx_native_frame_resource_ref destination);

int rsx_native_frame_capture_compute(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* live_list,
    const rsx_native_frame_compute_desc* compute);

/* One upload event describes one placed-footprint-compatible subresource.
 * Unsupported formats/layouts fail closed; callers should enumerate mips and
 * array faces as separate events. Source rows are copied before returning. */
int rsx_native_frame_capture_upload(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* live_list,
    const rsx_native_frame_upload_desc* upload);

int rsx_native_frame_seal(rsx_native_frame* frame);
int rsx_native_frame_set_compare_target(rsx_native_frame* frame,
    rsx_native_frame_resource_ref target,
    const D3D12_RENDER_TARGET_VIEW_DESC* view);
/* Call after the original frame's final writes and before reusing its target.
 * It snapshots the original pixels on the live list so later source mutation
 * cannot weaken parity comparison. Caller must submit/wait before reading. */
int rsx_native_frame_capture_compare_reference(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* live_list,
    rsx_native_frame_resource_ref target,
    const D3D12_RENDER_TARGET_VIEW_DESC* view);
int rsx_native_frame_record_replay(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* replay_list,
    ID3D12GraphicsCommandList9* bias_list);
int rsx_native_frame_record_compare(rsx_native_frame* frame,
    ID3D12GraphicsCommandList* compare_list);
int rsx_native_frame_get_compare_result(const rsx_native_frame* frame,
    uint64_t* compared_bytes, uint64_t* mismatch_bytes);
void rsx_native_frame_get_stats(const rsx_native_frame* frame,
    rsx_native_frame_stats* stats);

#ifdef __cplusplus
}
#endif
#endif
