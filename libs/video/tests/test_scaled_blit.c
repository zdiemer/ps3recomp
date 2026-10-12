#include <assert.h>
#include "../cellGcmSys.c"
uint8_t* vm_base;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void (*g_spu_put_hook)(u32,u32);
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void rsx_state_init(rsx_state* s) { (void)s;abort(); }
void rsx_raise_user_cmd(u32 a) { (void)a;abort(); }
int rsx_draw_engine_enabled(void) { abort(); }
void rsx_draw_engine_method(u32 m,u32 a) { (void)m;(void)a;abort(); }
int rsx_process_method(rsx_state* s,u32 m,u32 a) { (void)s;(void)m;(void)a;abort(); }
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { abort(); }
void ppu_dump_guest_stack(ppu_context* c,const char* s) { abort(); }
void ppu_dump_bctrl_ring(uint32_t t,const char* s) { abort(); }
static u32 reserved[2],notifications,locks;
void spu_lockline_lock(void) { assert(!locks); locks=1; }
void spu_lockline_unlock(void) { assert(locks); locks=0; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; assert(0); }
int spu_coh_is_reserved(uint32_t ea) {
    ea &= ~127u; return ea==reserved[0] || ea==reserved[1];
}
void spu_coh_notify_write(uint32_t ea) {
    assert(locks && spu_coh_is_reserved(ea)); ++notifications;
}
void ps3_ww_report_inline(uint32_t ea,uint64_t v,int w) { (void)ea;(void)v;(void)w;assert(0); }
int rsx_live_draw_enabled(void) { return 0; }
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0; }
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;return 0; }
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d) { (void)a;(void)b;(void)c;(void)d; }
static u32 seed=0x17319;
static u32 random_u32(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static void reference(u8* output,u32 src,u32 bpp)
{
    u32 w=s_nv3089.out_sz & 65535,h=s_nv3089.out_sz>>16;
    u32 ox=s_nv3089.out_pt & 65535,oy=s_nv3089.out_pt>>16;
    u32 cx=s_nv3089.clip_pt & 65535,cy=s_nv3089.clip_pt>>16;
    u32 cw=s_nv3089.clip_sz & 65535,ch=s_nv3089.clip_sz>>16;
    u32 u=s_nv3089.in_uv & 65535,v=s_nv3089.in_uv>>16;
    for (u32 y=0;y<h;++y) for (u32 x=0;x<w;++x) {
        u32 dx=ox+x,dy=oy+y;
        if (dx<cx || dy<cy || (cw && dx>=cx+cw) || (ch && dy>=cy+ch)) continue;
        u32 sx=(u32)((((u64)u<<8)+(u64)x*s_nv3089.ds_dx)>>20);
        u32 sy=(u32)((((u64)v<<8)+(u64)y*s_nv3089.dt_dy)>>20);
        memcpy(output+dy*(s_gcm2d.pitch>>16)+dx*bpp,
               vm_base+src+sy*(s_nv3089.in_fmt & 65535)+sx*bpp,bpp);
    }
}
int main(void)
{
    u8* memory=malloc(1024*1024),*expected=malloc(65536); assert(memory && expected);
    vm_base=memory; reserved[0]=reserved[1]=UINT32_MAX;
    for (u32 i=0;i<1024*1024;++i) memory[i]=(u8)(i*37+(i>>8)*17);
    const u32 rates[]={0,0x10000,0x40000,0x80000,0x100000,0x180000,0x400000};
    for (u32 trial=0;trial<8192;++trial) {
        u32 bpp=1u<<(trial%3);
        s_nv3089.out_sz=(1+random_u32()%32)|((1+random_u32()%24)<<16);
        s_nv3089.out_pt=(random_u32()%12)|((random_u32()%12)<<16);
        s_nv3089.clip_pt=(random_u32()%24)|((random_u32()%24)<<16);
        s_nv3089.clip_sz=(random_u32()%48)|((random_u32()%48)<<16);
        s_nv3089.in_uv=(random_u32()%64)|((random_u32()%64)<<16);
        s_nv3089.ds_dx=rates[random_u32()%7]; s_nv3089.dt_dy=rates[random_u32()%7];
        s_nv3089.in_fmt=1024; s_gcm2d.pitch=256u<<16;
        memset(memory+0x40000,0xCD,65536); memset(expected,0xCD,65536);
        reference(expected,0x1000,bpp);
        assert(nv3089_copy_rows(0x1000,0x40000,bpp));
        assert(!memcmp(memory+0x40000,expected,65536));
    }
    memset(&s_nv3089,0,sizeof s_nv3089);
    s_nv3089.out_sz=8|(1u<<16);s_nv3089.in_fmt=256;
    s_nv3089.ds_dx=s_nv3089.dt_dy=0x100000;s_gcm2d.pitch=256u<<16;
    reserved[0]=0x40000;reserved[1]=0x40080;notifications=0;
    assert(nv3089_copy_rows(0x1000,0x4007C,4));
    assert(notifications==2 && !locks);
    assert(!memcmp(memory+0x4007C,memory+0x1000,32));
    reserved[0]=reserved[1]=UINT32_MAX;
    /* Fallbacks cannot partially modify the destination. */
    memcpy(expected,memory+0x1000,256);
    assert(!nv3089_copy_rows(0x1000,0x1004,4));
    assert(!memcmp(expected,memory+0x1000,256));
    assert(!nv3089_copy_rows(UINT32_MAX-8,0x40000,4));
    assert(!nv3089_copy_rows(0x1000,UINT32_MAX-8,4));
    g_ww_lo=0x40000;g_ww_hi=0x40010;
    assert(!nv3089_copy_rows(0x1000,0x40000,4));g_ww_lo=g_ww_hi=0;
    g_resv_store_active=1;assert(!nv3089_copy_rows(0x1000,0x40000,4));g_resv_store_active=0;
    /* Integrate the actual NV3089 trigger and location resolver. */
    FILE* file=fopen("local/test-row-mode.txt","w");assert(file);fputs("128\n",file);fclose(file);
    _putenv_s("GCM_ROW_BLIT_MODE_FILE","local/test-row-mode.txt");
    s_config.localAddress=0;s_config.localSize=1024*1024;
    s_nv3089.src_dma=s_gcm2d.dst_dma=0xFEED0000;
    s_nv3089.in_off=0x1000;s_gcm2d.dst_offset=0x40000;
    s_nv3089.fmt=3;s_gcm2d.color_fmt=10;
    memset(memory+0x40000,0xCD,32);nv3089_blit();
    assert(!memcmp(memory+0x40000,memory+0x1000,32));
    /* Exact-width loads and writes at inaccessible page boundaries. */
    u8* page=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(page);
    DWORD old;assert(VirtualProtect(page+4096,4096,PAGE_NOACCESS,&old));vm_base=page;
    s_nv3089.out_sz=1|(1u<<16);
    for (u32 bpp=1;bpp<=4;bpp*=2) {
        memset(page+4096-bpp,0xA5,bpp);
        assert(nv3089_copy_rows(4096-bpp,0,bpp));
        assert(!memcmp(page,page+4096-bpp,bpp));
        memset(page,0x5A,bpp);assert(nv3089_copy_rows(0,4096-bpp,bpp));
        assert(!memcmp(page,page+4096-bpp,bpp));
    }
    VirtualFree(page,0,MEM_RELEASE);free(memory);free(expected);
    puts("NV3089 rows: 8192 point-sampling/clipping cases, coherent lines, overlap/hooks/bounds fallback, trigger and protected tails passed");
    return 0;
}
