#include "rsx_vp_burst.h"

#define VP_METHOD RSX_VP_BURST_METHOD

static int is_exact_vp_packet(u32 subchannel, u32 method)
{
    return subchannel == 0 && method == VP_METHOD;
}

int rsx_vp_burst_dispatch_32(rsx_dispatch *rsx, u32 subchannel, u32 method,
                             const u32 words[RSX_VP_BURST_DWORDS])
{
    if (!rsx || !words || !is_exact_vp_packet(subchannel, method))
        return 0;

    /* Equivalent to 32 rsx_dispatch_method calls, including their register
     * and seen[] replication. This window has no callbacks or other decoding. */
    u32 load = rsx->regs[0x1E9Cu >> 2];
    for (u32 i = 0; i < RSX_VP_BURST_DWORDS; ++i) {
        const u32 m = VP_METHOD + i * 4u;
        const u32 idx = m >> 2;
        rsx->regs[idx] = words[i];
        rsx->seen[idx]++;

        const u32 lane = i & 3u;
        const u32 pos = load * 4u + lane;
        if (pos < RSX_DSP_VP_WORDS && rsx->vp[pos] != words[i]) {
            rsx->vp[pos] = words[i];
            rsx->vp_metadata_dirty = 1;
        }
        if (lane == 3u)
            ++load;
    }
    rsx->regs[0x1E9Cu >> 2] = load;
    return 1;
}

int rsx_vp_burst_process_32(rsx_state *state, u32 subchannel, u32 method,
                            const u32 words[RSX_VP_BURST_DWORDS])
{
    if (!state || !words || !is_exact_vp_packet(subchannel, method))
        return 0;

    /* Reject cursors whose old w+4 capacity expression would wrap. The
     * generic fallback also uses a nonwrapping capacity check. */
    if (state->vp_ucode_write > UINT32_MAX - 4u)
        return 0;

    /* rsx_process_method receives vm_read32's host-endian numeric word. Its
     * VP upload stores that numeric value in little-endian byte order. Preserve
     * its cursor, high-water, dirty and no-room behavior word by word. */
    for (u32 i = 0; i < RSX_VP_BURST_DWORDS; ++i) {
        const u32 w = state->vp_ucode_write;
        if (w + 4u <= sizeof(state->vp_ucode)) {
            const u32 data = words[i];
            state->vp_ucode[w + 0u] = (u8)data;
            state->vp_ucode[w + 1u] = (u8)(data >> 8);
            state->vp_ucode[w + 2u] = (u8)(data >> 16);
            state->vp_ucode[w + 3u] = (u8)(data >> 24);
            state->vp_ucode_write = w + 4u;
            if (w + 4u > state->vp_ucode_bytes)
                state->vp_ucode_bytes = w + 4u;
            state->vp_dirty = 1;
        }
    }
    return 1;
}

