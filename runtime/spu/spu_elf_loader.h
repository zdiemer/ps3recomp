/* Shared ELF-to-LS loader. Overlay staging lives in cellOvis, outside LS. */
#ifndef PS3RECOMP_SPU_ELF_LOADER_H
#define PS3RECOMP_SPU_ELF_LOADER_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "spu_context.h"
static inline uint16_t spu_elf_read16(const uint8_t* p)
{ return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline uint32_t spu_elf_read32(const uint8_t* p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline int spu_elf_copy_segments(const uint8_t* image, size_t image_size, uint8_t* ls, uint32_t* entry_out)
{
    if (!image || !ls || image_size < 0x34) return 0;

    /* ELF ident: 0x7F 'E' 'L' 'F', ELFCLASS32 (1), ELFDATA2MSB (2). */
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 /*ELFCLASS32*/ || image[5] != 2 /*ELFDATA2MSB*/)
        return 0;

    uint32_t e_entry     = spu_elf_read32(image + 0x18);
    uint32_t e_phoff     = spu_elf_read32(image + 0x1C);
    uint16_t e_phentsize = spu_elf_read16(image + 0x2A);
    uint16_t e_phnum     = spu_elf_read16(image + 0x2C);
    if (e_phentsize < 0x20 || e_entry >= SPU_LS_SIZE ||
        (uint64_t)e_phoff + (uint64_t)e_phnum * e_phentsize > image_size) return 0;

    for (uint16_t i = 0; i < e_phnum; i++) {
        size_t po = (size_t)e_phoff + (size_t)i * e_phentsize;
        if (po + 0x20 > image_size) return 0;
        const uint8_t* ph = image + po;

        uint32_t p_type   = spu_elf_read32(ph + 0x00);
        if (p_type != 1 /*PT_LOAD*/) continue;
        uint32_t p_offset = spu_elf_read32(ph + 0x04);
        uint32_t p_vaddr  = spu_elf_read32(ph + 0x08);
        uint32_t p_paddr  = spu_elf_read32(ph + 0x0C);
        uint32_t p_filesz = spu_elf_read32(ph + 0x10);
        uint32_t p_memsz  = spu_elf_read32(ph + 0x14);

        /* Physical addresses beyond LS identify cellOvis staging segments.
         * Loading every overlay at its shared VMA overwrites the resident
         * program. The task DMA-loads these from its PPU overlay table. */
        if (p_paddr >= SPU_LS_SIZE) continue;

        /* bounds: segment must fit in local store and in the image */
        if (p_filesz > p_memsz) return 0;
        if ((uint64_t)p_vaddr + p_memsz > SPU_LS_SIZE)            return 0;
        if ((uint64_t)p_offset + p_filesz > image_size)          return 0;

        if (p_filesz) memcpy(ls + p_vaddr, image + p_offset, p_filesz);
        if (p_memsz > p_filesz)
            memset(ls + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
    }
    if (entry_out) *entry_out = e_entry;
    return 1;
}

#endif
