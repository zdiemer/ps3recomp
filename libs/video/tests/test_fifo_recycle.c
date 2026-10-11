#include <assert.h>
#include "../cellGcmSys.c"
uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void spu_lockline_lock(void) { }
void spu_lockline_unlock(void) { }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t v, int w) { (void)ea; (void)v; (void)w; }
static DWORD WINAPI recycle(void* unused) { (void)unused; cellGcm_fifo_recycle(0x100); return 0; }
static void wait_put(u32 value) {
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (vm_read32(GCM_CONTROL_GUEST_ADDR) != value && GetTickCount64() < deadline) Sleep(1);
    assert(vm_read32(GCM_CONTROL_GUEST_ADDR) == value);
}
int main(void) {
    const u32 begin = 0x100000, tail = 0x101000;
    vm_base = calloc(1, GCM_CONTROL_GUEST_ADDR + 4096);
    assert(vm_base);
    memset(s_io_address_table, 0xFF, sizeof s_io_address_table);
    s_io_address_table[1] = 0; s_ea_address_table[0] = 1;
    s_gcm_context_ea = 0x100; s_gcm_ctx_out_ea = 0x80; vm_write32(0x80, 0x100);
    vm_write32(0x100, begin); vm_write32(0x104, tail); vm_write32(0x108, tail);
    /* An old GET at begin must not acknowledge a new unconsumed ring. */
    g_gcm_fifo_drained_ea = begin;
    HANDLE t = CreateThread(NULL, 0, recycle, NULL, 0, NULL); assert(t);
    wait_put(0x1000);
    assert(vm_read32(tail) == 0x20000000);
    /* A slow shader compiler must not release storage after the old 2s timeout. */
    Sleep(2300);
    assert(WaitForSingleObject(t, 0) == WAIT_TIMEOUT);
    assert(vm_read32(0x108) == tail);
    /* The actual drain must not invoke its fallback recycler under the callback. */
    cellGcm_rsx_process_fifo();
    wait_put(0);
    assert(WaitForSingleObject(t, 0) == WAIT_TIMEOUT);
    assert(vm_read32(0x108) == tail);
    cellGcm_rsx_process_fifo();
    assert(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
    assert(vm_read32(0x108) == begin);
    CloseHandle(t);
    assert(!atomic_load(&s_fifo_recycle_active));
    /* Flip insertion must retain the write head until the reserve callback. */
    vm_write32(0x104, 0x142000); vm_write32(0x108, 0x140000);
    vm_write32(0x10C, 0x200); vm_write32(0x200, GCM_FIFO_CALLBACK_SENTINEL_EA);
    s_display_buffer_set[0] = 1; s_fifo_getoff = GCM_RECYCLE_MARGIN;
    assert(gcm_flip_into_fifo(0x100, 0));
    assert(vm_read32(0x108) == 0x140004);
    assert(vm_read32(0x140000) == GCM_FLIP_MARKER);
    /* A hardware acquire can span many ticker passes without being dropped
     * by the stuck-GET watchdog or the fallback ring recycler. */
    _putenv_s("GCM_SEMA_ACQUIRE", "1");
    s_fifo_getoff = 0;
    vm_write32(begin, 0x00080064); /* OFFSET + ACQUIRE in one method packet. */
    vm_write32(begin + 4, 0x100); vm_write32(begin + 8, 0xABC);
    vm_write32(GCM_CONTROL_GUEST_ADDR, 12);
    for (unsigned i = 0; i < 12; ++i) {
        cellGcm_rsx_process_fifo();
        assert(s_fifo_getoff == 0);
        assert(vm_read32(GCM_CONTROL_GUEST_ADDR + 4) == 0);
    }
    vm_write32(GCM_LABEL_GUEST_BASE + 0x100, 0xABC);
    cellGcm_rsx_process_fifo();
    assert(s_fifo_getoff == 12);
    _putenv_s("GCM_SEMA_ACQUIRE", "0");
    free(vm_base);
    puts("PASS: FIFO tail/wrap acknowledgments, delayed consumer, flip insertion and semaphore waits");
    return 0;
}
void (*g_spu_put_hook)(u32,u32);
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void rsx_state_init(rsx_state* s) { memset(s, 0, sizeof *s); }
void rsx_raise_user_cmd(u32 a) { (void)a; abort(); }
int rsx_live_draw_enabled(void) { abort(); }
void rsx_live_draw_method(u32 m,u32 a) { (void)m;(void)a;abort(); }
int rsx_draw_engine_enabled(void) { abort(); }
void rsx_draw_engine_method(u32 m,u32 a) { (void)m;(void)a;abort(); }
int rsx_process_method(rsx_state* s,u32 m,u32 a) { (void)s;(void)m;(void)a;abort(); }
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { abort(); }
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f) { abort(); }
void ppu_dump_guest_stack(ppu_context* c,const char* s) { abort(); }
void ppu_dump_bctrl_ring(uint32_t t,const char* s) { abort(); }
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h) { abort(); }
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l) { abort(); }
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d) { abort(); }
