/* Cross-thread publication regression for the reservation fast predicate.
 * Build as one native C++ fixture linked with runtime/spu/spu_coherency.c.
 * No guest VM or SPU context is needed: each iteration reserves one new line
 * through the production API and checks both the inline and ABI predicates. */

#include "../spu_coherency.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

extern "C" void spu_ch_wake(spu_context*) {}

namespace {
constexpr uint32_t kCount = 4096;
constexpr uint32_t kFirstLine = 0x12000000u;
std::atomic<uint32_t> ready{0};
std::atomic<uint32_t> observed{0};
std::atomic<uint32_t> failures{0};

bool wait_at_least(std::atomic<uint32_t>& value, uint32_t target)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (value.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void check(bool passed)
{
    if (!passed) failures.fetch_add(1, std::memory_order_relaxed);
}

void reader()
{
    for (uint32_t i = 0; i < kCount; ++i) {
        const uint32_t line = kFirstLine + i * 128u;
        const uint32_t index = i + 1u;
        check(!spu_coh_is_reserved_inline(line));
        check(!spu_coh_is_reserved(line));
        ready.store(index, std::memory_order_release);

        /* Poll the actual predicate; no sideband event makes the bitmap fresh. */
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!spu_coh_is_reserved_inline(line + 127u)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                check(false);
                return;
            }
        }
        check(spu_coh_is_reserved(line));
        observed.store(index, std::memory_order_release);
    }
}

void reserver()
{
    for (uint32_t i = 0; i < kCount; ++i) {
        const uint32_t line = kFirstLine + i * 128u;
        const uint32_t index = i + 1u;
        if (!wait_at_least(ready, index)) { check(false); return; }
        spu_lockline_lock();
        spu_coh_reserve(nullptr, line);
        spu_lockline_unlock();
        if (!wait_at_least(observed, index)) { check(false); return; }
    }
}
} // namespace

int main()
{
    std::thread p(reader);
    std::thread s(reserver);
    p.join();
    s.join();
    const uint32_t failed = failures.load(std::memory_order_relaxed);
    if (failed) {
        std::fprintf(stderr, "coherency atomic visibility: %u failures\n", failed);
        return 1;
    }
    std::puts("coherency atomic visibility: 4096 cross-thread predicate polls passed");
    return 0;
}
