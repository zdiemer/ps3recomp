#include "spu_overlay_registry.h"
#include "../platform/win32_compat.h"
#include <stdio.h>

#define OVERLAY_MAX 128
static struct { uint64_t fingerprint; uint32_t span, code_span; int image_id; } s_images[OVERLAY_MAX];
static struct { uint32_t ea, span, code_span; int image_id; } s_bindings[OVERLAY_MAX];
static unsigned s_image_count, s_binding_count;
static SRWLOCK s_lock = SRWLOCK_INIT;

void spu_overlay_register_fingerprint(uint64_t fingerprint, uint32_t span, int image_id)
{
    spu_overlay_register_fingerprint_range(fingerprint, span, span, image_id);
}

void spu_overlay_register_fingerprint_range(uint64_t fingerprint, uint32_t span,
                                             uint32_t code_span, int image_id)
{
    if (!span || span > 0x40000 || !code_span || code_span > span || image_id <= 0) return;
    AcquireSRWLockExclusive(&s_lock);
    if (s_image_count < OVERLAY_MAX) {
        s_images[s_image_count].fingerprint = fingerprint;
        s_images[s_image_count].span = span;
        s_images[s_image_count].code_span = code_span;
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
    for (unsigned i = 0; i < s_image_count; ++i)
        if (s_images[i].span == span && s_images[i].fingerprint == hash) {
            image = s_images[i].image_id;
            code_span = s_images[i].code_span;
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
        s_bindings[free_slot].image_id = image;
        if (free_slot == s_binding_count) ++s_binding_count;
        fprintf(stderr, "[spu-ovl] bound compiled image %d to EA 0x%08X span=0x%X\n", image, ea, span);
    } else if (image) fprintf(stderr, "[spu-ovl] staging binding registry is full\n");
    ReleaseSRWLockExclusive(&s_lock);
}

int spu_overlay_lookup_bound(uint32_t ea, uint32_t* span)
{
    int image = 0;
    AcquireSRWLockShared(&s_lock);
    for (unsigned i = 0; i < s_binding_count; ++i)
        if (s_bindings[i].image_id && s_bindings[i].ea == ea) {
            image = s_bindings[i].image_id;
            if (span) *span = s_bindings[i].code_span;
            break;
        }
    ReleaseSRWLockShared(&s_lock);
    return image;
}
