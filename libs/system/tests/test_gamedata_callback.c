/* Exercise the actual HLE callback, with a synthetic guest and no game files. */
#include <assert.h>
#include "../cellGame.c"

uint8_t* vm_base;
const char* ppu_vfs_root = ".";
ps3_guest_caller_fn g_ps3_guest_caller;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void spu_lockline_lock(void) { }
void spu_lockline_unlock(void) { }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t v, int w) { (void)ea; (void)v; (void)w; }

static int calls;
static u32 expected_new;
static s32 callback_result = CELL_GAMEDATA_CBRESULT_OK_CANCEL;
static int provide_param;
static int expect_persisted;
static void callback(uint32_t opd, uint64_t cb, uint64_t get, uint64_t set,
                     uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    assert(opd == 0x1234);
    assert(cb + 16 <= get && get + 0x1448 <= set);
    assert(vm_read32((u32)get + 4) == expected_new);
    assert(strcmp((char*)vm_base + get + 8, "/dev_hdd0/game/TEST00001") == 0);
    assert(strcmp((char*)vm_base + get + 0x427, "/dev_hdd0/game/TEST00001/USRDIR") == 0);
    assert(strcmp((char*)vm_base + get + 0x12E0, "TEST00001") == 0);
    assert(vm_read32((u32)get + 0x13FC) == (expected_new ? 0 : 0xFFFFFFFFu));
    assert(vm_read32((u32)get + 0x1400) == 0);
    assert(vm_read32((u32)set) == 0);
    /* Firmware reserved bytes and localized titles must not contain sizeKB. */
    assert(vm_read32((u32)get + 0xB9C) == 0);
    assert(vm_base[get + 0x1447] == 0);
    assert(vm_base[set + 0x20] == 0xCD); /* scratch boundary stays intact */
    if (expect_persisted) {
        assert(strcmp((char*)vm_base + get + 0x860, "Persisted Title") == 0);
        assert(strcmp((char*)vm_base + get + 0x860 + 0xA8C, "02.34") == 0);
        assert(strcmp((char*)vm_base + get + 0x860 + 0x80, "Localized title") == 0);
        assert(vm_read32((u32)get + 0x860 + 0xA94) == 7);
    }
    if (provide_param) {
        u32 param = (u32)get + 0x860;
        strcpy((char*)vm_base + param, "Persisted Title");
        strcpy((char*)vm_base + param + 0xA8C, "02.34");
        strcpy((char*)vm_base + param + 0x80, "Localized title");
        vm_write32(param + 0xA94, 7);
        vm_write32((u32)set, param);
    }
    vm_write32((u32)cb, callback_result);
    calls++;
}

int main(void)
{
    vm_base = (uint8_t*)malloc(GAMEDATA_CB_BASE + 0x2000);
    assert(vm_base);
    memset(vm_base, 0xCD, GAMEDATA_CB_BASE + 0x2000);
    strcpy((char*)vm_base + 0x100, "TEST00001");
    g_ps3_guest_caller = callback;
    expected_new = 1;
    assert(gamedata_stat_callback("test", 0, 0x100, 0x1234, expected_new) == 1);
    expected_new = 0;
    assert(gamedata_stat_callback("test", 0, 0x100, 0x1234, expected_new) == 1);
    assert(calls == 2);
    /* Public API must distinguish absent and existing directories. The test
     * runner creates local/; this fixture stays there and contains no assets. */
#ifdef _WIN32
    snprintf(s_content_path, sizeof s_content_path, "local/gamedata-test-%lu", GetCurrentProcessId());
#define TEST_RMDIR _rmdir
#else
    snprintf(s_content_path, sizeof s_content_path, "local/gamedata-test-%ld", (long)getpid());
#define TEST_RMDIR rmdir
#endif
    s_content_path_resolved = 1;
    assert(HOST_MKDIR(s_content_path) == 0);
    expected_new = 1;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    char fixture[CELL_GAME_PATH_MAX];
    snprintf(fixture, sizeof fixture, "%s/TEST00001", s_content_path);
    assert(!dir_exists(fixture)); /* OK_CANCEL must not create data. */
    callback_result = CELL_GAMEDATA_CBRESULT_OK;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_GAME_ERROR_PARAM);
    assert(!dir_exists(fixture)); /* New data requires callback parameters. */
    provide_param = 1;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    char usrdir[CELL_GAME_PATH_MAX], sfo[CELL_GAME_PATH_MAX], text[128];
    snprintf(usrdir, sizeof usrdir, "%s/USRDIR", fixture);
    snprintf(sfo, sizeof sfo, "%s/PARAM.SFO", fixture);
    assert(dir_exists(usrdir));
    assert(sfo_read_string(sfo, "TITLE", text, sizeof text) == 0);
    assert(strcmp(text, "Persisted Title") == 0);
    assert(sfo_read_string(sfo, "VERSION", text, sizeof text) == 0);
    assert(strcmp(text, "02.34") == 0);
    assert(sfo_read_string(sfo, "CATEGORY", text, sizeof text) == 0);
    assert(strcmp(text, "GD") == 0);
    assert(sfo_read_string(sfo, "TITLE_00", text, sizeof text) == 0);
    assert(strcmp(text, "Localized title") == 0);
    callback_result = CELL_GAMEDATA_CBRESULT_OK_CANCEL;
    provide_param = 0;
    expect_persisted = 1;
    expected_new = 0;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    expect_persisted = 0;
    assert(remove(sfo) == 0);
    assert(TEST_RMDIR(usrdir) == 0);
    assert(TEST_RMDIR(fixture) == 0);
    assert(HOST_MKDIR(fixture) == 0);
    expected_new = 0;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    assert(calls == 7);
    assert(cellGameDataCheckCreate2(1, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_GAME_ERROR_PARAM);
    assert(calls == 7);
    assert(TEST_RMDIR(fixture) == 0);
    assert(TEST_RMDIR(s_content_path) == 0);
    free(vm_base);
    return 0;
}
