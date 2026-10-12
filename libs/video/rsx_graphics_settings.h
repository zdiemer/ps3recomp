/* Startup options shared by the title's config loader and live renderer. */
#ifndef RSX_GRAPHICS_SETTINGS_H
#define RSX_GRAPHICS_SETTINGS_H
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rsx_render_resolution.h"

typedef struct {
    const char *key, *environment, *fallback, *allowed;
    int kind;
    float low, high;
} rsx_graphics_option;
static const rsx_graphics_option rsx_graphics_options[] = {
    {"render_resolution","RSX_RENDER_RESOLUTION","native","native or WIDTHxHEIGHT (1..8192)",1,0,0},
    {"anti_aliasing","RSX_ANTI_ALIASING","off","off or fxaa",0,0,0},
    {"anisotropic_filtering","RSX_ANISOTROPY","1","1, 2, 4, 8 or 16",2,1,16},
    {"shadow_resolution","RSX_SHADOW_RESOLUTION","follow","follow, native, 1024, 2048, 4096 or 8192",3,0,0},
    {"mip_lod_bias","RSX_MIP_LOD_BIAS","0","-2..2",4,-2,2},
    {"sharpening","RSX_SHARPENING","0","0..1",4,0,1},
    {"exposure","RSX_EXPOSURE","0","-2..2 stops",4,-2,2},
    {"vsync","RSX_VSYNC","auto","auto, on or off",0,0,0},
    {"frame_limit","RSX_MAX_FPS","30","0..240 (0 = uncapped)",5,0,240},
};
static const rsx_graphics_option* rsx_graphics_find_option(const char* key) {
    for (unsigned i=0;i<sizeof(rsx_graphics_options)/sizeof(rsx_graphics_options[0]);++i)
        if (!strcmp(key,rsx_graphics_options[i].key)) return &rsx_graphics_options[i];
    return NULL;
}
static int rsx_graphics_valid(const rsx_graphics_option* o,const char* value) {
    if (!value || !*value) return 0;
    if (o->kind==1) {
        rsx_render_resolution r;
        return !strcmp(value,"native") || rsx_render_resolution_parse(&r,1,1,value)==0;
    }
    if (o->kind==0) {
        if (!strcmp(o->key,"anti_aliasing")) return !strcmp(value,"off") || !strcmp(value,"fxaa");
        return !strcmp(value,"auto") || !strcmp(value,"on") || !strcmp(value,"off");
    }
    if (o->kind==3) return !strcmp(value,"follow") || !strcmp(value,"native") ||
        !strcmp(value,"1024") || !strcmp(value,"2048") || !strcmp(value,"4096") || !strcmp(value,"8192");
    char* end=NULL;
    const double n=strtod(value,&end);
    if (*end || !isfinite(n) || n<o->low || n>o->high) return 0;
    if (o->kind==4) return 1;
    for (const char* c=value;*c;++c) if (*c<'0' || *c>'9') return 0;
    return o->kind==5 || n==1 || n==2 || n==4 || n==8 || n==16;
}
#endif
