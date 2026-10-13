#pragma once

#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

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

/* The original external accessor is a compiler memory boundary. Preserve
 * that boundary when inlining: guest RAM may be published by another thread,
 * including in polling loops with no explicit guest fence. These barriers
 * emit no hardware fence; guest PPU_FENCE still supplies hardware ordering. */
static PPU_INLINE_READ_FORCE void ppu_inline_read_compiler_barrier(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __asm__ __volatile__("" ::: "memory");
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
#else
#error "Inline PPU reads require a compiler memory barrier"
#endif
}

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
        ppu_inline_read_compiler_barrier();
        const uint8_t raw = vm_base[ea];
        ppu_inline_read_compiler_barrier();
        return raw;
    }
    return vm_read8(addr);
}

static PPU_INLINE_READ_FORCE uint16_t
ppu_inline_vm_read16(uint64_t addr)
{
    const uint32_t ea = (uint32_t)addr;
    if (ppu_native_read_inline_range(ea, 2)) {
        uint16_t raw;
        ppu_inline_read_compiler_barrier();
        memcpy(&raw, vm_base + ea, sizeof(raw));
        ppu_inline_read_compiler_barrier();
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
        uint32_t raw;
        ppu_inline_read_compiler_barrier();
        memcpy(&raw, vm_base + ea, sizeof(raw));
        ppu_inline_read_compiler_barrier();
        return __builtin_bswap32(raw);
    }
    return vm_read32(addr);
}

static PPU_INLINE_READ_FORCE uint64_t
ppu_inline_vm_read64(uint64_t addr)
{
    const uint32_t ea = (uint32_t)addr;
    if (ppu_native_read_inline_range(ea, 8)) {
        uint64_t raw;
        ppu_inline_read_compiler_barrier();
        memcpy(&raw, vm_base + ea, sizeof(raw));
        ppu_inline_read_compiler_barrier();
        return __builtin_bswap64(raw);
    }
    return vm_read64(addr);
}

#undef PPU_INLINE_READ_FORCE
