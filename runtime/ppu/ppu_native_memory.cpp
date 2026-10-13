#include "ppu_native_memory.h"
extern "C" {
extern uint32_t ppu_vm_size, ppu_hle_inject_base;
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
int spu_coh_is_reserved(uint32_t);
}
#define VM_HLE_INJECT_BASE ppu_hle_inject_base
extern "C" int ppu_native_memory_range(uint32_t address, uint32_t size)
{
    return ppu_native_ram_range(address,size,ppu_vm_size,VM_HLE_INJECT_BASE);
}
extern "C" int ppu_native_memory_begin(uint32_t source, uint32_t source_size,
    uint32_t destination, uint32_t destination_size, uint32_t metadata, uint32_t metadata_size)
{
    if (!ppu_native_memory_available() ||
        source_size > 0x100000u || destination_size > 0x100000u || metadata_size > 0x100000u ||
        !ppu_native_memory_range(source,source_size) ||
        !ppu_native_memory_range(destination,destination_size) ||
        !ppu_native_memory_range(metadata,metadata_size)) return 0;
    const uint32_t starts[] = {source,destination,metadata};
    const uint32_t sizes[] = {source_size,destination_size,metadata_size};
    for (unsigned i=0;i<3;++i) for (unsigned j=i+1;j<3;++j)
        if ((uint64_t)starts[i] < (uint64_t)starts[j]+sizes[j] &&
            (uint64_t)starts[j] < (uint64_t)starts[i]+sizes[i]) return 0;
    // Serializing the check and the copy prevents a new GETLLAR reservation
    // from appearing between them. Never bypass notification for reserved RAM.
    spu_lockline_lock();
    for (unsigned i=0;i<3;++i) {
        const uint32_t last=(starts[i]+sizes[i]-1)&~127u;
        for (uint32_t line=starts[i]&~127u;;line+=128) {
            if (spu_coh_is_reserved(line)) { spu_lockline_unlock(); return 0; }
            if (line==last) break;
        }
    }
    return 1;
}
extern "C" void ppu_native_memory_end(void) { spu_lockline_unlock(); }

extern "C" int ppu_native_memory_begin_write(uint32_t destination, uint32_t size)
{
    if (!ppu_native_memory_available() || size > 0x100000u ||
        !ppu_native_memory_range(destination, size)) return 0;
    // A new reservation cannot appear between the scan and the owned write.
    // Rejection returns with no held lock and leaves all notification to the
    // original accessor path. The caller establishes ownership separately.
    spu_lockline_lock();
    const uint32_t last = (destination + size - 1u) & ~127u;
    for (uint32_t line = destination & ~127u;; line += 128u) {
        if (spu_coh_is_reserved(line)) { spu_lockline_unlock(); return 0; }
        if (line == last) break;
    }
    return 1;
}

