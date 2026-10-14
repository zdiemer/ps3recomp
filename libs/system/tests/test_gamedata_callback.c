/* Exercise the actual HLE callback, with a synthetic guest and no game files. */
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
static int metadata_stat_error;
#ifdef _WIN32
static int test_host_stat(const char* path, struct __stat64* value)
{
    if (metadata_stat_error && strstr(path, "/PARAM.SFO")) {
        errno = metadata_stat_error; return -1;
    }
    return _stat64(path, value);
}
#define _stat64(path, value) test_host_stat(path, value)
#else
static int test_host_stat(const char* path, struct stat* value)
{
    if (metadata_stat_error && strstr(path, "/PARAM.SFO")) {
        errno = metadata_stat_error; return -1;
    }
    return stat(path, value);
}
#define stat(path, value) test_host_stat(path, value)
#endif
#include "../cellGame.c"
#ifdef _WIN32
#define TEST_RMDIR _rmdir
#define TEST_SETENV(name, value) _putenv_s(name, value)
#else
#define TEST_RMDIR rmdir
#define TEST_SETENV(name, value) setenv(name, value, 1)
#endif

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

static void host_path_case(unsigned root_length, int legacy_parent)
{
    char base[CELL_GAME_HOST_PATH_MAX], root[CELL_GAME_HOST_PATH_MAX];
    char hdd[CELL_GAME_HOST_PATH_MAX], alternate[CELL_GAME_HOST_PATH_MAX];
    char parent[CELL_GAME_HOST_PATH_MAX], usrdir[CELL_GAME_HOST_PATH_MAX];
    char sfo[CELL_GAME_HOST_PATH_MAX], truncated_usr[CELL_GAME_PATH_MAX];
    char truncated_sfo[CELL_GAME_PATH_MAX], text[128];
#ifdef _WIN32
    snprintf(base, sizeof base, "local/game-hostpaths-%lu-%u", GetCurrentProcessId(), root_length);
#else
    snprintf(base, sizeof base, "local/game-hostpaths-%ld-%u", (long)getpid(), root_length);
#endif
    assert(strlen(base) + 1 < root_length && root_length < 200);
    assert(ensure_dirs(base));
    snprintf(hdd, sizeof hdd, "%s/", base);
    size_t start = strlen(hdd);
    memset(hdd + start, 'x', root_length - 5 - start);
    hdd[root_length - 5] = 0;
    snprintf(root, sizeof root, "%s/game", hdd);
    cellGame_set_content_path(root);
    assert(strcmp(content_root(), root) == 0);
    snprintf(alternate, sizeof alternate, "%s/unrelated-disc-root", base);
    assert(TEST_SETENV("PS3_HDD0_ROOT", hdd) == 0);
    ppu_vfs_root = alternate; s_content_path_resolved = 0;
    assert(strcmp(content_root(), root) == 0);
    assert(cellGameBootCheck(NULL, NULL, NULL, NULL) == CELL_OK);
    snprintf(parent, sizeof parent, "%s/TEST00001", root);
    snprintf(usrdir, sizeof usrdir, "%s/USRDIR", parent);
    snprintf(sfo, sizeof sfo, "%s/PARAM.SFO", parent);
    assert(strlen(usrdir) >= CELL_GAME_PATH_MAX && strlen(sfo) >= CELL_GAME_PATH_MAX);
    snprintf(truncated_usr, sizeof truncated_usr, "%s", usrdir);
    snprintf(truncated_sfo, sizeof truncated_sfo, "%s", sfo);
    if (legacy_parent) {
        /* Reproduce the installed-path failure: the correct parent exists,
         * but the old 127-byte destinations are US and PA instead of full names. */
        assert(root_length == 114);
        assert(ensure_dirs(parent));
        assert(strcmp(truncated_usr + strlen(parent), "/US") == 0);
        assert(strcmp(truncated_sfo + strlen(parent), "/PA") == 0);
        assert(HOST_MKDIR(truncated_usr) == 0);
        FILE* old = fopen(truncated_sfo, "wb"); assert(old);
        assert(fputs("legacy truncated metadata", old) >= 0); assert(fclose(old) == 0);
        assert(!dir_exists(usrdir));
    }
    expected_new = 1; /* A bare parent or truncated PA is not installed metadata. */
    callback_result = CELL_GAMEDATA_CBRESULT_OK_CANCEL;
    provide_param = 0;
    expect_persisted = 0;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    assert(!dir_exists(usrdir));
    HOST_STAT_T absent;
    assert(HOST_STAT(sfo, &absent) != 0 && errno == ENOENT);
    callback_result = CELL_GAMEDATA_CBRESULT_OK;
    provide_param = 1;
    expect_persisted = 0;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    assert(dir_exists(usrdir));
    assert(sfo_read_string(sfo, "TITLE", text, sizeof text) == 0);
    assert(strcmp(text, "Persisted Title") == 0);
    assert(sfo_read_string(sfo, "VERSION", text, sizeof text) == 0);
    assert(strcmp(text, "02.34") == 0);
    /* Existing metadata loads through the complete host path. Acceptance with
     * no new parameters still repairs a missing USRDIR; cancellation does not. */
    assert(TEST_RMDIR(usrdir) == 0);
    provide_param = 0; expect_persisted = 1; expected_new = 0;
    callback_result = CELL_GAMEDATA_CBRESULT_OK_CANCEL;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    assert(!dir_exists(usrdir));
    callback_result = CELL_GAMEDATA_CBRESULT_OK;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    assert(dir_exists(usrdir));
    /* Public outputs remain short guest paths, with 128-byte ABI buffers. */
    memset(vm_base + 0x400, 0xCD, CELL_GAME_PATH_MAX + 1);
    memset(vm_base + 0x500, 0xCD, CELL_GAME_PATH_MAX + 1);
    assert(cellGameDataCheck(3, (const char*)(uintptr_t)0x100, NULL) == CELL_OK);
    assert(cellGameCreateGameData((void*)(uintptr_t)0x300, (char*)(uintptr_t)0x400,
                                 (char*)(uintptr_t)0x500) == CELL_OK);
    assert(cellGameContentPermit((char*)(uintptr_t)0x400, (char*)(uintptr_t)0x500) == CELL_OK);
    assert(strcmp((char*)vm_base + 0x400, "/dev_hdd0/game/TEST00001") == 0);
    assert(strcmp((char*)vm_base + 0x500, "/dev_hdd0/game/TEST00001/USRDIR") == 0);
    assert(vm_base[0x400 + CELL_GAME_PATH_MAX] == 0xCD);
    assert(vm_base[0x500 + CELL_GAME_PATH_MAX] == 0xCD);
    assert(!dir_exists(alternate)); /* HDD overrides never create a second install. */
    /* An existing file where USRDIR belongs is an access error, not success. */
    assert(TEST_RMDIR(usrdir) == 0);
    FILE* blocked = fopen(usrdir, "wb"); assert(blocked); assert(fclose(blocked) == 0);
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_GAME_ERROR_ACCESS_ERROR);
    assert(remove(usrdir) == 0);
    assert(remove(sfo) == 0);
    if (legacy_parent) {
        assert(dir_exists(truncated_usr)); /* Do not remove unrelated legacy data. */
        assert(remove(truncated_sfo) == 0);
        assert(TEST_RMDIR(truncated_usr) == 0);
    }
    assert(TEST_RMDIR(parent) == 0);
    assert(TEST_RMDIR(root) == 0);
    assert(TEST_RMDIR(hdd) == 0);
    assert(TEST_RMDIR(base) == 0);
    assert(TEST_SETENV("PS3_HDD0_ROOT", "") == 0);
    ppu_vfs_root = ".";
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
#else
    snprintf(s_content_path, sizeof s_content_path, "local/gamedata-test-%ld", (long)getpid());
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
    expected_new = 1;
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_OK);
    assert(calls == 7);
    /* Permission and invalid-parent errors are not an absent installation. */
    const int metadata_errors[] = {EACCES, ENOTDIR, EIO};
    for (unsigned i = 0; i < sizeof metadata_errors / sizeof metadata_errors[0]; ++i) {
        metadata_stat_error = metadata_errors[i];
        assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                        (void*)(uintptr_t)0x1234, 0) == CELL_GAME_ERROR_ACCESS_ERROR);
        assert(calls == 7);
    }
    metadata_stat_error = 0;
    assert(HOST_MKDIR(sfo) == 0); /* A directory named PARAM.SFO is not missing data. */
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_GAME_ERROR_ACCESS_ERROR);
    assert(calls == 7);
    assert(TEST_RMDIR(sfo) == 0);
    assert(cellGameDataCheckCreate2(1, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_GAME_ERROR_PARAM);
    assert(calls == 7);
    assert(TEST_RMDIR(fixture) == 0);
    assert(TEST_RMDIR(s_content_path) == 0);
    host_path_case(114, 1);
    host_path_case(160, 0);
    /* Both VFS-derived and explicitly configured roots must keep host bytes. */
    char vfs[180], resolved[CELL_GAME_HOST_PATH_MAX];
    memset(vfs, 'v', sizeof vfs - 1); vfs[sizeof vfs - 1] = 0;
#ifdef _WIN32
    assert(_putenv_s("PS3_HDD0_ROOT", "") == 0);
#else
    assert(unsetenv("PS3_HDD0_ROOT") == 0);
#endif
    ppu_vfs_root = vfs; s_content_path_resolved = 0;
    snprintf(resolved, sizeof resolved, "%s/game", vfs);
    assert(strcmp(content_root(), resolved) == 0);
    ppu_vfs_root = ".";
    /* Reject the entire operation if any complete endpoint exceeds capacity.
     * No callback runs and no truncated filesystem target can be created. */
    char oversized[CELL_GAME_HOST_PATH_MAX + 32];
    memset(oversized, 'z', sizeof oversized - 1); oversized[sizeof oversized - 1] = 0;
    int previous_calls = calls;
    cellGame_set_content_path(oversized);
    assert(content_root() == NULL);
    assert(cellGameBootCheck(NULL, NULL, NULL, NULL) == CELL_GAME_ERROR_ACCESS_ERROR);
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_GAME_ERROR_ACCESS_ERROR);
    assert(calls == previous_calls);
    oversized[CELL_GAME_HOST_PATH_MAX - 2] = 0;
    cellGame_set_content_path(oversized); /* Root fits; composed title/suffix do not. */
    assert(content_root() != NULL);
    assert(cellGameDataCheckCreate2(0, (const char*)(uintptr_t)0x100, 0,
                                    (void*)(uintptr_t)0x1234, 0) == CELL_GAME_ERROR_ACCESS_ERROR);
    assert(cellGameCreateGameData((void*)(uintptr_t)0x300, NULL, NULL) == CELL_GAME_ERROR_ACCESS_ERROR);
    assert(gamedata_write_sfo(oversized, GAMEDATA_CB_BASE + 0x20 + 0x860) == -1);
    assert(calls == previous_calls);
    cellGame_set_content_path(".");
    free(vm_base);
    puts("Game-data host paths: long installs, truncated-parent recovery, guest ABI and overflow rejection passed");
    return 0;
}
