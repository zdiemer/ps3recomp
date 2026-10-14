#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;
extern int ppu_native_reads_inline_enabled;
void ppu_native_reads_inline_init(void);
uint8_t  vm_read8 (uint64_t addr);
uint16_t vm_read16(uint64_t addr);
uint32_t vm_read32(uint64_t addr);
uint64_t vm_read64(uint64_t addr);
#ifdef __cplusplus
}
#endif

#if defined(_MSC_VER)
#define PPU_INLINE_READ_FORCE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define PPU_INLINE_READ_FORCE inline __attribute__((always_inline))
#else
#define PPU_INLINE_READ_FORCE inline
#endif

/* Ordinary reads cannot modify the host PPU context. A full compiler memory
 * clobber would incorrectly obscure that fact and defeat register promotion.
 * Volatile retains each dynamic guest load, including polling without a guest
 * fence. It does not supply PPC memory ordering: PPU_FENCE and the original
 * store/device/callback paths remain responsible for that ordering.
 * Packed scalar wrappers retain unaligned native loads without forming a
 * misaligned scalar pointer. may_alias also permits byte-backed guest RAM. */
#if defined(__GNUC__) || defined(__clang__)
#define PPU_INLINE_READ_PACKED __attribute__((packed, may_alias))
#else
#define PPU_INLINE_READ_PACKED
#pragma pack(push, 1)
#endif
typedef struct PPU_INLINE_READ_PACKED { uint16_t value; } ppu_inline_read_u16;
typedef struct PPU_INLINE_READ_PACKED { uint32_t value; } ppu_inline_read_u32;
typedef struct PPU_INLINE_READ_PACKED { uint64_t value; } ppu_inline_read_u64;
#if !defined(__GNUC__) && !defined(__clang__)
#pragma pack(pop)
#endif
#undef PPU_INLINE_READ_PACKED

/* This is deliberately a narrow read-only fast path. The generated-code
 * force-include shim includes ppu_recomp.h first, then this header, then maps
 * vm_readN call sites to these wrappers. Ineligible accesses call the original
 * ABI accessor, retaining every diagnostic/device/bounds behavior there. */
static PPU_INLINE_READ_FORCE int
ppu_native_read_inline_range(uint32_t ea, uint32_t width)
{
    if (!ppu_native_reads_inline_enabled || !vm_base || ea < 0x10000u)
        return 0;
    const uint64_t end = (uint64_t)ea + width;
    return end <= 0xE0000000ull &&
        (!ppu_vm_size || end <= ppu_vm_size);
}

static PPU_INLINE_READ_FORCE uint8_t
ppu_inline_vm_read8(uint64_t addr)
{
    const uint32_t ea = (uint32_t)addr;
    if (ppu_native_read_inline_range(ea, 1)) {
        return *(const volatile uint8_t*)(vm_base + ea);
    }
    return vm_read8(addr);
}

static PPU_INLINE_READ_FORCE uint16_t
ppu_inline_vm_read16(uint64_t addr)
{
    const uint32_t ea = (uint32_t)addr;
    if (ppu_native_read_inline_range(ea, 2)) {
        const uint16_t raw = ((const volatile ppu_inline_read_u16*)(vm_base + ea))->value;
        return __builtin_bswap16(raw);
    }
    return vm_read16(addr);
}

static PPU_INLINE_READ_FORCE uint32_t
ppu_inline_vm_read32(uint64_t addr)
{
    const uint32_t ea = (uint32_t)addr;
    if (ppu_native_read_inline_range(ea, 4) &&
        ea != ppu_hle_inject_base + 0x2008u) {
        const uint32_t raw = ((const volatile ppu_inline_read_u32*)(vm_base + ea))->value;
        return __builtin_bswap32(raw);
    }
    return vm_read32(addr);
}

static PPU_INLINE_READ_FORCE uint64_t
ppu_inline_vm_read64(uint64_t addr)
{
    const uint32_t ea = (uint32_t)addr;
    if (ppu_native_read_inline_range(ea, 8)) {
        const uint64_t raw = ((const volatile ppu_inline_read_u64*)(vm_base + ea))->value;
        return __builtin_bswap64(raw);
    }
    return vm_read64(addr);
}

#undef PPU_INLINE_READ_FORCE
