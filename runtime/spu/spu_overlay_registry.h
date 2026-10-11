#ifndef PS3RECOMP_SPU_OVERLAY_REGISTRY_H
#define PS3RECOMP_SPU_OVERLAY_REGISTRY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Register compiled content at startup; bind its relocated PPU staging span
 * after cellOvis has copied it. No title addresses or short signatures required. */
void spu_overlay_register_fingerprint(uint64_t fingerprint, uint32_t span, int image_id);
/* Content identity covers the whole segment; code ownership can end before
 * its writable data tail. code_span is relative to the segment's LS base. */
void spu_overlay_register_fingerprint_range(uint64_t fingerprint, uint32_t span,
                                             uint32_t code_span, int image_id);
/* A PIC program may have translations at several observed LS allocations. */
void spu_overlay_register_fingerprint_at(uint64_t fingerprint, uint32_t span,
                                        uint32_t code_span, uint32_t lsa, int image_id);
void spu_overlay_bind(uint32_t ea, const uint8_t* bytes, uint32_t span);
/* Recognize complete SPU ELF reads and bind their loaded segment sources. */
void spu_overlay_bind_elf(uint32_t ea, const uint8_t* bytes, uint32_t size);
/* Bind compiled SPU programs embedded in a file-backed PPU load segment. */
void spu_overlay_bind_embedded(uint32_t ea, const uint8_t* bytes, uint32_t size);
/* Match complete compiled segments at the start of a DMA block, which may
 * include trailing padding. No short instruction signatures are used. */
int spu_overlay_match_loaded(const uint8_t* bytes, uint32_t size, uint32_t lsa, uint32_t* code_span);
/* Enumerate complete programs within a DMA block. offset is the minimum
 * candidate offset on entry and the matched offset on success. */
int spu_overlay_match_loaded_range(const uint8_t* bytes, uint32_t size, uint32_t lsa,
                                  uint32_t* offset, uint32_t* code_span);
int spu_overlay_lookup_bound(uint32_t ea, uint32_t* span);
int spu_overlay_lookup_bound_at(uint32_t ea, uint32_t lsa, uint32_t* span);
#ifdef __cplusplus
}
#endif
#endif
