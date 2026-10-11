/* Synthetic guest: verify the deprecated initializer's argument order,
 * packed event ABI, opaque object boundaries and reusable player slots. */
#include <assert.h>
#include "../cellSail.c"

uint8_t* vm_base;
ps3_guest_caller_fn g_ps3_guest_caller;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void spu_lockline_lock(void) { }
void spu_lockline_unlock(void) { }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t v, int w) { (void)ea; (void)v; (void)w; }

static unsigned calls, boot_completed;
static void callback(uint32_t opd, uint64_t user, uint64_t event, uint64_t value,
                     uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)value; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    assert(opd == 0x4000 && user == 0x12345678);
    assert((event >> 32) == 2 || (event >> 32) == 3);
    if (event == (((u64)2 << 32) | 1)) boot_completed++;
    calls++;
}

int main(void)
{
    vm_base = (uint8_t*)malloc(0x10000);
    assert(vm_base);
    memset(vm_base, 0xCD, 0x10000);
    g_ps3_guest_caller = callback;
    assert(cellSailPlayerInitialize(0, 1, 0x2000, 0x4000, 0, 2, 1, 0, 0) < 0);
    assert(cellSailPlayerInitialize(0x1000, 99, 0x2000, 0x4000, 0, 2, 1, 0, 0) < 0);
    assert(calls == 0);
    assert(cellSailPlayerInitialize(0x1000, 1, 0x2000, 0x4000,
                                  0x12345678, 2, 1, 0, 0x5000) == CELL_OK);
    assert(calls == 1);
    /* An opaque player must not overwrite the allocator or adjacent objects. */
    for (unsigned i = 0; i < 0x10000; i++) assert(vm_base[i] == 0xCD);
    int slot = sail_player_slot(0x1000);
    assert(slot >= 0 && s_sail_players[slot].spurs_ea == 0x5000);
    assert(s_sail_players[slot].max_audio == 2 && s_sail_players[slot].max_video == 1);
    assert(cellSailPlayerInitialize(0x1000, 1, 0x2000, 0x4000, 0, 2, 1, 0, 0) < 0);
    assert(cellSailPlayerBoot(0x1000, 0) == CELL_OK);
    assert(calls == 4 && boot_completed == 1);
    const u64 p0 = 0x1122334455667788ull, p1 = 0x8877665544332211ull;
    assert(cellSailPlayerSetParameter(0x1000, 25, p0, p1) == CELL_OK);
    assert(cellSailPlayerGetParameter(0x1000, 25, 0x8000, 0x8010) == CELL_OK);
    assert(vm_read64(0x8000) == p0 && vm_read64(0x8010) == p1);
    assert(vm_base[0x7FFF] == 0xCD && vm_base[0x8008] == 0xCD && vm_base[0x8018] == 0xCD);
    assert(cellSailPlayerGetParameter(0x1000, 25, 0, 0x8010) < 0);
    assert(cellSailPlayerSetParameter(0x1000, 36, 0, 0) < 0);
    assert(cellSailPlayerFinalize(0x1000) == CELL_OK);
    assert(cellSailPlayerBoot(0x1000, 0) < 0);
    for (u32 i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        assert(cellSailPlayerInitialize(0x1000 + i * 0x400, 1, 0x2000, 0x4000,
                                      0x12345678, 2, 1, 0, 0) == CELL_OK);
    assert(cellSailPlayerInitialize(0x3000, 1, 0x2000, 0x4000,
                                  0x12345678, 2, 1, 0, 0) < 0);
    assert(cellSailGraphicsAdapterInitialize(0x6000, 0x4000, 0) == CELL_OK);
    assert(cellSailSoundAdapterInitialize(0x6400, 0x4000, 0) == CELL_OK);
    assert(cellSailGraphicsAdapterGetFrame2(0x6000, 0x9000, 0x9040, 0x9080, 2) ==
           (s32)CELL_SAIL_ERROR_EMPTY);
    assert(vm_read32(0x9000) == 0 && vm_read64(0x9010) == 0);
    assert(vm_read32(0x9040) == 0 && vm_read64(0x9080) == 0);
    assert(vm_base[0x8FFF] == 0xCD && vm_base[0x9018] == 0xCD && vm_base[0x9088] == 0xCD);
    assert(cellSailGraphicsAdapterGetFrame2(0x6000, 0x9000, 0, 0, 2) ==
           (s32)CELL_SAIL_ERROR_EMPTY);
    assert(cellSailSoundAdapterGetFrame(0x6400, 512, 0x9100) == (s32)CELL_SAIL_ERROR_EMPTY);
    assert(vm_read32(0x9100) == 0 && vm_base[0x9118] == 0xCD);
    assert(cellSailGraphicsAdapterFinalize(0x6000) == CELL_OK);
    assert(cellSailSoundAdapterFinalize(0x6400) == CELL_OK);
    assert(cellSailSoundAdapterGetFrame(0x6400, 512, 0x9100) < 0);
    free(vm_base);
    puts("SAIL lifecycle and callback ABI contract passed");
    return 0;
}
