/* Real syscall handlers, guest BE outputs and committed host backing. */
#include "../sys_memory.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
static u32 read_be32(u32 ea)
{
    const u8* p = vm_base + ea;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
int main(void)
{
#ifdef _WIN32
    vm_base = VirtualAlloc(NULL, (SIZE_T)VM_TOTAL_SIZE, MEM_RESERVE, PAGE_READWRITE);
    assert(vm_base);
    assert(VirtualAlloc(vm_base + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE));
#else
    vm_base = mmap(NULL, VM_TOTAL_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(vm_base != MAP_FAILED);
    assert(mprotect(vm_base + 0x10000, 0x10000, PROT_READ | PROT_WRITE) == 0);
#endif
    lv2_syscall_table table = {0};
    sys_memory_init(&table);
    /* Retail guest ABI uses 341/342; 324/325 are debug aliases. */
    assert(table.handlers[341] == sys_memory_container_create);
    assert(table.handlers[342] == sys_memory_container_destroy);
    assert(table.handlers[324] == sys_memory_container_create);
    assert(table.handlers[325] == sys_memory_container_destroy);
    assert(table.handlers[350] == sys_memory_allocate_from_container);
    ppu_context ctx = {0};
    ctx.gpr[3] = 0x10000; ctx.gpr[4] = 0x200000;
    assert(table.handlers[341](&ctx) == CELL_OK);
    u32 cid = read_be32(0x10000); assert(cid != 0);
    ctx.gpr[3] = 0x100000; ctx.gpr[4] = cid;
    ctx.gpr[5] = SYS_MEMORY_PAGE_SIZE_1M; ctx.gpr[6] = 0x10004;
    assert(table.handlers[350](&ctx) == CELL_OK);
    u32 allocation = read_be32(0x10004);
    assert(allocation >= SYS_MEM_ALLOC_BASE && !(allocation & 0xFFFFF));
    assert(vm_base[allocation] == 0); vm_base[allocation + 0xFFFFF] = 0x5A;
    ctx.gpr[3] = cid; ctx.gpr[4] = 0x10008;
    assert(sys_memory_container_get_size(&ctx) == CELL_OK);
    assert(read_be32(0x10008) == 0x200000 && read_be32(0x1000C) == 0x100000);
    ctx.gpr[3] = cid;
    assert(sys_memory_container_destroy(&ctx) == (int64_t)(int32_t)CELL_EBUSY);
    ctx.gpr[3] = 0x200000; ctx.gpr[4] = cid;
    ctx.gpr[5] = SYS_MEMORY_PAGE_SIZE_1M; ctx.gpr[6] = 0x10010;
    write_be32(0x10010, 0xDEADBEEF);
    assert(sys_memory_allocate_from_container(&ctx) == (int64_t)(int32_t)CELL_ENOMEM);
    assert(read_be32(0x10010) == 0xDEADBEEF);
    ctx.gpr[3] = 0x10001;
    assert(sys_memory_allocate_from_container(&ctx) == (int64_t)(int32_t)CELL_EALIGN);
    ctx.gpr[3] = 0x100000; ctx.gpr[5] = 0x123;
    assert(sys_memory_allocate_from_container(&ctx) == (int64_t)(int32_t)CELL_EINVAL);
    ctx.gpr[5] = SYS_MEMORY_PAGE_SIZE_64K; ctx.gpr[6] = 0;
    assert(sys_memory_allocate_from_container(&ctx) == (int64_t)(int32_t)CELL_EFAULT);
    ctx.gpr[3] = allocation;
    assert(sys_memory_free(&ctx) == CELL_OK);
    assert(sys_memory_free(&ctx) == (int64_t)(int32_t)CELL_EINVAL);
    /* Repeat the movie/level buffer lifetime beyond the whole 256 MB window. */
    for (int i = 0; i < 300; ++i) {
        ctx.gpr[3] = 0x100000; ctx.gpr[4] = cid;
        ctx.gpr[5] = SYS_MEMORY_PAGE_SIZE_1M; ctx.gpr[6] = 0x10004;
        assert(table.handlers[350](&ctx) == CELL_OK);
        assert(read_be32(0x10004) == allocation);
        ctx.gpr[3] = allocation;
        assert(sys_memory_free(&ctx) == CELL_OK);
    }
    /* Reserved mmapper ranges must remain unavailable to ordinary allocations. */
    ctx.gpr[3] = 0x100000; ctx.gpr[5] = 0x100000; ctx.gpr[6] = 0x10014;
    assert(sys_mmapper_allocate_address(&ctx) == CELL_OK);
    assert(read_be32(0x10014) == allocation);
    ctx.gpr[3] = 0x100000; ctx.gpr[4] = cid;
    ctx.gpr[5] = SYS_MEMORY_PAGE_SIZE_1M; ctx.gpr[6] = 0x10004;
    assert(table.handlers[350](&ctx) == CELL_OK);
    assert(read_be32(0x10004) == allocation + 0x100000);
    ctx.gpr[3] = allocation;
    assert(sys_memory_free(&ctx) == (int64_t)(int32_t)CELL_EINVAL);
    assert(sys_mmapper_free_address(&ctx) == CELL_OK);
    ctx.gpr[3] = read_be32(0x10004);
    assert(sys_memory_free(&ctx) == CELL_OK);
    ctx.gpr[3] = cid; ctx.gpr[4] = 0x10008;
    assert(sys_memory_container_get_size(&ctx) == CELL_OK);
    assert(read_be32(0x1000C) == 0x200000);
    assert(table.handlers[342](&ctx) == CELL_OK);
    ctx.gpr[3] = 0x10000; ctx.gpr[4] = cid;
    ctx.gpr[5] = SYS_MEMORY_PAGE_SIZE_64K; ctx.gpr[6] = 0x10010;
    assert(sys_memory_allocate_from_container(&ctx) == (int64_t)(int32_t)CELL_ESRCH);
    vm_shutdown();
    puts("Memory-container allocation and lifetime checks passed");
    return 0;
}
