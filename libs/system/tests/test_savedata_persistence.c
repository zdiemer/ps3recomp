/* Actual callback/file operations, including missing-load behavior and a
 * persistent absolute root across working-directory changes. Windows runner. */
#include "../cellSaveData.c"
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
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; abort(); }
ps3_guest_caller_fn g_ps3_guest_caller;
static unsigned step;
static int missing, loading;
static const char payload[] = "persistent checkpoint";

static void callback(u32 opd, u64 cb, u64 get, u64 set,
                     u64 a, u64 b, u64 c, u64 d, u64 e)
{
    assert(!a && !b && !c && !d && !e);
    if (opd == 0x800) {
        assert(vm_read32(get + 4) == (missing || !loading));
        if (missing) { vm_write32(cb, (u32)CELL_SAVEDATA_CBRESULT_ERR_NODATA); return; }
        if (!loading) {
            strcpy((char*)vm_base + get + 64, "Persistence test");
            vm_write32(set, (u32)get + 64);
        } else {
            assert(!strcmp((char*)vm_base + get + 64, "Persistence test"));
            assert(vm_read32(get + 1628) == 1);
        }
    } else {
        assert(opd == 0x900);
        assert(vm_read32(get) == (step ? sizeof(payload) : 0));
        if (step++) { vm_write32(cb, CELL_SAVEDATA_CBRESULT_OK_LAST); return; }
        vm_write32(set, loading ? CELL_SAVEDATA_FILEOP_READ : CELL_SAVEDATA_FILEOP_WRITE);
        vm_write32(set + 8, CELL_SAVEDATA_FILETYPE_SECUREFILE);
        vm_write32(set + 28, 0x400);
        vm_write32(set + 36, sizeof(payload));
        vm_write32(set + 40, sizeof(payload));
        vm_write32(set + 44, loading ? 0x600 : 0x500);
    }
}

int main(void)
{
    char cwd[1024], root[1200], save[1200], path[1400];
    assert(_getcwd(cwd, sizeof(cwd)));
    snprintf(root, sizeof(root), "%s/local/savedata-test-%lu", cwd, GetCurrentProcessId());
    assert(_mkdir(root) == 0);
    assert(cellSaveData_set_root_path(NULL) == CELL_SAVEDATA_ERROR_PARAM);
    assert(cellSaveData_set_root_path("") == CELL_SAVEDATA_ERROR_PARAM);
    assert(cellSaveData_set_root_path(root) == CELL_OK);
    vm_base = calloc(1, 0x200000); assert(vm_base);
    assert(cellSaveData_set_scratch_region(0x100000, 0x20000) == CELL_OK);
    strcpy((char*)vm_base + 0x300, "TEST00000");
    strcpy((char*)vm_base + 0x400, "DATA.BIN");
    memcpy(vm_base + 0x500, payload, sizeof(payload));
    vm_write32(0x204, 2);
    g_ps3_guest_caller = callback;
    missing = 1;
    assert(cellSaveDataAutoLoad2(0, (void*)0x300, 0, (void*)0x200,
                               (void*)0x800, (void*)0x900, 0, NULL) == CELL_OK);
    cellSaveData_set_strict_load_errors(1);
    assert(cellSaveDataAutoLoad2(0, (void*)0x300, 0, (void*)0x200,
                               (void*)0x800, (void*)0x900, 0, NULL) == CELL_SAVEDATA_ERROR_NODATA);
    assert(cellSaveDataAutoLoad(0, (void*)0x300, 0, (void*)0x200,
                              (void*)0x800, (void*)0x900, 0, NULL) == CELL_SAVEDATA_ERROR_NODATA);
    missing = 0;
    assert(cellSaveDataAutoSave2(0, (void*)0x300, 0, (void*)0x200,
                               (void*)0x800, (void*)0x900, 0, NULL) == CELL_OK);
    /* A different launch directory must still read the same save. */
    assert(_chdir(root) == 0);
    loading = 1; step = 0;
    assert(cellSaveDataAutoLoad2(0, (void*)0x300, 0, (void*)0x200,
                               (void*)0x800, (void*)0x900, 0, NULL) == CELL_OK);
    assert(!memcmp(vm_base + 0x600, payload, sizeof(payload)));
    assert(_chdir(cwd) == 0);
    snprintf(save, sizeof(save), "%s/TEST00000", root);
    snprintf(path, sizeof(path), "%s/DATA.BIN", save); assert(_unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/PARAM.SFO", save); assert(_unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/PARAM.PFD", save); assert(_unlink(path) == 0);
    assert(_rmdir(save) == 0); assert(_rmdir(root) == 0);
    free(vm_base);
    puts("Persistent save root, metadata/byte round trip and missing-load errors passed");
}
