/*
 * ps3recomp - cellOvis SPU overlay staging
 *
 * Retail ELF staging APIs are implemented; legacy convenience APIs are stubs.
 */

#include "cellOvis.h"
#include <stdio.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../../runtime/spu/spu_overlay_registry.h"

/* Internal state */

static int s_initialized = 0;
static u32 s_next_handle = 1;

/* API */

s32 cellOvisInit(void)
{
    printf("[cellOvis] Init()\n");
    if (s_initialized)
        return (s32)CELL_OVIS_ERROR_ALREADY_INITIALIZED;
    s_initialized = 1;
    s_next_handle = 1;
    return CELL_OK;
}

s32 cellOvisTerm(void)
{
    printf("[cellOvis] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

/* Retail API: elf is an in-memory ELF32, and the size is the return value.
 * Overlay segments use physical addresses beyond SPU LS (0x40000). Their
 * physical address minus 0x40000 is the offset in the PPU staging allocation.
 * This contract is verified against the disc-supplied 2.17 runtime. */
static s32 ovis_headers(u32 elf, u32* start, u32* stride, u32* count)
{
    if (!elf || (u64)elf + 52 > 0x100000000ULL ||
        vm_read32(elf) != 0x7F454C46 || vm_read8(elf + 4) != 1 ||
        vm_read8(elf + 5) != 2 || vm_read16(elf + 18) != 23)
        return (s32)CELL_OVIS_ERROR_INVAL;
    u32 off = vm_read32(elf + 28);
    *stride = vm_read16(elf + 42); *count = vm_read16(elf + 44);
    if (*stride < 32 || *count > 128 ||
        (u64)elf + off + (u64)*stride * *count > 0x100000000ULL)
        return (s32)CELL_OVIS_ERROR_INVAL;
    *start = elf + off;
    return CELL_OK;
}

s32 cellOvisGetOverlayTableSize(u32 elf)
{
    u32 start, stride, count;
    s32 rc = ovis_headers(elf, &start, &stride, &count);
    if (rc) return rc;
    u64 end = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 ph = start + i * stride;
        if (vm_read32(ph) != 1) continue;
        u32 physical = vm_read32(ph + 12), bytes = vm_read32(ph + 16);
        if (bytes && (u64)physical + bytes > end) end = (u64)physical + bytes;
    }
    if (end > 0x40000ULL + INT32_MAX) return (s32)CELL_OVIS_ERROR_INVAL;
    s32 size = end > 0x3FFFF ? (s32)(end - 0x40000) : 0;
    fprintf(stderr, "[cellOvis] ELF 0x%08X overlay staging size=0x%X\n", elf, (u32)size);
    return size;
}

s32 cellOvisInitializeOverlayTable(u32 table, u32 elf)
{
    if (table & 127) return (s32)CELL_OVIS_ERROR_ALIGN;
    u32 start, stride, count;
    s32 rc = ovis_headers(elf, &start, &stride, &count);
    if (rc) return rc;
    s32 size = cellOvisGetOverlayTableSize(elf);
    if (size < 0) return size;
    if (size && (!table || (u64)table + (u32)size > 0x100000000ULL))
        return (s32)CELL_OVIS_ERROR_INVAL;
    /* Validate every source span before changing any destination bytes. */
    for (u32 i = 0; i < count; ++i) {
        u32 ph = start + i * stride;
        if (vm_read32(ph) == 1 && vm_read32(ph + 12) >= 0x40000 &&
            (u64)elf + vm_read32(ph + 4) + vm_read32(ph + 16) > 0x100000000ULL)
            return (s32)CELL_OVIS_ERROR_INVAL;
    }
    for (u32 i = 0; i < count; ++i) {
        u32 ph = start + i * stride;
        if (vm_read32(ph) != 1) continue;
        u32 physical = vm_read32(ph + 12), bytes = vm_read32(ph + 16);
        if (physical < 0x40000 || !bytes) continue;
        u32 destination = table + physical - 0x40000;
        u32 source = elf + vm_read32(ph + 4);
        memcpy(vm_base + destination, vm_base + source, bytes);
        spu_overlay_bind(destination, vm_base + destination, bytes);
    }
    return CELL_OK;
}

s32 cellOvisCreateOverlay(const void* table, u32 tableSize, CellOvisHandle* handle)
{
    (void)table; (void)tableSize;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    if (!handle) return (s32)CELL_OVIS_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)handle, (u32)s_next_handle++);
    return CELL_OK;
}

s32 cellOvisDestroyOverlay(CellOvisHandle handle)
{
    (void)handle;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}

s32 cellOvisInvalidateOverlay(CellOvisHandle handle)
{
    (void)handle;
    if (!s_initialized) return (s32)CELL_OVIS_ERROR_NOT_INITIALIZED;
    return CELL_OK;
}
