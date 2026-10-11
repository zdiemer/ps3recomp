#include "spu_overlay_registry.h"
#include "../platform/win32_compat.h"
#include <stdio.h>
#include <string.h>

#define OVERLAY_MAX 128
static struct { uint64_t fingerprint; uint32_t span, code_span, lsa; int image_id; } s_images[OVERLAY_MAX];
static struct { uint32_t ea, span, code_span, lsa; int image_id; } s_bindings[OVERLAY_MAX];
static unsigned s_image_count, s_binding_count;
static SRWLOCK s_lock = SRWLOCK_INIT;

void spu_overlay_register_fingerprint(uint64_t fingerprint, uint32_t span, int image_id)
{
    spu_overlay_register_fingerprint_range(fingerprint, span, span, image_id);
}

void spu_overlay_register_fingerprint_range(uint64_t fingerprint, uint32_t span,
                                             uint32_t code_span, int image_id)
{
    spu_overlay_register_fingerprint_at(fingerprint, span, code_span, UINT32_MAX, image_id);
}

void spu_overlay_register_fingerprint_at(uint64_t fingerprint, uint32_t span,
                                        uint32_t code_span, uint32_t lsa, int image_id)
{
    if (!span || span > 0x40000 || !code_span || code_span > span || image_id <= 0) return;
    if (lsa != UINT32_MAX && (uint64_t)lsa + code_span > 0x40000) return;
    AcquireSRWLockExclusive(&s_lock);
    if (s_image_count < OVERLAY_MAX) {
        s_images[s_image_count].fingerprint = fingerprint;
        s_images[s_image_count].span = span;
        s_images[s_image_count].code_span = code_span;
        s_images[s_image_count].lsa = lsa;
        s_images[s_image_count++].image_id = image_id;
    } else fprintf(stderr, "[spu-ovl] compiled overlay registry is full\n");
    ReleaseSRWLockExclusive(&s_lock);
}

void spu_overlay_bind(uint32_t ea, const uint8_t* bytes, uint32_t span)
{
    if (!bytes || !span || (uint64_t)ea + span > 0x100000000ULL) return;
    uint64_t hash = 1469598103934665603ULL;
    for (uint32_t i = 0; i < span; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
    AcquireSRWLockExclusive(&s_lock);
    int image = 0;
    uint32_t code_span = 0;
    uint32_t lsa = UINT32_MAX;
    for (unsigned i = 0; i < s_image_count; ++i)
        if (s_images[i].span == span && s_images[i].fingerprint == hash) {
            image = s_images[i].image_id;
            code_span = s_images[i].code_span;
            lsa = s_images[i].lsa;
            break;
        }
    unsigned free_slot = s_binding_count;
    for (unsigned i = 0; i < s_binding_count; ++i) {
        /* A reused staging allocation must not retain its previous code owner. */
        if ((uint64_t)ea < (uint64_t)s_bindings[i].ea + s_bindings[i].span &&
            (uint64_t)s_bindings[i].ea < (uint64_t)ea + span)
            s_bindings[i].image_id = 0;
        if (!s_bindings[i].image_id) free_slot = i;
    }
    if (image && free_slot < OVERLAY_MAX) {
        s_bindings[free_slot].ea = ea;
        s_bindings[free_slot].span = span;
        s_bindings[free_slot].code_span = code_span;
        s_bindings[free_slot].lsa = lsa;
        s_bindings[free_slot].image_id = image;
        if (free_slot == s_binding_count) ++s_binding_count;
        fprintf(stderr, "[spu-ovl] bound compiled image %d to EA 0x%08X span=0x%X\n", image, ea, span);
    } else if (image) fprintf(stderr, "[spu-ovl] staging binding registry is full\n");
    ReleaseSRWLockExclusive(&s_lock);
}

int spu_overlay_lookup_bound(uint32_t ea, uint32_t* span)
{
    return spu_overlay_lookup_bound_at(ea, UINT32_MAX, span);
}

int spu_overlay_lookup_bound_at(uint32_t ea, uint32_t lsa, uint32_t* span)
{
    int image = 0;
    AcquireSRWLockShared(&s_lock);
    for (unsigned i = 0; i < s_binding_count; ++i)
        if (s_bindings[i].image_id && s_bindings[i].ea == ea &&
            (lsa == UINT32_MAX || s_bindings[i].lsa == UINT32_MAX || s_bindings[i].lsa == lsa)) {
            image = s_bindings[i].image_id;
            if (span) *span = s_bindings[i].code_span;
            break;
        }
    ReleaseSRWLockShared(&s_lock);
    return image;
}

static uint32_t elf_be32(const uint8_t* b)
{ return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3]; }

void spu_overlay_bind_elf(uint32_t ea, const uint8_t* bytes, uint32_t size)
{
    if (!bytes || size < 52 || (uint64_t)ea + size > 0x100000000ULL ||
        memcmp(bytes,"\x7f" "ELF\x01\x02",6) || bytes[18] || bytes[19] != 23) return;
    uint32_t phoff=elf_be32(bytes+28);
    uint32_t entsize=((uint32_t)bytes[42]<<8)|bytes[43];
    uint32_t count=((uint32_t)bytes[44]<<8)|bytes[45];
    if (entsize < 32 || (uint64_t)phoff+(uint64_t)entsize*count > size) return;
    /* Validate every extent before publishing any binding from this read. */
    for (uint32_t i=0;i<count;i++) {
        const uint8_t* ph=bytes+phoff+i*entsize;
        if (elf_be32(ph)!=1) continue;
        uint32_t offset=elf_be32(ph+4), span=elf_be32(ph+16);
        if ((uint64_t)offset+span > size || span > 0x40000) return;
    }
    for (uint32_t i=0;i<count;i++) {
        const uint8_t* ph=bytes+phoff+i*entsize;
        if (elf_be32(ph)==1 && (elf_be32(ph+24)&1)) {
            uint32_t offset=elf_be32(ph+4), span=elf_be32(ph+16);
            spu_overlay_bind(ea+offset,bytes+offset,span);
        }
    }
}

void spu_overlay_bind_embedded(uint32_t ea, const uint8_t* bytes, uint32_t size)
{
    if (!bytes || (uint64_t)ea + size > 0x100000000ULL) return;
    uint32_t offset = 0;
    while (size - offset >= 52) {
        const uint8_t* candidate = (const uint8_t*)memchr(bytes + offset, 0x7F, size - offset - 51);
        if (!candidate) break;
        offset = (uint32_t)(candidate - bytes);
        spu_overlay_bind_elf(ea + offset, candidate, size - offset);
        ++offset;
    }
}

int spu_overlay_match_loaded(const uint8_t* bytes, uint32_t size, uint32_t lsa, uint32_t* code_span)
{
    uint32_t offset = 0;
    int image = spu_overlay_match_loaded_range(bytes,size,lsa,&offset,code_span);
    return offset == 0 ? image : 0;
}

int spu_overlay_match_loaded_range(const uint8_t* bytes, uint32_t size, uint32_t lsa,
                                  uint32_t* offset, uint32_t* code_span)
{
    if (!bytes || !offset || *offset >= size) return 0;
    int image = 0;
    uint32_t span = 0, best_offset = UINT32_MAX;
    AcquireSRWLockShared(&s_lock);
    for (unsigned i = 0; i < s_image_count; ++i) {
        if (s_images[i].lsa != UINT32_MAX && s_images[i].lsa < lsa) continue;
        uint32_t candidate = s_images[i].lsa == UINT32_MAX ? 0 : s_images[i].lsa - lsa;
        if (candidate < *offset || candidate > best_offset ||
            (uint64_t)candidate + s_images[i].span > size) continue;
        uint64_t hash = 1469598103934665603ULL;
        for (uint32_t j = 0; j < s_images[i].span; ++j)
            hash = (hash ^ bytes[candidate+j]) * 1099511628211ULL;
        if (hash != s_images[i].fingerprint) continue;
        if (image && candidate == best_offset && image != s_images[i].image_id) {
            /* A prefix shared by two registered programs is ambiguous. */
            image = 0;
            break;
        }
        image = s_images[i].image_id;
        span = s_images[i].code_span;
        best_offset = candidate;
    }
    ReleaseSRWLockShared(&s_lock);
    if (image) {
        *offset = best_offset;
        if (code_span) *code_span = span;
    }
    return image;
}
