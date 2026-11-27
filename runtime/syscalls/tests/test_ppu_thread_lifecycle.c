/* PPU thread-table lifetimes against the real sys_ppu_thread.c.
 *
 * The table has 64 slots. Every thread used to be forced joinable, and an
 * exited thread kept its slot until a join or detach, so a title that creates
 * detached workers ran out after 63 lifetimes with CELL_EAGAIN (Haze issue
 * #120: repeated title/menu loader and mixer threads). Guest stacks came from
 * a bump allocator and were never reused either.
 *
 * This runs far more than 64 sequential lifetimes of every kind -- detached
 * (returning, or leaving through sys_ppu_thread_exit), joinable joined,
 * detached before and after exit -- plus concurrent creators racing exiting
 * threads, and checks slot state, exit status, join/detach errors, stack reuse
 * and zeroing, and that a genuinely full table still answers CELL_EAGAIN.
 *
 * Usage: test_ppu_thread_lifecycle [nounwind]
 *   nounwind: run with PS3_NO_THREAD_EXIT_UNWIND=1 already set in the
 *   environment; exits then return to the guest, and a slot must stay put
 *   until the body really returns. */
#include "../sys_ppu_thread.c"
#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/mman.h>
#include <time.h>
#endif

/* Symbols sys_ppu_thread.c references that this test provides. */
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t vm_read32(uint64_t a) { (void)a; return 0; }
void ppu_resv_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long t) { (void)t; return 0; }
int ps3_intr_wait(unsigned long long t) { (void)t; return 0; }

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } } while (0)

/* Guest scratch: tid outputs and join status outputs, one 16-byte cell each. */
#define SCRATCH       0x00010000u
#define SCRATCH_SIZE  0x00010000u
#define STACK_SIZE    0x4000u

/* Thread arg: low byte = mode, the rest = a token. */
enum {
    M_RETURN = 1,     /* return with r3 = token */
    M_EXIT,           /* sys_ppu_thread_exit(token) */
    M_GATE_EXIT,      /* wait for the gate, then exit(token) */
    M_DIRTY_EXIT,     /* scribble over the stack, then exit */
    M_ZERO_EXIT,      /* check the stack is zero, then exit */
    M_EXIT_CHECK,     /* exit; without the unwind, check the slot survived */
};

static volatile long s_gate;
static volatile long s_ran;
static volatile long s_bad;
static volatile long s_token_seen[8192];
static int s_nounwind;

static void pause_briefly(void)
{
#ifdef _WIN32
    Sleep(0);
#else
    struct timespec ts = {0, 50 * 1000}; nanosleep(&ts, 0);
#endif
}

static void guest_exit(ppu_context* ctx, uint64_t status)
{
    ctx->gpr[3] = status;
    sys_ppu_thread_exit(ctx);
}

static void entry(ppu_context* ctx)
{
    uint64_t arg = ctx->gpr[3];
    unsigned mode = (unsigned)(arg & 0xFF);
    uint64_t token = arg >> 8;
    if (token < sizeof s_token_seen / sizeof s_token_seen[0])
        __atomic_fetch_add(&s_token_seen[token], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_ran, 1, __ATOMIC_RELAXED);
    /* The descriptor this thread runs on must still describe this thread. */
    ppu_thread_info* self = &g_ppu_threads[ctx->thread_id - 1];
    if (self->ctx.thread_id != ctx->thread_id || self->state == PPU_THREAD_STATE_FREE)
        __atomic_fetch_add(&s_bad, 1, __ATOMIC_RELAXED);

    uint8_t* sp = (uint8_t*)vm_to_host((uint32_t)ctx->gpr[1]);
    switch (mode) {
    case M_RETURN:
        ctx->gpr[3] = token;
        return;
    case M_GATE_EXIT:
        while (!__atomic_load_n(&s_gate, __ATOMIC_ACQUIRE)) pause_briefly();
        guest_exit(ctx, token);
        return;
    case M_DIRTY_EXIT:
        memset(sp - 0x1000, 0xA5, 0x1000 + 48);
        guest_exit(ctx, token);
        return;
    case M_ZERO_EXIT:
        for (int i = -0x1000; i < 48; i++)
            if (sp[i] != 0) { __atomic_fetch_add(&s_bad, 1, __ATOMIC_RELAXED); break; }
        guest_exit(ctx, token);
        return;
    case M_EXIT_CHECK:
        guest_exit(ctx, token);
        /* Only reachable without the unwind: the guest runs on past exit, so
         * its slot must not have been handed back yet. */
        if (self->state == PPU_THREAD_STATE_FREE)
            __atomic_fetch_add(&s_bad, 1, __ATOMIC_RELAXED);
        ctx->gpr[3] = token;
        return;
    default:
        guest_exit(ctx, token);
        return;
    }
}

static uint32_t cell(unsigned i) { return SCRATCH + (i % (SCRATCH_SIZE / 16)) * 16; }

static uint64_t read_be64(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* Returns the CELL code; *tid receives the new id on success. */
static int32_t create(unsigned mode, uint64_t token, uint64_t flags, unsigned out_cell, uint64_t* tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = 1;
    ctx.gpr[3] = cell(out_cell);
    ctx.gpr[4] = 0x10000;
    ctx.gpr[5] = (token << 8) | mode;
    ctx.gpr[6] = 1000;
    ctx.gpr[7] = STACK_SIZE;
    ctx.gpr[8] = flags;
    ctx.gpr[9] = 0;
    int32_t rc = (int32_t)sys_ppu_thread_create(&ctx);
    if (rc == CELL_OK && tid) *tid = read_be64(cell(out_cell));
    return rc;
}

static int32_t join(uint64_t tid, unsigned out_cell, uint64_t* status)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = 1;
    ctx.gpr[3] = tid;
    ctx.gpr[4] = cell(out_cell);
    int32_t rc = (int32_t)sys_ppu_thread_join(&ctx);
    if (rc == CELL_OK && status) *status = read_be64(cell(out_cell));
    return rc;
}

static int32_t detach(uint64_t tid)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = 1;
    ctx.gpr[3] = tid;
    return (int32_t)sys_ppu_thread_detach(&ctx);
}

static int slot_state(uint64_t tid)
{
    table_lock();
    int s = g_ppu_threads[tid - 1].state;
    table_unlock();
    return s;
}

static int slot_field(uint64_t tid, size_t off)
{
    table_lock();
    int v = *(int*)((char*)&g_ppu_threads[tid - 1] + off);
    table_unlock();
    return v;
}

static int live_slots(void)
{
    int n = 0;
    table_lock();
    for (int i = 0; i < PPU_THREAD_MAX; i++)
        if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}

/* Wait (bounded) until only `want` slots are live. */
static void wait_live(int want, const char* what)
{
    for (long spins = 0; live_slots() != want; spins++) {
        CHECK(spins < 2000000, "%s: %d slots still live, want %d", what, live_slots(), want);
        pause_briefly();
    }
}

static void wait_host_done(uint64_t tid)
{
    for (long spins = 0; !slot_field(tid, offsetof(ppu_thread_info, host_done)); spins++) {
        CHECK(spins < 2000000, "tid %llu never finished", (unsigned long long)tid);
        pause_briefly();
    }
}

static uint32_t stack_bump(void)
{
    table_lock();
    uint32_t v = g_vm_stack_alloc.next_addr;
    table_unlock();
    return v;
}

/* ---- concurrent creators ------------------------------------------------ */

#define CREATORS          4
#define PER_CREATOR       400
static volatile long s_next_token;

#ifdef _WIN32
static DWORD WINAPI creator(LPVOID arg)
#else
static void* creator(void* arg)
#endif
{
    unsigned base = (unsigned)(uintptr_t)arg * 1000u;
    for (int i = 0; i < PER_CREATOR; i++) {
        long token = __atomic_add_fetch(&s_next_token, 1, __ATOMIC_RELAXED);
        unsigned mode = (i & 1) ? M_RETURN : M_EXIT;
        int32_t rc;
        while ((rc = create(mode, (uint64_t)token, 0, base + (unsigned)i, NULL)) == (int32_t)CELL_EAGAIN)
            pause_briefly();   /* momentarily full: other creators' threads still live */
        if (rc != CELL_OK) {
            fprintf(stderr, "creator: create failed 0x%X\n", (unsigned)rc);
            exit(1);
        }
    }
    return 0;
}

struct joiner_args { uint64_t tid; uint64_t status; int32_t rc; };

#ifdef _WIN32
static DWORD WINAPI joiner_proc(LPVOID p)
#else
static void* joiner_proc(void* p)
#endif
{
    struct joiner_args* a = (struct joiner_args*)p;
    a->rc = join(a->tid, 3, &a->status);
    return 0;
}

int main(int argc, char** argv)
{
    s_nounwind = argc > 1 && strcmp(argv[1], "nounwind") == 0;
    if (s_nounwind && !getenv("PS3_NO_THREAD_EXIT_UNWIND")) {
        fprintf(stderr, "nounwind mode needs PS3_NO_THREAD_EXIT_UNWIND=1\n");
        return 2;
    }

#ifdef _WIN32
    vm_base = VirtualAlloc(NULL, (SIZE_T)VM_TOTAL_SIZE, MEM_RESERVE, PAGE_READWRITE);
    CHECK(vm_base != NULL, "reserve");
    CHECK(VirtualAlloc(vm_base + SCRATCH, SCRATCH_SIZE, MEM_COMMIT, PAGE_READWRITE), "scratch");
    CHECK(VirtualAlloc(vm_base + VM_STACK_BASE, VM_STACK_REGION, MEM_COMMIT, PAGE_READWRITE), "stacks");
#else
    vm_base = mmap(NULL, VM_TOTAL_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(vm_base != MAP_FAILED, "reserve");
    CHECK(mprotect(vm_base + SCRATCH, SCRATCH_SIZE, PROT_READ | PROT_WRITE) == 0, "scratch");
    CHECK(mprotect(vm_base + VM_STACK_BASE, VM_STACK_REGION, PROT_READ | PROT_WRITE) == 0, "stacks");
#endif
    ppu_vm_size = 1;

    lv2_syscall_table table; memset(&table, 0, sizeof table);
    sys_ppu_thread_init(&table);
    g_ppu_thread_entry_trampoline = entry;
    CHECK(ppu_thread_register_main() == 1, "main id");
    const uint64_t joinable = SYS_PPU_THREAD_CREATE_JOINABLE;
    uint64_t tid = 0, status = 0;
    const uint32_t bump0 = stack_bump();

    /* 1. Detached lifetimes, far past 64, through both ways out. */
    for (unsigned i = 0; i < 300; i++) {
        unsigned mode = (i & 1) ? M_RETURN : (s_nounwind ? M_EXIT_CHECK : M_EXIT);
        CHECK(create(mode, i, 0, i, &tid) == CELL_OK, "detached create %u", i);
        CHECK(tid >= 2 && tid <= PPU_THREAD_MAX, "tid %llu", (unsigned long long)tid);
        wait_live(1, "detached lifetime");
    }
    CHECK(s_ran == 300, "ran %ld", s_ran);
    /* One stack serves every sequential lifetime. */
    CHECK(stack_bump() - bump0 <= 2 * (STACK_SIZE + 0x10000), "stack region grew 0x%X",
          stack_bump() - bump0);

    /* 2. Joinable lifetimes keep their slot and status until joined. */
    for (unsigned i = 0; i < 200; i++) {
        unsigned mode = (i & 1) ? M_RETURN : (s_nounwind ? M_EXIT_CHECK : M_EXIT);
        uint64_t token = 0x1000 + i;
        CHECK(create(mode, token, joinable, i, &tid) == CELL_OK, "joinable create %u", i);
        wait_host_done(tid);
        CHECK(slot_state(tid) == PPU_THREAD_STATE_FINISHED, "joinable %u state %d", i, slot_state(tid));
        CHECK(join(tid, i + 1, &status) == CELL_OK, "join %u", i);
        CHECK(status == token, "join %u status 0x%llX", i, (unsigned long long)status);
        CHECK(slot_state(tid) == PPU_THREAD_STATE_FREE, "joined slot not freed");
        CHECK(join(tid, i + 1, NULL) == (int32_t)CELL_ESRCH, "rejoin of a reaped thread");
    }

    /* 3. Joining or detaching a never-joinable thread is refused, and it still
     *    reclaims itself. */
    s_gate = 0;
    CHECK(create(M_GATE_EXIT, 7, 0, 0, &tid) == CELL_OK, "gated detached create");
    CHECK(join(tid, 1, NULL) == (int32_t)CELL_EINVAL, "join of a detached thread");
    CHECK(detach(tid) == (int32_t)CELL_EINVAL, "detach of a detached thread");
    __atomic_store_n(&s_gate, 1, __ATOMIC_RELEASE);
    wait_live(1, "gated detached");

    /* 4. Detach before exit: the thread proc retires the slot. */
    for (unsigned i = 0; i < 80; i++) {
        s_gate = 0;
        CHECK(create(M_GATE_EXIT, 8, joinable, i, &tid) == CELL_OK, "create for detach-before-exit");
        CHECK(detach(tid) == CELL_OK, "detach before exit");
        CHECK(detach(tid) == (int32_t)CELL_EINVAL, "second detach");
        CHECK(join(tid, 1, NULL) == (int32_t)CELL_EINVAL, "join after detach");
        __atomic_store_n(&s_gate, 1, __ATOMIC_RELEASE);
        wait_live(1, "detach before exit");
    }

    /* 5. Detach after exit retires the slot on the spot. */
    for (unsigned i = 0; i < 80; i++) {
        CHECK(create(M_EXIT, 9, joinable, i, &tid) == CELL_OK, "create for detach-after-exit");
        wait_host_done(tid);
        CHECK(detach(tid) == CELL_OK, "detach after exit");
        CHECK(slot_state(tid) == PPU_THREAD_STATE_FREE, "detach after exit kept the slot");
    }

    /* 6. A join in progress: detach is busy, a second join is refused. */
    {
        s_gate = 0;
        CHECK(create(M_GATE_EXIT, 0x77, joinable, 0, &tid) == CELL_OK, "create for busy join");
        /* The joiner runs as an ordinary host thread through the real path. */
        struct joiner_args ja = { tid, 0, -1 };
#ifdef _WIN32
        HANDLE jh = CreateThread(NULL, 0, joiner_proc, &ja, 0, NULL);
        CHECK(jh != NULL, "joiner thread");
#else
        pthread_t jh;
        CHECK(pthread_create(&jh, NULL, joiner_proc, &ja) == 0, "joiner thread");
#endif
        for (long spins = 0; !slot_field(tid, offsetof(ppu_thread_info, join_waiting)); spins++) {
            CHECK(spins < 2000000, "joiner never started");
            pause_briefly();
        }
        CHECK(detach(tid) == (int32_t)CELL_EBUSY, "detach during join");
        CHECK(join(tid, 2, NULL) == (int32_t)CELL_EINVAL, "second concurrent join");
        __atomic_store_n(&s_gate, 1, __ATOMIC_RELEASE);
#ifdef _WIN32
        WaitForSingleObject(jh, INFINITE); CloseHandle(jh);
#else
        pthread_join(jh, NULL);
#endif
        CHECK(ja.rc == CELL_OK && ja.status == 0x77, "joiner rc 0x%X status 0x%llX",
              (unsigned)ja.rc, (unsigned long long)ja.status);
        CHECK(live_slots() == 1, "joined slot still live");
    }

    /* 7. Genuinely occupied slots still answer EAGAIN, and leave the live
     *    threads intact. */
    {
        s_gate = 0;
        uint64_t tids[PPU_THREAD_MAX];
        int n = 0;
        for (; n < PPU_THREAD_MAX; n++) {
            int32_t rc = create(M_GATE_EXIT, 0x100 + n, n & 1 ? joinable : 0, n, &tids[n]);
            if (rc != CELL_OK) {
                CHECK(rc == (int32_t)CELL_EAGAIN, "full table rc 0x%X", (unsigned)rc);
                break;
            }
        }
        CHECK(n == PPU_THREAD_MAX - 1, "fit %d threads beside main", n);
        CHECK(create(M_RETURN, 1, 0, 0, NULL) == (int32_t)CELL_EAGAIN, "full table again");
        for (int i = 0; i < n; i++)
            CHECK(slot_state(tids[i]) == PPU_THREAD_STATE_RUNNING, "thread %d disturbed", i);
        __atomic_store_n(&s_gate, 1, __ATOMIC_RELEASE);
        for (int i = 0; i < n; i++) {
            if (!(i & 1)) continue;
            CHECK(join(tids[i], i, &status) == CELL_OK, "join full-table %d", i);
            CHECK(status == (uint64_t)(0x100 + i), "full-table %d status", i);
        }
        wait_live(1, "full table drained");
    }

    /* 8. A recycled stack comes back zeroed. */
    for (unsigned i = 0; i < 20; i++) {
        CHECK(create(M_DIRTY_EXIT, 10, 0, i, NULL) == CELL_OK, "dirty create");
        wait_live(1, "dirty");
        CHECK(create(M_ZERO_EXIT, 11, 0, i, NULL) == CELL_OK, "zero create");
        wait_live(1, "zero");
    }

    /* 9. Concurrent creators racing exiting threads. */
    {
        long ran0 = s_ran;
        s_next_token = 0;
        memset((void*)s_token_seen, 0, sizeof s_token_seen);
#ifdef _WIN32
        HANDLE hs[CREATORS];
        for (int i = 0; i < CREATORS; i++) {
            hs[i] = CreateThread(NULL, 0, creator, (LPVOID)(uintptr_t)i, 0, NULL);
            CHECK(hs[i] != NULL, "creator thread");
        }
        WaitForMultipleObjects(CREATORS, hs, TRUE, INFINITE);
        for (int i = 0; i < CREATORS; i++) CloseHandle(hs[i]);
#else
        pthread_t hs[CREATORS];
        for (int i = 0; i < CREATORS; i++)
            CHECK(pthread_create(&hs[i], NULL, creator, (void*)(uintptr_t)i) == 0, "creator thread");
        for (int i = 0; i < CREATORS; i++) pthread_join(hs[i], NULL);
#endif
        wait_live(1, "concurrent lifetimes");
        CHECK(s_ran - ran0 == CREATORS * PER_CREATOR, "ran %ld of %d", s_ran - ran0,
              CREATORS * PER_CREATOR);
        for (long t = 1; t <= CREATORS * PER_CREATOR; t++)
            CHECK(s_token_seen[t] == 1, "token %ld ran %ld times", t, s_token_seen[t]);
    }

    CHECK(s_bad == 0, "%ld threads saw a reused descriptor, dirty stack or freed slot", s_bad);
    CHECK(stack_bump() - bump0 <= (uint32_t)PPU_THREAD_MAX * (STACK_SIZE + 0x10000),
          "stack region grew 0x%X over %ld lifetimes", stack_bump() - bump0, s_ran);
    printf("ppu thread lifecycle%s: %ld lifetimes, %lu slots retired, stack region 0x%X bytes\n",
           s_nounwind ? " (no exit unwind)" : "", s_ran, s_slots_retired, stack_bump() - bump0);
    return 0;
}

