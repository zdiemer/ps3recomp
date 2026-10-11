/* Compile with a generated ppu_recomp.h, /Gy /Gw and /OPT:REF on Windows.
 * Exercise the actual CRT handlers with a bounded big-endian guest memory.
 * A callback immediately after the condition variable must survive create.
 */
#include "../ppu_sysprx.cpp"
#include <assert.h>

static uint8_t memory[0x4000];
extern "C" {
uint8_t* vm_base = memory;
/* Unrelated runtime dependencies must never execute in this focused test. */
volatile long long g_gcm_ref_pub_count = 0;
uint32_t ppu_hle_inject_base = 0;
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) { abort(); }
void ps3_net_host_register(unsigned int (*)(unsigned int, unsigned int)) { abort(); }
void np_score_register_ctx() { abort(); }
void ppu_register_function(uint64_t, void (*)(ppu_context*)) { abort(); }
unsigned int cellGcmSetupContext(unsigned int, unsigned int, unsigned int,
    unsigned int, CellGcmGuestAlloc, CellGcmGuestWrite32) { abort(); }
void cellGcm_fifo_recycle(unsigned int) { abort(); }
void spu_raw_note_image(uint32_t, uint32_t) { abort(); }
unsigned long long ppu_thread_cpu_us(unsigned) { abort(); }
unsigned ppu_thread_prof_pc(unsigned) { abort(); }
int64_t sys_ppu_thread_create(ppu_context*) { abort(); }
int64_t sys_ppu_thread_exit(ppu_context*) { abort(); }
uint32_t vm_read32(uint64_t a) {
    assert(a + 4 <= sizeof memory);
    return (uint32_t)memory[a] << 24 | (uint32_t)memory[a + 1] << 16 |
           (uint32_t)memory[a + 2] << 8 | memory[a + 3];
}
uint64_t vm_read64(uint64_t a) { return (uint64_t)vm_read32(a) << 32 | vm_read32(a + 4); }
void vm_write32(uint64_t a, uint32_t v) {
    assert(a + 4 <= sizeof memory);
    for (unsigned i = 0; i < 4; ++i) memory[a + i] = (uint8_t)(v >> (24 - 8 * i));
}
void vm_write64(uint64_t a, uint64_t v) { vm_write32(a, (uint32_t)(v >> 32)); vm_write32(a + 4, (uint32_t)v); }
}

int main() {
    const uint32_t mutex = 0x1000, cond = 0x2004;
    ppu_context ctx = {};
    ctx.thread_id = 17;
    memset(memory + cond - 4, 0xA5, 20);
    vm_write32(cond + 8, 0x00E79E20);  /* adjacent allocator callback OPD */
    ctx.gpr[3] = cond; ctx.gpr[4] = mutex;
    sys_lwcond_create(&ctx);
    assert(ctx.gpr[3] == 0);
    assert(vm_read32(cond) == mutex);
    assert(vm_read32(cond + 4) != 0);
    assert(vm_read32(cond - 4) == 0xA5A5A5A5);
    assert(vm_read32(cond + 8) == 0x00E79E20);
    assert(vm_read32(cond + 12) == 0xA5A5A5A5);
    uint32_t first_id = vm_read32(cond + 4);
    ctx.gpr[3] = cond; ctx.gpr[4] = mutex;
    sys_lwcond_create(&ctx);
    assert(vm_read32(cond + 4) != first_id);
    assert(vm_read32(cond + 8) == 0x00E79E20);
#ifdef _WIN32
    HANDLE sem = lwm_sem(mutex);
    assert(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0);
    vm_write32(mutex + LWM_OWNER, 17);
    vm_write32(mutex + LWM_RECUR, 1);
    ctx.gpr[3] = cond;
    sys_lwcond_wait(&ctx);
    assert(ctx.gpr[3] == 0);
    assert(vm_read32(mutex + LWM_OWNER) == 17);
    assert(vm_read32(mutex + LWM_RECUR) == 1);
    assert(WaitForSingleObject(sem, 0) == WAIT_TIMEOUT);
    ReleaseSemaphore(sem, 1, NULL);
#endif
    puts("PASS: retail 8-byte lwcond layout, adjacent callback, unique IDs and paired mutex wait");
}
