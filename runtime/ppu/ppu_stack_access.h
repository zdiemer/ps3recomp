#pragma once

/* Inline guest accesses for generated code, for sites whose address is the
 * guest stack pointer plus a constant: register spills, restores and the
 * back-chain store of stdu. They are a large share of all guest accesses, and
 * each one called an out-of-line accessor.
 *
 * Every access first checks vm_inline_page (spu_coherency.h) for the page it
 * starts in, so an address that turns out not to be ordinary RAM, a page with
 * an SPU reservation, or any memory diagnostic takes the original accessor.
 * Volatile keeps each access, as in ppu_inline_reads.h; guest fences remain in
 * the generated code. */

#include <stdint.h>

/* Include after ppu_recomp.h, whose vm_readN/vm_writeN declarations (or a
 * force-included remapping of them) the fallbacks below call. */

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t* vm_base;
extern unsigned char vm_inline_page[0x10000];
#ifdef __cplusplus
}
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define PPU_STACK_INLINE __forceinline
#define PPU_STACK_LIKELY(x) (x)
#else
#define PPU_STACK_INLINE inline __attribute__((always_inline))
#define PPU_STACK_LIKELY(x) __builtin_expect(!!(x), 1)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PPU_STACK_PACKED __attribute__((packed, may_alias))
#else
#define PPU_STACK_PACKED
#pragma pack(push, 1)
#endif
typedef struct PPU_STACK_PACKED { uint16_t value; } ppu_stack_u16;
typedef struct PPU_STACK_PACKED { uint32_t value; } ppu_stack_u32;
typedef struct PPU_STACK_PACKED { uint64_t value; } ppu_stack_u64;
#if !defined(__GNUC__) && !defined(__clang__)
#pragma pack(pop)
#endif
#undef PPU_STACK_PACKED

static PPU_STACK_INLINE int ppu_stack_inline(uint32_t ea)
{
    return PPU_STACK_LIKELY(*(const volatile unsigned char*)&vm_inline_page[ea >> 16]);
}

static PPU_STACK_INLINE uint8_t vm_stack_read8(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_stack_inline(ea)) return *(const volatile uint8_t*)(vm_base + ea);
    return vm_read8(a);
}
static PPU_STACK_INLINE uint16_t vm_stack_read16(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_stack_inline(ea))
        return __builtin_bswap16(((const volatile ppu_stack_u16*)(vm_base + ea))->value);
    return vm_read16(a);
}
static PPU_STACK_INLINE uint32_t vm_stack_read32(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_stack_inline(ea))
        return __builtin_bswap32(((const volatile ppu_stack_u32*)(vm_base + ea))->value);
    return vm_read32(a);
}
static PPU_STACK_INLINE uint64_t vm_stack_read64(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_stack_inline(ea))
        return __builtin_bswap64(((const volatile ppu_stack_u64*)(vm_base + ea))->value);
    return vm_read64(a);
}
static PPU_STACK_INLINE void vm_stack_write8(uint64_t a, uint8_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_stack_inline(ea)) *(volatile uint8_t*)(vm_base + ea) = v;
    else vm_write8(a, v);
}
static PPU_STACK_INLINE void vm_stack_write16(uint64_t a, uint16_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_stack_inline(ea)) ((volatile ppu_stack_u16*)(vm_base + ea))->value = __builtin_bswap16(v);
    else vm_write16(a, v);
}
static PPU_STACK_INLINE void vm_stack_write32(uint64_t a, uint32_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_stack_inline(ea)) ((volatile ppu_stack_u32*)(vm_base + ea))->value = __builtin_bswap32(v);
    else vm_write32(a, v);
}
static PPU_STACK_INLINE void vm_stack_write64(uint64_t a, uint64_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_stack_inline(ea)) ((volatile ppu_stack_u64*)(vm_base + ea))->value = __builtin_bswap64(v);
    else vm_write64(a, v);
}

#undef PPU_STACK_INLINE
#undef PPU_STACK_LIKELY
