/* Retail one-argument ABI: no output pointer or Init prerequisite. */
#include <assert.h>
#include <stdlib.h>
#include "../cellOvis.c"
void spu_overlay_bind(uint32_t ea, const uint8_t* bytes, uint32_t span)
{ assert(bytes == vm_base + ea); assert(span > 0); }
uint8_t* vm_base;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void spu_lockline_lock(void) { }
void spu_lockline_unlock(void) { }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t v, int w) { (void)ea; (void)v; (void)w; }
int main(void)
{
    vm_base = calloc(1, 0x10000); assert(vm_base);
    const u32 elf = 0x1000, ph = elf + 0x40;
    vm_write32(elf, 0x7F454C46); vm_write8(elf + 4, 1); vm_write8(elf + 5, 2);
    vm_write16(elf + 18, 23); vm_write32(elf + 28, 0x40);
    vm_write16(elf + 42, 32); vm_write16(elf + 44, 2);
    vm_write32(ph, 1); vm_write32(ph + 8, 0x100); vm_write32(ph + 20, 0x100);
    vm_write32(ph + 32, 1); vm_write32(ph + 40, 0x200); vm_write32(ph + 52, 0x80);
    assert(cellOvisGetOverlayTableSize(elf) == 0);
    assert(cellOvisInitializeOverlayTable(0x2000, elf) == CELL_OK);
    /* Two overlays share a VMA but occupy distinct physical staging ranges. */
    vm_write32(ph + 32 + 8, 0x100);
    vm_write32(ph + 32 + 12, 0x40000);
    vm_write32(ph + 32 + 4, 0x200);
    vm_write32(ph + 32 + 16, 0x80);
    memset(vm_base + elf + 0x200, 0xAB, 0x80);
    assert(cellOvisGetOverlayTableSize(elf) == 0x80);
    memset(vm_base + 0x2000, 0xCD, 0x100);
    assert(cellOvisInitializeOverlayTable(0x2001, elf) == (s32)CELL_OVIS_ERROR_ALIGN);
    assert(cellOvisInitializeOverlayTable(0x2000, elf) == CELL_OK);
    assert(vm_base[0x2000] == 0xAB && vm_base[0x207F] == 0xAB);
    assert(vm_base[0x2080] == 0xCD);
    vm_write32(ph + 12, 0x40100); vm_write32(ph + 4, 0x300); vm_write32(ph + 16, 0x20);
    memset(vm_base + elf + 0x300, 0xEF, 0x20);
    assert(cellOvisGetOverlayTableSize(elf) == 0x120);
    assert(cellOvisInitializeOverlayTable(0x2000, elf) == CELL_OK);
    assert(vm_base[0x20FF] == 0xCD && vm_base[0x2100] == 0xEF && vm_base[0x211F] == 0xEF);
    assert(cellOvisInitializeOverlayTable(0xFFFFFF80, elf) == (s32)CELL_OVIS_ERROR_INVAL);
    vm_write16(elf + 42, 16);
    assert(cellOvisGetOverlayTableSize(elf) == (s32)CELL_OVIS_ERROR_INVAL);
    assert(cellOvisGetOverlayTableSize(0) == (s32)CELL_OVIS_ERROR_INVAL);
    assert(cellOvisGetOverlayTableSize(0xFFFFFFF0) == (s32)CELL_OVIS_ERROR_INVAL);
    vm_write32(elf, 0);
    assert(cellOvisGetOverlayTableSize(elf) == (s32)CELL_OVIS_ERROR_INVAL);
    free(vm_base);
    puts("Overlay ELF query ABI and staging-copy checks passed");
    return 0;
}
