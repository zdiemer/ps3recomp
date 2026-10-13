/* Exercise the real mixer across ring wraps, without starting a device/thread.
 * macOS: clang -std=gnu17 -I include -I /opt/homebrew/include
 *   libs/audio/tests/test_audio_ring.c -L /opt/homebrew/lib -lSDL2
 *   -Wl,-dead_strip -o /tmp/test_audio_ring && /tmp/test_audio_ring
 */
#include "../cellAudio.c"
#include "../../misc/cellAvconfExt.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size) { (void)key; (void)size; assert(0); return 0; }
uint32_t sys_event_find_queue_by_key(uint64_t key) { (void)key; assert(0); return 0; }
int sys_event_queue_push_by_id(uint32_t id, uint64_t source, uint64_t a, uint64_t b, uint64_t c)
{ (void)id; (void)source; (void)a; (void)b; (void)c; assert(0); return 0; }

int main(void)
{
#ifdef _WIN32
    vm_base = VirtualAlloc(NULL, 0x100000000ULL, MEM_RESERVE, PAGE_READWRITE);
    assert(vm_base);
    assert(VirtualAlloc(vm_base, 0x1000, MEM_COMMIT, PAGE_READWRITE));
    assert(VirtualAlloc(vm_base + 0x60000000, 0x1000, MEM_COMMIT, PAGE_READWRITE));
#else
    vm_base = mmap(NULL, 0x100000000ULL, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(vm_base != MAP_FAILED);
#endif
    mutex_init(&s_audio_mutex);
    for (unsigned blocks = 8; blocks <= 32; blocks *= 2) {
        AudioPortSlot* port = &s_ports[0];
        memset(port, 0, sizeof(*port));
        port->in_use = port->running = 1;
        port->param.nChannel = 8;
        port->param.nBlock = blocks;
        port->param.level = 1.0f;
        port->read_idx_addr = 16;
        port->buffer = calloc(blocks * CELL_AUDIO_BLOCK_SAMPLES * 8, sizeof(float));
        assert(port->buffer);
        for (unsigned i = 1; i <= blocks * 3; ++i) {
            audio_mix_one_block();
            assert(port->read_index == i);
            assert(vm_read64(16) == i % blocks);
        }
        free(port->buffer);
    }
    memset(s_ports, 0, sizeof(s_ports));
    s_audio_initialized = 1;
    vm_write64(0x100, 8);
    vm_write64(0x108, 32);
    vm_write64(0x60000000, 0x123456789abcdef0ULL);
    for (unsigned i = 0; i < CELL_AUDIO_PORT_MAX; ++i) {
        assert(cellAudioPortOpen((void*)0x100, (void*)0x200) == CELL_OK);
        assert(vm_read32(0x200) == i);
        assert(s_ports[i].port_addr >= 0x58000000);
        assert(s_ports[i].read_idx_addr + 8 <= 0x59000000);
        if (i) assert(s_ports[i].port_addr > s_ports[i-1].read_idx_addr);
    }
    assert(vm_read64(0x60000000) == 0x123456789abcdef0ULL);
    u64 first = s_ports[0].port_addr;
    assert(cellAudioPortClose(0) == CELL_OK);
    assert(cellAudioPortOpen((void*)0x100, (void*)0x200) == CELL_OK);
    assert(s_ports[0].port_addr == first);
    assert(!s_ports[0].running);
    audio_mix_one_block();
    assert(vm_read64((u32)s_ports[0].read_idx_addr) == 0);
    assert(cellAudioPortStart(0) == CELL_OK);
    assert(cellAudioPortStart(0) == CELL_AUDIO_ERROR_PORT_ALREADY_RUN);
    s_ports[0].read_index = 35;
    s_audio_start_us = 1000000;
    assert(cellAudioGetPortBlockTag(0, 3, (void*)0x300) == CELL_OK);
    assert(vm_read64(0x300) == 35);
    assert(cellAudioGetPortTimestamp(0, 35, (void*)0x308) == CELL_OK);
    assert(vm_read64(0x308) == 1000000 + 35 * 256000000ULL / 48000);
    assert(cellAudioGetPortTimestamp(0, 36, (void*)0x308) == CELL_AUDIO_ERROR_TAG_NOT_FOUND);
    assert(cellAudioGetPortBlockTag(0, 32, (void*)0x300) == CELL_AUDIO_ERROR_PARAM);
    /* Surround-only PCM must not be added to stereo when downmix is NONE.
     * Then exercise every channel of the explicit type-A downmix. */
    for (unsigned p=1;p<CELL_AUDIO_PORT_MAX;p++) assert(cellAudioPortClose(p)==CELL_OK);
    for (unsigned mode=0;mode<=2;mode++) {
        vm_write8(0x400,2); vm_write8(0x401,0); vm_write32(0x40C,mode);
        assert(cellAudioOutConfigure(0,(void*)0x400,NULL,0)==CELL_OK);
        for (unsigned channel=0;channel<8;channel++) {
            AudioPortSlot* port=&s_ports[0];
            u32 ea=(u32)port->port_addr+(u32)(port->read_index%port->param.nBlock)*256*8*4;
            vm_write32(ea+channel*4,0x3f000000); /* 0.5 */
            audio_mix_one_block();
            const float l[]={.707f,0,.5f,0,.5f,0,.5f,0};
            const float r[]={0,.707f,.5f,0,0,.5f,0,.5f};
            float left=mode==1 ? l[channel]*.5f : (channel==0 ? .5f : 0);
            float right=mode==1 ? r[channel]*.5f : (channel==1 ? .5f : 0);
            assert(fabsf(s_mix_buffer[0]-left)<1e-6f);
            assert(fabsf(s_mix_buffer[1]-right)<1e-6f);
            for (unsigned i=2;i<512;i++) assert(s_mix_buffer[i]==0);
            assert(vm_read32(ea+channel*4)==0);
        }
    }
#ifdef _WIN32
    VirtualFree(vm_base, 0, MEM_RELEASE);
#else
    munmap(vm_base, 0x100000000ULL);
#endif
    puts("audio ring: 3 sizes, 3 wraps each passed");
    return 0;
}
