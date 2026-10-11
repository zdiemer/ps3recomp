/* Exercise the host APIs used by cellFs against an actual sparse file >4 GiB. */
#include "../host_file64.h"
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

int main(void)
{
    char path[128];
#ifdef _WIN32
    snprintf(path, sizeof path, "local/file64-test-%lu.bin", GetCurrentProcessId());
#else
    snprintf(path, sizeof path, "local/file64-test-%ld.bin", (long)getpid());
#endif
    FILE* file = fopen(path, "w+b");
    assert(file);
#ifdef _WIN32
    DWORD returned;
    HANDLE handle = (HANDLE)_get_osfhandle(ps3_host_fileno(file));
    assert(DeviceIoControl(handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                           &returned, NULL));
#endif
    const int64_t offset = INT64_C(0x20000007B);
    assert(ps3_host_seek(file, offset, SEEK_SET) == 0);
    assert(ps3_host_tell(file) == offset);
    assert(fputc(0xA5, file) == 0xA5);
    assert(fflush(file) == 0);
    ps3_host_stat info;
    assert(ps3_host_stat_path(path, &info) == 0);
    assert(info.st_size == offset + 1);
    assert(ps3_host_seek(file, -1, SEEK_END) == 0);
    assert(ps3_host_stat_fd(ps3_host_fileno(file), &info) == 0);
    assert(info.st_size == offset + 1);
    assert(ps3_host_tell(file) == offset); /* Fstat must preserve read position. */
    assert(fgetc(file) == 0xA5);
    assert(ps3_host_tell(file) == offset + 1);
    assert(ps3_host_seek(file, -1, SEEK_SET) != 0);
    assert(fclose(file) == 0);
    /* The installer passes 0x241: write + create + truncate. Exercise actual
     * host effects, including append and exclusive creation, after a big file. */
#ifdef _WIN32
#define TEST_OPEN _open
#define TEST_WRITE _write
#define TEST_CLOSE _close
#define TEST_BINARY _O_BINARY
#else
#define TEST_OPEN open
#define TEST_WRITE write
#define TEST_CLOSE close
#define TEST_BINARY 0
#endif
    int fd = TEST_OPEN(path, ps3_host_open_flags(0x241) | TEST_BINARY, 0666);
    assert(fd >= 0);
    assert(TEST_WRITE(fd, "ABC", 3) == 3);
    assert(TEST_CLOSE(fd) == 0);
    assert(ps3_host_stat_path(path, &info) == 0 && info.st_size == 3);
    fd = TEST_OPEN(path, ps3_host_open_flags(0x401) | TEST_BINARY, 0666);
    assert(fd >= 0);
    assert(TEST_WRITE(fd, "D", 1) == 1);
    assert(TEST_CLOSE(fd) == 0);
    file = fopen(path, "rb"); assert(file);
    char bytes[4]; assert(fread(bytes, 1, 4, file) == 4);
    assert(bytes[0] == 'A' && bytes[3] == 'D');
    assert(fclose(file) == 0);
    assert(TEST_OPEN(path, ps3_host_open_flags(0xC1) | TEST_BINARY, 0666) < 0);
    assert(remove(path) == 0);
    fd = TEST_OPEN(path, ps3_host_open_flags(0x41) | TEST_BINARY, 0666);
    assert(fd >= 0); assert(TEST_CLOSE(fd) == 0);
    assert(remove(path) == 0);
    return 0;
}
