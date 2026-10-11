#include <assert.h>
#include <stdio.h>
#include "../spu_overlay_registry.c"

static uint64_t fingerprint(const uint8_t* bytes, uint32_t span)
{
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < span; ++i) h = (h ^ bytes[i]) * 1099511628211ULL;
    return h;
}
static void be32(uint8_t* b, uint32_t v)
{ b[0]=(uint8_t)(v>>24); b[1]=(uint8_t)(v>>16); b[2]=(uint8_t)(v>>8); b[3]=(uint8_t)v; }
int main(void)
{
    uint8_t a[128] = {0}, b[128] = {0}, unknown[128] = {0};
    a[127] = 1; b[127] = 2; unknown[127] = 3;
    /* Identical initial instructions must not confuse two overlay programs. */
    spu_overlay_register_fingerprint(fingerprint(a, sizeof a), sizeof a, 45);
    spu_overlay_register_fingerprint(fingerprint(b, sizeof b), sizeof b, 46);
    spu_overlay_bind(0x10000, a, sizeof a);
    spu_overlay_bind(0x20000, b, sizeof b);
    uint32_t span = 0;
    assert(spu_overlay_lookup_bound(0x10000, &span) == 45 && span == 128);
    assert(spu_overlay_lookup_bound(0x20000, &span) == 46);
    assert(spu_overlay_lookup_bound(0x10010, &span) == 0);
    spu_overlay_bind(0x30000, a, sizeof a);
    assert(spu_overlay_lookup_bound(0x30000, &span) == 45);
    spu_overlay_bind(0x10000, b, sizeof b);
    assert(spu_overlay_lookup_bound(0x10000, &span) == 46);
    spu_overlay_bind(0x10000, unknown, sizeof unknown);
    assert(spu_overlay_lookup_bound(0x10000, &span) == 0);
    /* Partial overwrite invalidates the previous whole-program binding. */
    spu_overlay_bind(0x20040, unknown, 16);
    assert(spu_overlay_lookup_bound(0x20000, &span) == 0);
    spu_overlay_bind(0xFFFFFFF0, a, sizeof a);
    assert(spu_overlay_lookup_bound(0xFFFFFFF0, &span) == 0);
    uint8_t padded[256]={0};
    memcpy(padded,a,sizeof a);
    assert(spu_overlay_match_loaded(padded,sizeof padded,0x1000,&span)==45 && span==128);
    assert(!spu_overlay_match_loaded(padded,127,0x1000,&span));
    memcpy(padded,b,sizeof b); // Same staging allocation, different program.
    assert(spu_overlay_match_loaded(padded,sizeof padded,0x1000,&span)==46);
    padded[127]=3; // Only the last byte differs from both known programs.
    assert(!spu_overlay_match_loaded(padded,sizeof padded,0x1000,&span));
    assert(!spu_overlay_match_loaded(NULL,sizeof padded,0x1000,&span));
    /* Writable data is part of content identity, but not code ownership. */
    spu_overlay_register_fingerprint_range(fingerprint(a, 96), 96, 64, 47);
    spu_overlay_bind(0x40000, a, 96);
    assert(spu_overlay_lookup_bound(0x40000, &span) == 47 && span == 64);
    a[80] = 42;
    spu_overlay_bind(0x50000, a, 96);
    assert(spu_overlay_lookup_bound(0x50000, &span) == 0);
    spu_overlay_register_fingerprint_range(fingerprint(a, 96), 96, 97, 48);
    spu_overlay_bind(0x60000, a, 96);
    assert(spu_overlay_lookup_bound(0x60000, &span) == 0);
    uint8_t elf[256]={0};
    memcpy(elf,"\x7f" "ELF\x01\x02",6); elf[19]=23;
    be32(elf+28,52); elf[43]=32; elf[45]=1;
    be32(elf+52,1); be32(elf+56,128); be32(elf+68,128); be32(elf+76,5);
    memcpy(elf+128,b,128);
    spu_overlay_bind_elf(0x70000,elf,sizeof elf);
    assert(spu_overlay_lookup_bound(0x70080,&span)==46 && span==128);
    spu_overlay_bind_elf(0x80000,elf,sizeof elf-1); // Truncated code.
    assert(!spu_overlay_lookup_bound(0x80080,&span));
    be32(elf+56,0xFFFFFFF0); // Wrapped file extent.
    spu_overlay_bind_elf(0x90000,elf,sizeof elf);
    assert(!spu_overlay_lookup_bound(0x90080,&span));
    be32(elf+56,128); elf[19]=21; // PPU ELF is not SPU code.
    spu_overlay_bind_elf(0xA0000,elf,sizeof elf);
    assert(!spu_overlay_lookup_bound(0xA0080,&span));
    uint8_t segment[576]={0};
    elf[19]=23;
    memcpy(segment+17,elf,sizeof elf);
    memcpy(segment+300,elf,sizeof elf);
    segment[2]=0x7F; // A non-ELF candidate must not hide later programs.
    spu_overlay_bind_embedded(0xB0000,segment,sizeof segment);
    assert(spu_overlay_lookup_bound(0xB0091,&span)==46);
    assert(spu_overlay_lookup_bound(0xB01AC,&span)==46);
    spu_overlay_bind_embedded(0xC0000,segment,300+sizeof elf-1);
    assert(spu_overlay_lookup_bound(0xC0091,&span)==46);
    assert(!spu_overlay_lookup_bound(0xC01AC,&span));
    spu_overlay_bind_embedded(0xFFFFFF00,segment,sizeof segment);
    assert(!spu_overlay_lookup_bound(0xFFFFFF91,&span));
    uint8_t pic[64]={42};
    spu_overlay_register_fingerprint_at(fingerprint(pic,64),64,32,0x3000,49);
    spu_overlay_register_fingerprint_at(fingerprint(pic,64),64,32,0x3100,50);
    assert(spu_overlay_match_loaded(pic,64,0x3000,&span)==49 && span==32);
    assert(spu_overlay_match_loaded(pic,64,0x3100,&span)==50);
    assert(!spu_overlay_match_loaded(pic,64,0x3200,&span));
    spu_overlay_bind(0xD0000,pic,64);
    assert(spu_overlay_lookup_bound_at(0xD0000,0x3000,&span)==49);
    assert(!spu_overlay_lookup_bound_at(0xD0000,0x3100,&span));
    uint8_t dma[512]={0};
    memcpy(dma+16,pic,64);
    memcpy(dma+272,pic,64);
    uint32_t cursor=0;
    assert(spu_overlay_match_loaded_range(dma,sizeof dma,0x2FF0,&cursor,&span)==49);
    assert(cursor==16 && span==32);
    cursor+=span;
    assert(spu_overlay_match_loaded_range(dma,sizeof dma,0x2FF0,&cursor,&span)==50);
    assert(cursor==272);
    cursor+=span;
    assert(!spu_overlay_match_loaded_range(dma,sizeof dma,0x2FF0,&cursor,&span));
    puts("SPU overlay content, relocation and staging reuse checks passed");
    return 0;
}
