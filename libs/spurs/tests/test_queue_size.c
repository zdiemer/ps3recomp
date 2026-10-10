/* Synthetic guest-memory contract test; does not depend on game files. */
#include <assert.h>
#include <stdlib.h>
#include "../cellSpursQueue.c"

uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
static int locked;
void spu_lockline_lock(void) { assert(!locked); locked = 1; }
void spu_lockline_unlock(void) { assert(locked); locked = 0; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t v, int w) { (void)ea; (void)v; (void)w; }

static void expect_size(u32 head, u32 tail, u32 depth, u32 expected)
{
    vm_write32(0x100, head);
    vm_write32(0x104, tail);
    vm_write32(0x10C, depth);
    vm_write32(0x200, 0xDEADBEEF);
    assert(cellSpursQueueSize(0x100, 0x200) == CELL_OK);
    assert(vm_read32(0x200) == expected);
    assert(vm_read32(0x100) == head && vm_read32(0x104) == tail);
    assert(!locked);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 0x1000);
    assert(vm_base);
    expect_size(0, 0, 8, 0);       /* empty */
    expect_size(0, 8, 8, 8);       /* full */
    expect_size(14, 2, 8, 4);      /* wrap around twice-depth boundary */
    expect_size(~14u, ~2u, 8, 4);  /* claimed indices */
    expect_size(0, 128, 512, 128); /* Haze's completion queue shape */
    assert(vm_base[0x203] == 128 && vm_base[0x200] == 0); /* BE output */
    assert(cellSpursQueueSize(0, 0x200) == CELL_SPURS_TASK_ERROR_NULL_POINTER);
    assert(cellSpursQueueSize(0x100, 0) == CELL_SPURS_TASK_ERROR_NULL_POINTER);
    assert(cellSpursQueueSize(0x101, 0x200) == CELL_SPURS_TASK_ERROR_ALIGN);
    assert(cellSpursQueueSize(0x100, 0x201) == CELL_SPURS_TASK_ERROR_ALIGN);
    vm_write32(0x10C, 0);
    vm_write32(0x200, 0xDEADBEEF);
    assert(cellSpursQueueSize(0x100, 0x200) == CELL_SPURS_TASK_ERROR_INVAL);
    assert(vm_read32(0x200) == 0xDEADBEEF); /* errors leave output untouched */
    vm_write32(0x10C, 8);
    vm_write32(0x100, 0);
    vm_write32(0x104, 9);
    assert(cellSpursQueueSize(0x100, 0x200) == CELL_SPURS_TASK_ERROR_INVAL);
    free(vm_base);
    return 0;
}
