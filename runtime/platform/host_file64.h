#ifndef PS3RECOMP_HOST_FILE64_H
#define PS3RECOMP_HOST_FILE64_H

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Windows long and the default stat size remain 32-bit on x64. Disc archives
 * and PS3 file offsets require 64-bit sizes independently of pointer width. */
#ifdef _WIN32
#include <io.h>
typedef struct __stat64 ps3_host_stat;
#define ps3_host_stat_path _stat64
#define ps3_host_stat_fd _fstat64
#define ps3_host_fileno _fileno
#define ps3_host_seek _fseeki64
#define ps3_host_tell _ftelli64
#else
#include <unistd.h>
typedef struct stat ps3_host_stat;
#define ps3_host_stat_path stat
#define ps3_host_stat_fd fstat
#define ps3_host_fileno fileno
#define ps3_host_seek fseeko
#define ps3_host_tell ftello
#endif

/* PS3 open modifiers differ from both the Windows and Linux host constants. */
static inline int ps3_host_open_flags(uint32_t flags)
{
    int access = flags & 3;
    int host = access == 2 ? O_RDWR : access == 1 ? O_WRONLY : O_RDONLY;
    if (flags & 0x0040) host |= O_CREAT;
    if (flags & 0x0080) host |= O_EXCL;
    if (flags & 0x0200) host |= O_TRUNC;
    if (flags & 0x0400) host |= O_APPEND;
    return host;
}

#endif
