/*
 * ps3recomp - Memory management syscalls (implementation)
 */

#include "sys_memory.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_mem_alloc_info       g_sys_mem_allocs[SYS_MEMORY_ALLOC_MAX];
sys_mem_container_info   g_sys_mem_containers[SYS_MEMORY_CONTAINER_MAX];
sys_mmapper_shared_info  g_sys_mmapper_shared[SYS_MMAPPER_SHARED_MAX];

/* Bump allocator for main memory pool.
 * Starts after a reasonable offset to avoid the low addresses used by ELF. */
uint32_t g_sys_mem_bump_ptr = 0;

/* Total user memory size (default 213 MB, after kernel reservation) */
#define SYS_MEM_USER_TOTAL  (213 * 1024 * 1024)

/* Guest window handed out by sys_memory_allocate / sys_mmapper.
 * 0x40000000+ matches where the real lv2 places these allocations (verified
 * against an RPCS3 boot log of Yakuza: Dead Souls); the window is outside
 * the pre-committed main region, so pages are committed on demand. */
#define SYS_MEM_ALLOC_BASE  0x40000000u
#define SYS_MEM_ALLOC_END   0x50000000u

static uint32_t s_total_allocated = 0;

/* Caller holds the allocator lock. Search live ranges, including mmapper
 * reservations, so freed space can serve subsequent movies and levels. */
static uint32_t memory_find_range(uint32_t size, uint32_t alignment)
{
    uint64_t candidate = SYS_MEM_ALLOC_BASE;
    for (;;) {
        candidate = (candidate + alignment - 1) & ~((uint64_t)alignment - 1);
        if (candidate + size > SYS_MEM_ALLOC_END) return 0;
        int overlap = 0;
        for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i) {
            const sys_mem_alloc_info* a = &g_sys_mem_allocs[i];
            if (a->active && candidate < (uint64_t)a->addr + a->size &&
                candidate + size > a->addr) {
                candidate = (uint64_t)a->addr + a->size;
                overlap = 1;
                break;
            }
        }
        if (!overlap) return (uint32_t)candidate;
    }
}

/* Guest threads are real host threads; serialize the bump allocator. */
#ifdef _WIN32
static SRWLOCK s_bump_lock = SRWLOCK_INIT;
static void bump_lock(void)   { AcquireSRWLockExclusive(&s_bump_lock); }
static void bump_unlock(void) { ReleaseSRWLockExclusive(&s_bump_lock); }
#else
#include <pthread.h>
static pthread_mutex_t s_bump_mtx = PTHREAD_MUTEX_INITIALIZER;
static void bump_lock(void)   { pthread_mutex_lock(&s_bump_mtx); }
static void bump_unlock(void) { pthread_mutex_unlock(&s_bump_mtx); }
#endif

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_memory_allocate
 *
 * r3 = size
 * r4 = flags (page size)
 * r5 = pointer to receive allocated address (u32*)
 * -----------------------------------------------------------------------*/
static int64_t memory_allocate(uint64_t size64, uint32_t cid, uint64_t flags,
                               uint32_t addr_out)
{
    uint32_t alignment = flags == SYS_MEMORY_PAGE_SIZE_64K ? 0x10000 :
                         (flags == SYS_MEMORY_PAGE_SIZE_1M || flags == 0) ? 0x100000 : 0;
    if (!alignment) return (int64_t)(int32_t)CELL_EINVAL;
    if (!size64 || size64 % alignment) return (int64_t)(int32_t)CELL_EALIGN;
    if (!addr_out) return (int64_t)(int32_t)CELL_EFAULT;
    if (size64 > UINT32_MAX) return (int64_t)(int32_t)CELL_ENOMEM;
    uint32_t size = (uint32_t)size64;
    bump_lock();
    sys_mem_container_info* container = NULL;
    if (cid) {
        if (cid > SYS_MEMORY_CONTAINER_MAX || !g_sys_mem_containers[cid - 1].active) {
            bump_unlock(); return (int64_t)(int32_t)CELL_ESRCH;
        }
        container = &g_sys_mem_containers[cid - 1];
        if (size > container->total_size - container->used_size) {
            bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
        }
    }
    uint32_t addr = memory_find_range(size, alignment);
    if (!addr) {
        bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
    }
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i)
        if (!g_sys_mem_allocs[i].active) { slot = i; break; }
    if (slot < 0 || vm_commit(addr, size) != CELL_OK) {
        bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
    }
    sys_mem_alloc_info* allocation = &g_sys_mem_allocs[slot];
    allocation->addr = addr; allocation->size = size;
    allocation->container_id = (int32_t)cid; allocation->page_size = alignment;
    allocation->active = 1;
    if (addr + size > g_sys_mem_bump_ptr) g_sys_mem_bump_ptr = addr + size;
    s_total_allocated += size;
    if (container) container->used_size += size;
    write_be32(addr_out, addr);
    bump_unlock();
    fprintf(stderr, "[sys_memory] allocate size=0x%X container=%u flags=0x%llX -> 0x%08X\n",
            size, cid, (unsigned long long)flags, addr);
    return CELL_OK;
}

int64_t sys_memory_allocate(ppu_context* ctx)
{
    return memory_allocate(ctx->gpr[3], 0, ctx->gpr[4], LV2_ARG_PTR(ctx, 2));
}

/* r3=size, r4=container, r5=page flags, r6=guest u32 output address. */
int64_t sys_memory_allocate_from_container(ppu_context* ctx)
{
    uint32_t cid = LV2_ARG_U32(ctx, 1);
    if (!cid) return (int64_t)(int32_t)CELL_ESRCH;
    return memory_allocate(ctx->gpr[3], cid, ctx->gpr[5], LV2_ARG_PTR(ctx, 3));
}

/* ---------------------------------------------------------------------------
 * sys_memory_free
 *
 * r3 = address
 * -----------------------------------------------------------------------*/
int64_t sys_memory_free(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);

    bump_lock();
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        sys_mem_alloc_info* a = &g_sys_mem_allocs[i];
        if (a->active && a->container_id >= 0 && a->addr == addr) {
            s_total_allocated -= a->size;
            if (a->container_id > 0)
                g_sys_mem_containers[a->container_id - 1].used_size -= a->size;
            a->active = 0;
            bump_unlock();
            return CELL_OK;
        }
    }
    bump_unlock();
    return (int64_t)(int32_t)CELL_EINVAL;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_user_memory_size
 *
 * r3 = pointer to output struct: { u32 total, u32 available }
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_user_memory_size(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);

    fprintf(stderr, "[sys_memory] get_user_memory_size()\n");
    if (out_addr != 0) {
        uint32_t total = SYS_MEM_USER_TOTAL;
        uint32_t avail = total - s_total_allocated;
        write_be32(out_addr + 0, total);
        write_be32(out_addr + 4, avail);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_get_page_attribute (syscall 351 / 0x15F)
 *
 * r3 = address to query
 * r4 = pointer to sys_page_attribute_t:
 *        +0x00 attribute   (u64)
 *        +0x08 access_right(u64)
 *        +0x10 page_size   (u32)   <- the field titles actually check
 *        +0x14 pad         (u32)
 *
 * Was unimplemented (fell through to the return-0 stub), so the output struct
 * was never filled -> callers reading page_size got stack garbage. Dantelion2's
 * graphics init (func_009F6D40) queries a 1MB-page buffer and only proceeds to
 * construct a sub-object when page_size == 0x100000; the stub made it take the
 * failure path, leaving that sub-object NULL -> later null-deref crash.
 * -----------------------------------------------------------------------*/
int64_t sys_memory_get_page_attribute(ppu_context* ctx)
{
    uint32_t addr     = LV2_ARG_U32(ctx, 0);
    uint32_t attr_out = LV2_ARG_PTR(ctx, 1);

    /* Report the page size of the allocation containing addr. */
    uint32_t page_size = 0x10000;  /* default 64K */
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; i++) {
        if (g_sys_mem_allocs[i].active &&
            addr >= g_sys_mem_allocs[i].addr &&
            addr <  g_sys_mem_allocs[i].addr + g_sys_mem_allocs[i].size) {
            page_size = g_sys_mem_allocs[i].page_size ? g_sys_mem_allocs[i].page_size : 0x10000;
            break;
        }
    }

    if (attr_out != 0) {
        write_be32(attr_out + 0x00, 0);          /* attribute    (hi) */
        write_be32(attr_out + 0x04, 0x00040000); /* attribute    (lo) PROT_READ_WRITE-ish */
        write_be32(attr_out + 0x08, 0);          /* access_right (hi) */
        write_be32(attr_out + 0x0C, 0x00000008); /* access_right (lo) PPU_THREAD-ish */
        write_be32(attr_out + 0x10, page_size);  /* page_size */
        write_be32(attr_out + 0x14, 0);          /* pad */
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_create
 *
 * r3 = pointer to receive container ID (u32*)
 * r4 = size
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t size        = LV2_ARG_U32(ctx, 1);

    size &= ~0xFFFFFu;
    if (!size) return (int64_t)(int32_t)CELL_ENOMEM;
    if (!id_out_addr) return (int64_t)(int32_t)CELL_EFAULT;
    bump_lock();
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_CONTAINER_MAX; i++) {
        if (!g_sys_mem_containers[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        bump_unlock(); return (int64_t)(int32_t)CELL_EAGAIN;
    }

    sys_mem_container_info* c = &g_sys_mem_containers[slot];
    c->active     = 1;
    c->total_size = size;
    c->used_size  = 0;

    uint32_t container_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, container_id);
    }
    bump_unlock();
    fprintf(stderr, "[sys_memory] container_create size=0x%X -> %u\n", size, container_id);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_destroy
 *
 * r3 = container_id
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_destroy(ppu_context* ctx)
{
    uint32_t cid = LV2_ARG_U32(ctx, 0);
    bump_lock();
    if (!cid || cid > SYS_MEMORY_CONTAINER_MAX || !g_sys_mem_containers[cid - 1].active) {
        bump_unlock(); return (int64_t)(int32_t)CELL_ESRCH;
    }
    sys_mem_container_info* c = &g_sys_mem_containers[cid - 1];
    if (c->used_size) { bump_unlock(); return (int64_t)(int32_t)CELL_EBUSY; }
    c->active = 0;
    bump_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_memory_container_get_size
 *
 * r3 = container_id
 * r4 = pointer to output struct: { u32 total, u32 used }
 * -----------------------------------------------------------------------*/
int64_t sys_memory_container_get_size(ppu_context* ctx)
{
    uint32_t container_id = LV2_ARG_U32(ctx, 0);
    uint32_t out_addr     = LV2_ARG_PTR(ctx, 1);

    if (container_id == 0 || container_id > SYS_MEMORY_CONTAINER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mem_container_info* c = &g_sys_mem_containers[container_id - 1];
    if (!c->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (out_addr != 0) {
        write_be32(out_addr + 0, c->total_size);
        write_be32(out_addr + 4, c->total_size - c->used_size);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_address
 *
 * r3 = size
 * r4 = flags
 * r5 = alignment
 * r6 = pointer to receive address (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_address(ppu_context* ctx)
{
    uint64_t size64 = ctx->gpr[3];
    uint64_t alignment64 = ctx->gpr[5];
    uint32_t addr_out = LV2_ARG_PTR(ctx, 3);
    if (!alignment64) alignment64 = 0x10000;
    if (alignment64 > UINT32_MAX || (alignment64 & (alignment64 - 1)))
        return (int64_t)(int32_t)CELL_EALIGN;
    if (!size64 || size64 > UINT32_MAX) return (int64_t)(int32_t)CELL_ENOMEM;
    if (!addr_out) return (int64_t)(int32_t)CELL_EFAULT;
    uint64_t rounded = (size64 + alignment64 - 1) & ~(alignment64 - 1);
    if (rounded > UINT32_MAX) return (int64_t)(int32_t)CELL_ENOMEM;
    uint32_t size = (uint32_t)rounded, alignment = (uint32_t)alignment64;
    bump_lock();
    int slot = -1;
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i)
        if (!g_sys_mem_allocs[i].active) { slot = i; break; }
    uint32_t addr = memory_find_range(size, alignment);
    if (slot < 0 || !addr || vm_commit(addr, size) != CELL_OK) {
        bump_unlock(); return (int64_t)(int32_t)CELL_ENOMEM;
    }
    sys_mem_alloc_info* a = &g_sys_mem_allocs[slot];
    a->addr = addr; a->size = size; a->container_id = -1;
    a->page_size = alignment; a->active = 1;
    if (addr + size > g_sys_mem_bump_ptr) g_sys_mem_bump_ptr = addr + size;
    write_be32(addr_out, addr);
    bump_unlock();
    return CELL_OK;
}

int64_t sys_mmapper_free_address(ppu_context* ctx)
{
    uint32_t addr = LV2_ARG_U32(ctx, 0);
    bump_lock();
    for (int i = 0; i < SYS_MEMORY_ALLOC_MAX; ++i) {
        sys_mem_alloc_info* a = &g_sys_mem_allocs[i];
        if (a->active && a->container_id == -1 && a->addr == addr) {
            a->active = 0;
            bump_unlock(); return CELL_OK;
        }
    }
    bump_unlock();
    return (int64_t)(int32_t)CELL_EINVAL;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_allocate_shared_memory
 *
 * r3 = key
 * r4 = size
 * r5 = flags
 * r6 = pointer to receive shared mem ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_allocate_shared_memory(ppu_context* ctx)
{
    uint64_t key      = LV2_ARG_U64(ctx, 0);
    uint32_t size     = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */
    uint32_t id_out   = LV2_ARG_PTR(ctx, 3);

    int slot = -1;
    for (int i = 0; i < SYS_MMAPPER_SHARED_MAX; i++) {
        if (!g_sys_mmapper_shared[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[slot];
    s->active = 1;
    s->size   = VM_ALIGN_UP(size, VM_PAGE_SIZE);
    s->key    = key;
    s->addr   = 0;

    uint32_t shm_id = (uint32_t)(slot + 1);
    if (id_out != 0) {
        write_be32(id_out, shm_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_mmapper_map_shared_memory
 *
 * r3 = address (where to map)
 * r4 = shared mem ID
 * r5 = flags
 * -----------------------------------------------------------------------*/
int64_t sys_mmapper_map_shared_memory(ppu_context* ctx)
{
    uint32_t addr   = LV2_ARG_U32(ctx, 0);
    uint32_t shm_id = LV2_ARG_U32(ctx, 1);
    /* uint32_t flags = LV2_ARG_U32(ctx, 2); */

    if (shm_id == 0 || shm_id > SYS_MMAPPER_SHARED_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_mmapper_shared_info* s = &g_sys_mmapper_shared[shm_id - 1];
    if (!s->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Commit the memory at the specified address */
    int32_t rc = vm_commit(addr, s->size);
    if (rc != CELL_OK)
        return (int64_t)(int32_t)rc;

    s->addr = addr;
    memset(vm_to_host(addr), 0, s->size);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_memory_init(lv2_syscall_table* tbl)
{
    memset(g_sys_mem_allocs,     0, sizeof(g_sys_mem_allocs));
    memset(g_sys_mem_containers, 0, sizeof(g_sys_mem_containers));
    memset(g_sys_mmapper_shared, 0, sizeof(g_sys_mmapper_shared));
    g_sys_mem_bump_ptr = 0;
    s_total_allocated  = 0;

    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE,             sys_memory_allocate);
    lv2_syscall_register(tbl, SYS_MEMORY_ALLOCATE_FROM_CONTAINER, sys_memory_allocate_from_container);
    lv2_syscall_register(tbl, SYS_MEMORY_FREE,                 sys_memory_free);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_USER_MEMORY_SIZE, sys_memory_get_user_memory_size);
    lv2_syscall_register(tbl, SYS_MEMORY_GET_PAGE_ATTRIBUTE,   sys_memory_get_page_attribute);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_CREATE,     sys_memory_container_create);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_DESTROY,    sys_memory_container_destroy);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_CREATE_DEBUG, sys_memory_container_create);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_DESTROY_DEBUG, sys_memory_container_destroy);
    lv2_syscall_register(tbl, SYS_MEMORY_CONTAINER_GET_SIZE,   sys_memory_container_get_size);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_ADDRESS,    sys_mmapper_allocate_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_FREE_ADDRESS,        sys_mmapper_free_address);
    lv2_syscall_register(tbl, SYS_MMAPPER_ALLOCATE_SHARED_MEMORY, sys_mmapper_allocate_shared_memory);
    lv2_syscall_register(tbl, SYS_MMAPPER_MAP_SHARED_MEMORY,  sys_mmapper_map_shared_memory);
}
