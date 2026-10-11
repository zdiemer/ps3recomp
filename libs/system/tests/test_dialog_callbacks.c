/* Standalone test: include both callback producers and the real deferred queue. */
#define vm_write8 test_vm_write8
#define vm_write32 test_vm_write32
#include "../cellMsgDialog.c"
#undef vm_write8
#undef vm_write32
/* Match the scaffold's exported 64-bit-address memory wrappers. */
void vm_write8(uint64_t ea, uint8_t v) { test_vm_write8((uint32_t)ea, v); }
void vm_write32(uint64_t ea, uint32_t v) { test_vm_write32((uint32_t)ea, v); }
#include "../cellSysutil.c"
#include <assert.h>
#include <stdlib.h>
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
void np_psnr_pump(void) {}
const char* cellGame_get_title_id(void) { return "TEST00001"; }

static unsigned calls;
static int waiting;
static s32 last_result;
static unsigned system_events;
static u64 last_system_event;
static void callback(u32 opd, u64 result, u64 user, u64 c, u64 d,
                     u64 e, u64 f, u64 g, u64 h)
{
    if (opd == 0x300 || opd == 0x308) {
        assert(user == 0 && c == (opd == 0x300 ? 0x5678 : 0x5679));
        assert(!d && !e && !f && !g && !h);
        assert(result == CELL_SYSUTIL_DRAWING_BEGIN || result == CELL_SYSUTIL_DRAWING_END);
        last_system_event = result;
        system_events++;
        return;
    }
    assert(opd == 0x100 && user == 0x1234 && !c && !d && !e && !f && !g && !h);
    assert(waiting);
    assert(last_system_event == CELL_SYSUTIL_DRAWING_END);
    calls++; last_result = (s32)result;
    if (calls == 1) {
        assert(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO,
            (void*)0x200, (void*)0x100, (void*)0x1234, NULL) == CELL_OK);
        cellSysutilCheckCallback();
        assert(calls == 1);
    }
}
int main(void)
{
    vm_base = calloc(1, 65536); assert(vm_base);
    memcpy(vm_base + 0x200, "Install data", 13);
    g_ps3_guest_caller = callback;
    assert(cellSysutilRegisterCallback(0, (CellSysutilCallback)(uintptr_t)0x300,
                                      (void*)0x5678) == CELL_OK);
    assert(cellSysutilRegisterCallback(1, (CellSysutilCallback)(uintptr_t)0x308,
                                      (void*)0x5679) == CELL_OK);
    assert(cellMsgDialogOpen2(0xA0, (void*)0x200, (void*)0x100,
                            (void*)0x1234, NULL) == CELL_OK);
    assert(calls == 0 && system_events == 0);
    waiting = 1;
    cellSysutilCheckCallback(); assert(calls == 1 && last_result == CELL_MSGDIALOG_BUTTON_OK);
    assert(system_events == 4);
    cellSysutilCheckCallback(); assert(calls == 2 && last_result == CELL_MSGDIALOG_BUTTON_YES);
    assert(system_events == 8);
    cellSysutilCheckCallback(); assert(calls == 2);
    assert(system_events == 8);
    assert(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE,
        (void*)0x200, (void*)0x100, (void*)0x1234, NULL) == CELL_OK);
    cellSysutilCheckCallback(); assert(calls == 2);
    assert(system_events == 10 && last_system_event == CELL_SYSUTIL_DRAWING_BEGIN);
    assert(cellMsgDialogClose(0) == CELL_OK); assert(calls == 2);
    g_ps3_guest_caller = NULL;
    cellSysutilCheckCallback(); assert(calls == 2);
    g_ps3_guest_caller = callback;
    cellSysutilCheckCallback(); assert(calls == 3 && last_result == CELL_MSGDIALOG_BUTTON_NONE);
    cellSysutilCheckCallback(); assert(calls == 3);

    /* The yes/no auto-answer. DEFAULT is a blanket YES, including for a prompt
     * that offers to abort something -- see cellMsgDialog.c for why the
     * semantically better answer is opt-in (`MSGDIALOG_ANSWER=auto`): Virtua
     * Fighter 5 boots further on the blanket YES, and correctness about the
     * button is worth less than the title booting. Assert the default, so that
     * if it is ever flipped it is flipped deliberately. */
    memcpy(vm_base + 0x300, "Do you want to cancel the load operation?", 42);
    assert(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_SE_TYPE_NORMAL |
                              CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO,
                              (void*)0x300, (void*)0x100, (void*)0x1234, NULL) == CELL_OK);
    cellSysutilCheckCallback();
    assert(calls == 4 && last_result == CELL_MSGDIALOG_BUTTON_YES);
    assert(cellMsgDialogOpen2(CELL_MSGDIALOG_TYPE_PROGRESSBAR_SINGLE,
        (void*)0x200, (void*)0x100, (void*)0x1234, NULL) == CELL_OK);
    assert(cellMsgDialogOpen2(0, (void*)0x200, NULL, NULL, NULL) == (s32)0x8002B105);
    cellSysutilCheckCallback();
    unsigned before_abort = system_events;
    assert(cellMsgDialogAbort() == CELL_OK);
    cellSysutilCheckCallback();
    assert(calls == 4 && system_events == before_abort + 2);
    assert(last_system_event == CELL_SYSUTIL_DRAWING_END);

    /* The heuristic =auto would use, tested directly: the decision is cached on
     * first use so the env cannot be flipped mid-run, but the predicate is the
     * whole of the logic and it is right here in this translation unit. */
    assert(msg_offers_to_abort("Are you sure you want to cancel checking the game data?"));
    assert(msg_offers_to_abort("Do you want to cancel the load operation?"));
    assert(msg_offers_to_abort("CANCEL?"));
    assert(msg_offers_to_abort("Quit the game?"));
    assert(msg_offers_to_abort("Abort installation?"));
    assert(!msg_offers_to_abort("Do you want to use game data? The HDD access indicator will flash"));
    assert(!msg_offers_to_abort("Save complete."));
    assert(!msg_offers_to_abort(""));
    assert(!msg_offers_to_abort(NULL));


    free(vm_base);
    puts("Deferred dialog and button-mask checks passed");
}
