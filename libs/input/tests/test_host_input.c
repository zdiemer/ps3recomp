/* Exercise actual cellPad guest packets, host overrides and diagnostic sticks. */
#include "../cellPad.c"
#include <assert.h>
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
static int active=1;
static int callback(u32 port, CellPadHostInput* input, void* user)
{
    assert(user == &active);
    if (port) return 0;
    if (active) {
        input->buttons=CELL_PAD_CTRL_R1|CELL_PAD_CTRL_CROSS|CELL_PAD_CTRL_START;
        input->lx=11; input->ly=242; input->rx=213; input->ry=19;
    }
    return 1;
}
static u16 word(unsigned index) { return vm_read16(0x100+4+index*2); }
static void read_packet(void)
{
    /* GetTickCount64 can retain the same value across a 10 ms sleep. Wait
     * for the backend's actual clock to advance before requesting a report;
     * otherwise a stale report can consume a diagnostic stick pulse. */
    const unsigned long long begin=pad_now_ms();
    do { Sleep(1); } while (pad_now_ms()-begin<PAD_POLL_INTERVAL_MS);
    assert(cellPadGetData(0,(CellPadData*)(uintptr_t)0x100)==CELL_OK);
    assert(vm_read32(0x100)==CELL_PAD_LEN_CHANGE_PRESS_ON);
}
int main(void)
{
    char path[512];
    snprintf(path,sizeof(path),"local/pad-input-test-%lu.txt",GetCurrentProcessId());
    _putenv_s("PAD_FILE",path);
    vm_base=calloc(1,0x10000); ppu_vm_size=0x10000; assert(vm_base);
    cellPad_set_host_input_callback(callback,&active);
    assert(cellPadInit(1)==CELL_OK);
    assert(cellPadSetPressMode(0,1)==CELL_OK);
    read_packet();
    assert(word(CELL_PAD_BTN_OFFSET_DIGITAL1)==8);
    assert(word(CELL_PAD_BTN_OFFSET_DIGITAL2)==0x48);
    assert(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==11);
    assert(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y)==242);
    assert(word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X)==213);
    assert(word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y)==19);
    assert(word(CELL_PAD_BTN_OFFSET_PRESS_R1)==255);
    assert(word(CELL_PAD_BTN_OFFSET_PRESS_CROSS)==255);
    active=0;
    read_packet();
    assert(!word(CELL_PAD_BTN_OFFSET_DIGITAL1) && !word(CELL_PAD_BTN_OFFSET_DIGITAL2));
    assert(word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X)==128);
    assert(!word(CELL_PAD_BTN_OFFSET_PRESS_R1));
    FILE* f=fopen(path,"wb"); assert(f);
    fputs("sticks 12 244 230 17 2\n",f); fclose(f);
    for (int i=0;i<2;i++) {
        read_packet();
        assert(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==12);
        assert(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y)==244);
        assert(word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X)==230);
        assert(word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y)==17);
    }
    read_packet();
    assert(word(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X)==128);
    assert(word(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X)==128);
    assert(cellPadEnd()==CELL_OK);
    assert(remove(path)==0);
    free(vm_base);
    puts("Host input override, guest packet, pressure and bounded analog input checks passed.");
}
