#include "../rsx_m2mf.h"
#include <assert.h>
#include <stdio.h>

static unsigned char local[4096], main_mem[4096];
static unsigned char* resolve(void* opaque, uint32_t dma, uint32_t off, uint32_t size)
{
    (void)opaque;
    if ((uint64_t)off + size > sizeof local) return NULL;
    if (dma == 0xFEED0000) return local + off;
    if (dma == 0xFEED0001) return main_mem + off;
    return NULL;
}
int main(void)
{
    rsx_m2mf s = {0};
    for (unsigned i = 0; i < sizeof local; ++i) local[i] = (unsigned char)(i * 37 + 11);
    memset(main_mem, 0xCD, sizeof main_mem);
    const uint32_t methods[] = {0x184,0x188,0x30C,0x310,0x314,0x318,0x31C,0x320,0x324};
    const uint32_t values[] = {0xFEED0000,0xFEED0001,7,100,23,31,17,5,0x101};
    for (unsigned i = 0; i < sizeof methods / sizeof *methods; ++i)
        assert(rsx_m2mf_method(&s, methods[i], values[i]) == 1);
    assert(main_mem[100] == 0xCD); /* Register updates cannot launch a copy. */
    assert(rsx_m2mf_method(&s, 0x328, 0) == 2);
    assert(rsx_m2mf_copy(&s, resolve, NULL, NULL));
    for (unsigned y = 0; y < 5; ++y) {
        assert(memcmp(main_mem + 100 + y * 31, local + 7 + y * 23, 17) == 0);
        assert(main_mem[100 + y * 31 + 17] == 0xCD);
    }
    /* Reverse direction and negative pitch; byte order stays unchanged. */
    s.dma_in = 0xFEED0001; s.dma_out = 0xFEED0000;
    s.offset_in = 224; s.offset_out = 500; s.pitch_in = -31; s.pitch_out = 17;
    unsigned char expected[85];
    for (unsigned y = 0; y < 5; ++y) memcpy(expected + y * 17, main_mem + 224 - y * 31, 17);
    assert(rsx_m2mf_copy(&s, resolve, NULL, NULL));
    assert(memcmp(local + 500, expected, sizeof expected) == 0);
    /* Single-line overlapping GPU buffer moves. */
    s.dma_in = s.dma_out; s.offset_in = 500; s.offset_out = 503;
    s.length = sizeof expected; s.lines = 1;
    assert(rsx_m2mf_copy(&s, resolve, NULL, NULL));
    assert(memcmp(local + 503, expected, sizeof expected) == 0);
    /* No partial write when a later row, signed pitch, DMA or format is invalid. */
    unsigned char before[4096]; memcpy(before, local, sizeof local);
    s.offset_in = 4000; s.offset_out = 1000; s.length = 80; s.lines = 2; s.pitch_in = 80;
    assert(!rsx_m2mf_copy(&s, resolve, NULL, NULL));
    assert(memcmp(before, local, sizeof local) == 0);
    s.offset_in = 2; s.pitch_in = -3;
    assert(!rsx_m2mf_copy(&s, resolve, NULL, NULL));
    s.offset_in = UINT32_MAX - 20; s.lines = 1;
    assert(!rsx_m2mf_copy(&s, resolve, NULL, NULL));
    s.offset_in = 0; s.dma_in = 0xDEADBEEF;
    assert(!rsx_m2mf_copy(&s, resolve, NULL, NULL));
    s.dma_in = 0xFEED0000; s.format = 0x201;
    assert(!rsx_m2mf_copy(&s, resolve, NULL, NULL));
    s.lines = 0; assert(rsx_m2mf_copy(&s, resolve, NULL, NULL));
    assert(!rsx_m2mf_method(&s, 0x1808, 0));
    puts("NV0039: register trigger, raw bytes, both DMA directions, pitches, overlap and range failures passed");
    return 0;
}
