/*
 * Exact consumer-side kernel for one 32-dword NV4097 VP upload packet.
 *
 * The caller owns FIFO header/type/count validation, data-address validation,
 * host-endian vm_read32 conversion, and GET advancement. `words` must be a
 * separate array of 32 host-endian numeric values; it must not alias `rsx` or
 * `state`. The helpers accept only subchannel 0 and base method 0x0B80.
 */
#ifndef PS3RECOMP_RSX_VP_BURST_H
#define PS3RECOMP_RSX_VP_BURST_H

#include "rsx_commands.h"
#include "rsx_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_VP_BURST_METHOD 0x0B80u
#define RSX_VP_BURST_DWORDS 32u

/* Return 1 when accepted, 0 when rejected. Rejection leaves the complete
 * input struct unchanged. The state helper also rejects an overflowing cursor
 * (0xFFFFFFFC..0xFFFFFFFF). The generic handler must retain its nonwrapping
 * capacity check if used as fallback for these rejected cursors. */
int rsx_vp_burst_dispatch_32(rsx_dispatch *rsx, u32 subchannel, u32 method,
                             const u32 words[RSX_VP_BURST_DWORDS]);
int rsx_vp_burst_process_32(rsx_state *state, u32 subchannel, u32 method,
                            const u32 words[RSX_VP_BURST_DWORDS]);

#ifdef __cplusplus
}
#endif
#endif
