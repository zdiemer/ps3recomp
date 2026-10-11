#pragma once

/* Opt-in host timeline for firmware imports and LV2 calls. QPC scopes include
 * scheduling delays and nested calls; they are not CPU utilization measures.
 * Per-host-thread streams avoid a logging lock on the measured paths. */
#if defined(_WIN32) && defined(YZ_PERF_PROFILE)
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" uint8_t* vm_base;

struct PpuWaitProfile {
    const char* kind;
    uint32_t id, lr, guest_tid;
    uint64_t args[3];
    uint64_t stack_pointer = 0;
    ppu_context* ctx;
    LONGLONG begin = 0;

    PpuWaitProfile(ppu_context* c, const char* k, uint32_t i)
        : kind(k), id(i), ctx(c) {
        static const bool enabled = getenv("PPU_WAIT_PROFILE") != nullptr;
        if (!enabled) return;
        /* Live diagnostic switch permits an overhead comparison within one
         * process. Time-based checks also reach infrequently called workers. */
        static const char* control = getenv("PPU_WAIT_PROFILE_CONTROL");
        static thread_local ULONGLONG next_check = 0;
        static thread_local bool recording = true;
        if (control && GetTickCount64() >= next_check) {
            next_check = GetTickCount64() + 1000;
            FILE* file = fopen(control,"r"); unsigned value;
            if (file) {
                if (fscanf(file,"%u",&value)==1) recording=value!=0;
                fclose(file);
            }
        }
        if (!recording) return;
        guest_tid = (uint32_t)c->thread_id;
        lr = (uint32_t)c->lr;
        for (unsigned n = 0; n < 3; ++n) args[n] = c->gpr[n + 3];
        stack_pointer = c->gpr[1];
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        begin = t.QuadPart;
    }
    ~PpuWaitProfile() {
        if (!begin) return;
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        struct Stream {
            FILE* file = nullptr;
            LONGLONG frequency = 0;
            unsigned rows = 0;
            Stream() {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                frequency = f.QuadPart;
                char path[1024];
                const char* directory = getenv("PPU_WAIT_PROFILE");
                if (directory && *directory) {
                    snprintf(path, sizeof(path), "%s/wait-%lu.csv", directory,
                             GetCurrentThreadId());
                    file = fopen(path, "wb");
                    if (file) {
                        setvbuf(file, nullptr, _IOFBF, 256 * 1024);
                        fprintf(file, "begin_qpc,end_qpc,frequency,host_tid,guest_tid,kind,id,lr,arg0,arg1,arg2,result,stack_lr0,stack_lr1,stack_lr2\n");
                    }
                }
            }
            ~Stream() { if (file) fclose(file); }
        };
        static thread_local Stream stream;
        /* Keep waits >= 200 us. File creation/formatting is outside the scope.
         * The instrumented run must be compared with an uninstrumented run. */
        static const bool stacks = getenv("PPU_WAIT_STACKS") != nullptr;
        const bool signal = stacks && !strcmp(kind,"syscall") &&
            (id == 105 || id == 106 || id == 108);
        if (!stream.file || (!signal &&
            (end.QuadPart - begin) * 1000000 < stream.frequency * 200))
            return;
        uint32_t saved_lr[3] = {};
        if (stacks) {
            uint64_t sp = stack_pointer;
            for (unsigned n = 0; n < 3; ++n) {
                if (sp > 0xFFFFFFE8ull || (sp & 15)) break;
                const uintptr_t pointer = (uintptr_t)vm_base + (uintptr_t)sp;
                MEMORY_BASIC_INFORMATION page = {};
                if (!VirtualQuery((void*)pointer, &page, sizeof(page)) ||
                    page.State != MEM_COMMIT ||
                    (page.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                    pointer + 24 > (uintptr_t)page.BaseAddress + page.RegionSize) break;
                uint64_t back, ret;
                memcpy(&back, (void*)pointer, 8);
                memcpy(&ret, (void*)(pointer + 16), 8);
                back = _byteswap_uint64(back);
                saved_lr[n] = (uint32_t)_byteswap_uint64(ret);
                if (back <= sp || back - stack_pointer > 8u*1024*1024) break;
                sp = back;
            }
        }
        fprintf(stream.file, "%lld,%lld,%lld,%lu,%u,%s,%08X,%08X,%llX,%llX,%llX,%llX,%08X,%08X,%08X\n",
                begin, end.QuadPart, stream.frequency, GetCurrentThreadId(),
                guest_tid, kind, id, lr, (unsigned long long)args[0],
                (unsigned long long)args[1], (unsigned long long)args[2],
                (unsigned long long)ctx->gpr[3], saved_lr[0], saved_lr[1], saved_lr[2]);
        if ((++stream.rows & 63u) == 0) fflush(stream.file);
    }
};
#else
struct PpuWaitProfile {
    PpuWaitProfile(ppu_context*, const char*, uint32_t) {}
};
#endif
