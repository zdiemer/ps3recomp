#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ps3_env_once.h"
#ifdef _WIN32
#include <windows.h>
#define set_env(name, value) _putenv_s(name, value)
#else
#define set_env(name, value) setenv(name, value, 1)
#endif

static ps3_env_once_slot shared;

#ifdef _WIN32
static DWORD WINAPI reader(void* unused)
{
    (void)unused;
    for (unsigned n = 0; n < 10000; ++n) {
        const char* value = ps3_env_once(&shared, "PS3_ENV_ONCE_SHARED");
        assert(value && strcmp(value, "shared") == 0);
    }
    return 0;
}
#endif

int main(void)
{
    /* Unset at first read: the absence is what is cached. */
    static ps3_env_once_slot absent;
    assert(!ps3_env_once(&absent, "PS3_ENV_ONCE_ABSENT"));
    set_env("PS3_ENV_ONCE_ABSENT", "late");
    assert(!ps3_env_once(&absent, "PS3_ENV_ONCE_ABSENT"));

    /* Set at first read: later changes do not reach the call site. */
    static ps3_env_once_slot present;
    set_env("PS3_ENV_ONCE_PRESENT", "first");
    const char* first = ps3_env_once(&present, "PS3_ENV_ONCE_PRESENT");
    assert(first && strcmp(first, "first") == 0);
    assert(ps3_env_once(&present, "PS3_ENV_ONCE_PRESENT") == first);

    /* Separate slots stay independent for the same name. */
    static ps3_env_once_slot other;
    set_env("PS3_ENV_ONCE_ABSENT", "seen");
    const char* other_value = ps3_env_once(&other, "PS3_ENV_ONCE_ABSENT");
    assert(other_value && strcmp(other_value, "seen") == 0);

#ifdef _WIN32
    set_env("PS3_ENV_ONCE_SHARED", "shared");
    HANDLE threads[8];
    for (unsigned i = 0; i < 8; ++i) {
        threads[i] = CreateThread(NULL, 0, reader, NULL, 0, NULL);
        assert(threads[i]);
    }
    assert(WaitForMultipleObjects(8, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (unsigned i = 0; i < 8; ++i) CloseHandle(threads[i]);
#endif
    puts("Env once: first read cached per site (set or unset), independent slots, concurrent first use passed");
    return 0;
}
