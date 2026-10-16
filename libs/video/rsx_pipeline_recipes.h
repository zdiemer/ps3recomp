#ifndef RSX_PIPELINE_RECIPES_H
#define RSX_PIPELINE_RECIPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#else
typedef struct ID3D12Device ID3D12Device;
typedef struct ID3D12RootSignature ID3D12RootSignature;
typedef struct ID3D12PipelineState ID3D12PipelineState;
typedef struct D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12_GRAPHICS_PIPELINE_STATE_DESC;
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_pipeline_recipes rsx_pipeline_recipes;

/* Bound both persistent and resident recipes. Archives above these limits are
 * rejected before any graphics pipeline is created. */
#define RSX_PIPELINE_RECIPES_MAX_ENTRIES 2048u
#define RSX_PIPELINE_RECIPES_MAX_ARCHIVE_BYTES (256u * 1024u * 1024u)

typedef struct rsx_pipeline_recipes_stats {
    uint32_t loaded;
    uint32_t remembered; /* successful remember calls, including duplicates */
    uint32_t prewarmed;
    uint32_t prewarm_failed;
    uint32_t find_hits;
    uint32_t find_misses;
    uint32_t corrupt_archives;
    uint32_t unsupported_recipes;
    uint32_t capacity_rejections;
    uint64_t owned_bytes;
    double prewarm_ms;
} rsx_pipeline_recipes_stats;

/* Create an empty cache. The root-signature serialization is copied and is
 * required to match the exact root object supplied in every descriptor. The
 * path is copied; no file I/O or driver calls occur here. */
rsx_pipeline_recipes* rsx_pipeline_recipes_create(
    ID3D12Device* device,
    ID3D12RootSignature* root_signature,
    const void* serialized_root_signature,
    size_t root_signature_size,
    const wchar_t* archive_path);

/* Validate archive structure, checksums, root identity, and supported recipe
 * layout before prewarming any entry. The D3D12 driver still validates shader
 * bytecode and descriptor enum values. A missing/corrupt archive is a nonfatal
 * cache miss and the cache can collect new recipes. */
int rsx_pipeline_recipes_load(
    rsx_pipeline_recipes* cache,
    rsx_pipeline_recipes_stats* stats);

/* Exact recipe lookup. Returned pipeline has one caller-owned COM reference.
 * A NULL result means either a miss or an unsupported descriptor. */
ID3D12PipelineState* rsx_pipeline_recipes_find(
    rsx_pipeline_recipes* cache,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc);

/* Remember an exact, supported recipe and retain one COM reference to pso.
 * Returns 1 if already present or stored, 0 if unsupported/full/OOM. */
int rsx_pipeline_recipes_remember(
    rsx_pipeline_recipes* cache,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    ID3D12PipelineState* pso);

/* Save to a unique same-directory temporary followed by atomic replacement. */
int rsx_pipeline_recipes_save_atomic(rsx_pipeline_recipes* cache);
void rsx_pipeline_recipes_get_stats(
    const rsx_pipeline_recipes* cache,
    rsx_pipeline_recipes_stats* stats);
void rsx_pipeline_recipes_destroy(rsx_pipeline_recipes* cache);

#ifdef __cplusplus
}
#endif
#endif
