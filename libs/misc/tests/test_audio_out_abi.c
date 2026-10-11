/* Actual audio-output queries: retail offsets, endian fields and canaries. */
#include "../cellAvconfExt.c"
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t a) { (void)a; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
int main(void)
{
    assert(sizeof(CellAudioOutSoundMode)==8);
    assert(sizeof(CellAudioOutDeviceInfo)==136);
    assert(offsetof(CellAudioOutDeviceInfo,availableModes)==8);
    assert(offsetof(CellAudioOutSoundMode,layout)==4);
    assert(CELL_AUDIO_OUT_CODING_TYPE_AC3==1 && CELL_AUDIO_OUT_CODING_TYPE_DTS==6);
    vm_base=malloc(0x1000); assert(vm_base); ppu_vm_size=0x1000;
    memset(vm_base,0xCC,0x1000);
    assert(cellAudioOutGetDeviceInfo(0,0,(CellAudioOutDeviceInfo*)(uintptr_t)0x100)==CELL_OK);
    assert(vm_read8(0x100)==0 && vm_read8(0x101)==3 && vm_read8(0x102)==1);
    const u8 channels[]={2,6,8};
    const u32 layouts[]={1,0x10000,0x40000000};
    for (u32 i=0;i<3;i++) {
        u32 ea=0x108+i*8;
        assert(vm_read8(ea)==0 && vm_read8(ea+1)==channels[i] && vm_read8(ea+2)==4);
        assert(vm_read32(ea+4)==layouts[i]);
        assert(cellAudioOutGetSoundAvailability2(0,0,4,channels[i],0)==1);
    }
    assert(cellAudioOutGetSoundAvailability(0,0,4,0)==8);
    assert(cellAudioOutGetSoundAvailability(0,0,0,0)==0);
    assert(cellAudioOutGetSoundAvailability(0,CELL_AUDIO_OUT_CODING_TYPE_AC3,4,0)==0);
    assert(vm_read8(0xFF)==0xCC && vm_read8(0x188)==0xCC);
    assert(cellAudioOutGetState(0,0,(void*)(uintptr_t)0x300)==CELL_OK);
    for (u32 i=0;i<12;i++) assert(vm_read8(0x300+i)==0);
    assert(vm_read8(0x30C)==0 && vm_read8(0x30D)==2 && vm_read8(0x30E)==4);
    assert(vm_read32(0x310)==1);
    assert(vm_read8(0x2FF)==0xCC && vm_read8(0x314)==0xCC);
    assert(cellAudioOutGetDeviceInfo(0,0,NULL)==CELL_EINVAL);
    assert(cellAudioOutGetState(0,0,NULL)==CELL_EINVAL);
    free(vm_base);
    puts("Audio-output device/mode/state retail ABI, endian and boundary checks passed.");
}
