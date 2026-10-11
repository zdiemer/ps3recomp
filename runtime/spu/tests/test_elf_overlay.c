#include "../spu_elf_loader.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static void put32(uint8_t* p, uint32_t v)
{ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
int main(void)
{
    uint8_t elf[0x200] = {0};
    uint8_t* ls = malloc(SPU_LS_SIZE); assert(ls);
    memset(ls, 0xCC, SPU_LS_SIZE);
    memcpy(elf, "\x7F" "ELF", 4); elf[4]=1; elf[5]=2;
    put32(elf+24, 0x100); put32(elf+28, 0x40); elf[43]=32; elf[45]=2;
    put32(elf+0x40, 1); put32(elf+0x44, 0x100);
    put32(elf+0x48, 0x100); put32(elf+0x4C, 0x100);
    put32(elf+0x50, 4); put32(elf+0x54, 8);
    put32(elf+0x60, 1); put32(elf+0x64, 0x110);
    put32(elf+0x68, 0x100); put32(elf+0x6C, 0x40000);
    put32(elf+0x70, 4); put32(elf+0x74, 4);
    memcpy(elf+0x100, "CODE", 4); memcpy(elf+0x110, "OVLY", 4);
    uint32_t entry = 0;
    assert(spu_elf_copy_segments(elf, sizeof(elf), ls, &entry));
    assert(entry == 0x100 && memcmp(ls+0x100, "CODE", 4) == 0);
    assert(ls[0x104] == 0 && ls[0x107] == 0 && ls[0x108] == 0xCC);
    put32(elf+0x50, 9);
    assert(!spu_elf_copy_segments(elf, sizeof(elf), ls, &entry));
    put32(elf+0x50, 4); put32(elf+0x48, SPU_LS_SIZE-2);
    assert(!spu_elf_copy_segments(elf, sizeof(elf), ls, &entry));
    assert(!spu_elf_copy_segments(elf, 0x60, ls, &entry));
    free(ls);
    puts("SPU resident segments, BSS and overlay isolation checks passed");
    return 0;
}
