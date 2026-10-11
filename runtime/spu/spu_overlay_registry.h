#ifndef PS3RECOMP_SPU_OVERLAY_REGISTRY_H
#define PS3RECOMP_SPU_OVERLAY_REGISTRY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Register compiled content at startup; bind its relocated PPU staging span
 * after cellOvis has copied it. No title addresses or short signatures required. */
void spu_overlay_register_fingerprint(uint64_t fingerprint, uint32_t span, int image_id);
void spu_overlay_bind(uint32_t ea, const uint8_t* bytes, uint32_t span);
int spu_overlay_lookup_bound(uint32_t ea, uint32_t* span);
#ifdef __cplusplus
}
#endif
#endif
