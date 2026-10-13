/* Verify that the diagnostic separates unavailable rings from lock waits. */
#define YZ_PERF_PROFILE 1
#define main queue_contract_main
#include "test_queue_size.c"
#undef main

static HANDLE lock_acquired;
static DWORD WINAPI claimed_producer(void* ignored)
{
    (void)ignored;
    Sleep(40);
    spu_lockline_lock();
    queue_write_index(vm_base + 0x100, 0);
    queue_write_index(vm_base + 0x104, 1);
    spu_lockline_unlock();
    return 0;
}
static DWORD WINAPI lock_holder(void* ignored)
{
    (void)ignored;
    spu_lockline_lock();
    SetEvent(lock_acquired);
    Sleep(40);
    spu_lockline_unlock();
    return 0;
}
int main(void)
{
    char directory[MAX_PATH];
    assert(GetEnvironmentVariableA("SPURS_QUEUE_PROFILE", directory, sizeof(directory)));
    /* The existing contract supplies a delayed producer on an empty queue. */
    assert(queue_contract_main() == 0);
    vm_base = (uint8_t*)calloc(1, 0x1000);
    assert(_cellSpursQueueInitialize(0x800, 0x900, 0x100, 0x400, 16, 8, 1) == CELL_OK);
    vm_write32(0x100, ~0u);
    HANDLE producer = CreateThread(NULL, 0, claimed_producer, NULL, 0, NULL);
    assert(producer);
    assert(cellSpursQueuePopBody(0x100, 0x300, 0, 1) == CELL_OK);
    assert(WaitForSingleObject(producer, 1000) == WAIT_OBJECT_0);
    CloseHandle(producer);
    vm_write32(0x100, 0);
    lock_acquired = CreateEvent(NULL, TRUE, FALSE, NULL);
    producer = CreateThread(NULL, 0, lock_holder, NULL, 0, NULL);
    assert(producer && lock_acquired);
    assert(WaitForSingleObject(lock_acquired, 1000) == WAIT_OBJECT_0);
    assert(cellSpursQueuePopBody(0x100, 0x300, 0, 1) == CELL_OK);
    assert(WaitForSingleObject(producer, 1000) == WAIT_OBJECT_0);
    CloseHandle(producer);
    CloseHandle(lock_acquired);
    fflush(queue_profile_output.file);
    char path[MAX_PATH + 64];
    snprintf(path, sizeof(path), "%s/queue-%lu.csv", directory, GetCurrentThreadId());
    FILE* input = fopen(path, "r");
    assert(input);
    char line[1024];
    assert(fgets(line, sizeof(line), input));
    unsigned unavailable_cases = 0, claimed_cases = 0, lock_cases = 0;
    while (fgets(line, sizeof(line), input)) {
        long long begin, end, frequency, lock_ticks, sleep_ticks;
        unsigned tid, queue, taskset, push, blocking, peek, result, attempts, busy, unavailable;
        assert(sscanf(line, "%lld,%lld,%lld,%u,%x,%x,%u,%u,%u,%x,%u,%u,%u,%lld,%lld",
            &begin, &end, &frequency, &tid, &queue, &taskset, &push, &blocking, &peek,
            &result, &attempts, &busy, &unavailable, &lock_ticks, &sleep_ticks) == 15);
        assert(end >= begin && lock_ticks >= 0 && sleep_ticks >= 0);
        assert(lock_ticks + sleep_ticks <= end - begin);
        if (!blocking || push || result) continue;
        assert(queue == 0x100 && taskset == 0x900 && !peek);
        if (unavailable && !busy && sleep_ticks * 1000 / frequency >= 20) ++unavailable_cases;
        if (busy && !unavailable && sleep_ticks * 1000 / frequency >= 20) ++claimed_cases;
        if (attempts == 1 && !sleep_ticks && lock_ticks * 1000 / frequency >= 20) ++lock_cases;
    }
    assert(unavailable_cases && claimed_cases && lock_cases);
    fclose(input);
    free(vm_base);
    puts("SPURS queue timing: empty, claimed and lock-contention costs distinguished");
    return 0;
}
