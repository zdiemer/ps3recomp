/* Host raster dimensions. Guest addresses, pitches and shader constants stay
 * in the title's original pixel space. No game memory is resized. */
#ifndef RSX_RENDER_RESOLUTION_H
#define RSX_RENDER_RESOLUTION_H
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t guest_w, guest_h, host_w, host_h;
    uint32_t reference_w, reference_h;
    int custom;
    uint32_t shadow_guest, shadow_host;
} rsx_render_resolution;

static int rsx_render_resolution_parse(rsx_render_resolution* r,
    uint32_t w, uint32_t h, const char* option)
{
    *r = (rsx_render_resolution){w, h, w, h, w, h, 0};
    if (!w || !h) return -1;
    if (!option || !*option || !strcmp(option,"native")) return 0;
    /* Strict decimal WIDTHxHEIGHT; reject partial values and overflow. */
    uint32_t values[2] = {0, 0};
    const char* at = option;
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (*at < '0' || *at > '9') return -1;
        while (*at >= '0' && *at <= '9') {
            values[axis] = values[axis] * 10u + (unsigned)(*at++ - '0');
            if (values[axis] > 8192u) return -1;
        }
        if (!axis && *at++ != 'x') return -1;
    }
    if (*at || !values[0] || !values[1]) return -1;
    r->host_w = values[0]; r->host_h = values[1];
    r->custom = 1;
    return 0;
}

static uint32_t rsx_render_extent(uint32_t n, uint32_t guest, uint32_t host)
{
    if (!guest || guest == host) return n;
    uint64_t scaled = ((uint64_t)n * host + guest / 2u) / guest;
    return (uint32_t)(scaled ? scaled : 1u);
}

/* Shared edge mapping keeps adjacent scissors/copies contiguous, including
 * fractional resolutions and empty rectangles. */
static uint32_t rsx_render_edge(uint32_t n, uint32_t guest, uint32_t host)
{
    return guest ? (uint32_t)((uint64_t)n * host / guest) : n;
}

static void rsx_render_surface_extent(const rsx_render_resolution* r,
    uint32_t w, uint32_t h, uint32_t* host_w, uint32_t* host_h)
{
    if (r->shadow_guest && r->shadow_host && w==r->shadow_guest && h==r->shadow_guest) {
        *host_w=*host_h=r->shadow_host; return;
    }
    if (!r->custom) { *host_w=w; *host_h=h; return; }
    /* A title can render below scanout size. Both its main scene and its
     * final display target should reach the selected dimensions. */
    if (w==r->guest_w && h==r->guest_h) {
        *host_w=r->host_w; *host_h=r->host_h;
    } else {
        *host_w=rsx_render_extent(w,r->reference_w,r->host_w);
        *host_h=rsx_render_extent(h,r->reference_h,r->host_h);
    }
}
#endif
