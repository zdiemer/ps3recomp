#pragma once
#include <stdint.h>

/* Owned ordinary-RAM spans only. This does not establish game ownership.
 * begin holds the SPU reservation lock until end; while held, callers may
 * neither call guest/runtime services nor allocate, wait, or log. */
#ifdef __cplusplus
extern "C" {
#endif
int ppu_native_memory_available(void);
int ppu_native_memory_range(uint32_t address, uint32_t size);
int ppu_native_memory_begin(uint32_t source, uint32_t source_size,
                            uint32_t destination, uint32_t destination_size,
                            uint32_t metadata, uint32_t metadata_size);
void ppu_native_memory_end(void);
#ifdef __cplusplus
}
#endif

/* Pure bounds policy shared with boundary tests. Excludes all local/video/raw
 * SPU address windows and the relocatable HLE control/label/offset-table area. */
static inline int ppu_native_ram_range(uint32_t address, uint32_t size,
                                      uint32_t vm_size, uint32_t inject_base) {
    uint64_t end = (uint64_t)address + size;
    uint64_t inject_end = (uint64_t)inject_base + 0x8000u;
    return size && address >= 0x10000u && end <= 0xC0000000ull &&
        (!vm_size || end <= vm_size) &&
        !(address < inject_end && (uint64_t)inject_base < end);
}
