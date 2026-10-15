#ifndef PS3RECOMP_RSX_NATIVE_REPLAY_PASS_H
#define PS3RECOMP_RSX_NATIVE_REPLAY_PASS_H

/* Experimental, bounded offscreen replay of one mutation-free draw pass.
 * This API is deliberately separate from the immediate draw packet recorder.
 * It owns copied CPU payloads, copied shader-visible descriptor tables, and
 * references to immutable sampled resources, but it does not own the caller's
 * source render targets or the command queue/fence. */

#include "rsx_native_draw_packet.h"
#include "rsx_native_draw_payload.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS 4u
#define RSX_NATIVE_REPLAY_TABLE_SIZE 16u
#define RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES 32u
#define RSX_NATIVE_REPLAY_MAX_SNAPSHOTS 4096u

typedef struct rsx_native_replay_pass rsx_native_replay_pass;

typedef struct rsx_native_replay_compare_result {
    uint64_t compared_bytes;
    uint64_t mismatch_bytes;
    uint32_t compared_targets;
} rsx_native_replay_compare_result;

/* Optional immutable resource-version capture for one SRV slot. source_state
 * is the exact state at capture time (capture runs after packet begin). The
 * current prototype supports single-mip Texture2D SRVs and clones the full
 * single-mip/single-slice resource. Supported typed formats are
 * R8G8B8A8_UNORM, R16G16B16A16_FLOAT, and R32_FLOAT; view format must match
 * the source format. Component swizzles such as RRR1 are retained. Repeated
 * (resource,generation) pairs share one clone; views remain per-slot. */
typedef struct rsx_native_replay_srv_snapshot {
    D3D12_RESOURCE_STATES source_state;
    D3D12_SHADER_RESOURCE_VIEW_DESC view;
    uint64_t generation;
    int enabled;
} rsx_native_replay_srv_snapshot;

/* Creates bounded CPU-side storage and fixed-size private descriptor heaps.
 * max_draws and max_payload_bytes are hard limits; failure returns NULL. */
rsx_native_replay_pass* rsx_native_replay_pass_create(
    ID3D12Device* device,
    uint32_t max_draws,
    size_t max_payload_bytes);

/* Clones initial target contents into private resources by recording copies
 * on list. Source colors must be in RENDER_TARGET state and depth, if present,
 * in DEPTH_WRITE state. The caller supplies the typed DSV view description.
 * Color ordering must match packet.render_targets for every captured draw. */
int rsx_native_replay_pass_begin(
    rsx_native_replay_pass* pass,
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* const color_resources[RSX_NATIVE_REPLAY_MAX_COLOR_TARGETS],
    uint32_t color_count,
    ID3D12Resource* depth_resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* depth_view);

/* Captures one fully populated packet immediately before its live bind. This
 * compatibility form accepts only packets without transitions and copies the
 * descriptor tables verbatim. */
int rsx_native_replay_pass_capture(
    rsx_native_replay_pass* pass,
    const rsx_native_draw_packet* packet,
    const rsx_native_draw_payload_input* input,
    const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    const D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    const D3D12_CPU_DESCRIPTOR_HANDLE vtex_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    ID3D12Resource* const sampled_resources[RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES]);

/* Snapshot form. CPU descriptor arrays each contain 16 handles; vtex_cpu may
 * be NULL only when the packet has no vertex-texture table. sampled_resources
 * maps pixel SRV slots [0,16), then vertex-texture slots [16,32). snapshots
 * uses the same slot mapping. For every enabled entry, sampled_resources must
 * identify the resource described by view. Sources must be single-mip,
 * single-slice Texture2D resources without ALLOW_DEPTH_STENCIL, and source
 * states must contain only PIXEL_SHADER_RESOURCE/NON_PIXEL_SHADER_RESOURCE.
 * Every packet transition resource must be represented by an enabled snapshot
 * whose source_state is PIXEL_SHADER_RESOURCE. The stored replay packet omits
 * those transitions because its snapshots are already shader-readable. The
 * enabled sources must retain the described contents until their copy command
 * is ordered; later content changes are safe once that copy is ordered and
 * their state changes follow the same command ordering. For disabled slots,
 * copied descriptors still reference borrowed resources, so their contents
 * must remain immutable through replay completion. All sources remain alive
 * through the caller's fence; cloned versions and descriptors are owned by
 * the pass through that fence. */
int rsx_native_replay_pass_capture_with_snapshots(
    rsx_native_replay_pass* pass,
    const rsx_native_draw_packet* packet,
    const rsx_native_draw_payload_input* input,
    const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    const D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    const D3D12_CPU_DESCRIPTOR_HANDLE vtex_cpu[RSX_NATIVE_REPLAY_TABLE_SIZE],
    ID3D12Resource* const sampled_resources[RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES],
    const rsx_native_replay_srv_snapshot snapshots[RSX_NATIVE_REPLAY_MAX_SAMPLED_RESOURCES]);

/* Records captured draws against private clone targets using an upload
 * resource containing the owned vertex/index/constants. Caller must submit
 * and wait for completion before destroying the pass or comparing readback. */
int rsx_native_replay_pass_record(
    rsx_native_replay_pass* pass,
    ID3D12GraphicsCommandList* list,
    ID3D12GraphicsCommandList9* bias_list);

/* Records color-output copies to private readback buffers and restores color
 * targets to RENDER_TARGET state. Caller submits and waits before compare().
 * Depth is replayed but deliberately not compared by this first prototype. */
int rsx_native_replay_pass_record_compare(
    rsx_native_replay_pass* pass,
    ID3D12GraphicsCommandList* list);

/* Reads completed color readbacks and compares active texel bytes (excluding
 * D3D row-pitch padding). Returns 1 on a completed comparison. */
int rsx_native_replay_pass_compare(
    rsx_native_replay_pass* pass,
    rsx_native_replay_compare_result* result);

uint32_t rsx_native_replay_pass_draw_count(
    const rsx_native_replay_pass* pass);
uint32_t rsx_native_replay_pass_snapshot_count(
    const rsx_native_replay_pass* pass);
uint64_t rsx_native_replay_pass_snapshot_bytes(
    const rsx_native_replay_pass* pass);
uint64_t rsx_native_replay_pass_snapshot_reuses(
    const rsx_native_replay_pass* pass);
const char* rsx_native_replay_pass_error(
    const rsx_native_replay_pass* pass);

/* Caller guarantees all commands recorded with pass resources have completed. */
void rsx_native_replay_pass_destroy(rsx_native_replay_pass* pass);

#ifdef __cplusplus
}
#endif

#endif
