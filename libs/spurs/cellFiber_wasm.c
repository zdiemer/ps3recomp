/* The BLUS30094 import inventory contains no cellFiber imports. Do not claim
 * context-switch support on Wasm, where POSIX ucontext does not exist. */
#ifdef __EMSCRIPTEN__
#include "cellFiber.h"
#define UNAVAILABLE return CELL_ENOSYS
s32 cellFiberPpuInitialize(void) { UNAVAILABLE; }
s32 cellFiberPpuFinalize(void) { UNAVAILABLE; }
s32 cellFiberPpuCreateFiber(CellFiber* fiber, CellFiberEntry entry, u64 arg,
                          const CellFiberAttribute* attr) { UNAVAILABLE; }
s32 cellFiberPpuDeleteFiber(CellFiber fiber) { UNAVAILABLE; }
s32 cellFiberPpuSwitchFiber(CellFiber fiber) { UNAVAILABLE; }
s32 cellFiberPpuYieldFiber(void) { UNAVAILABLE; }
s32 cellFiberPpuExitFiber(void) { UNAVAILABLE; }
s32 cellFiberPpuGetCurrentFiber(CellFiber* fiber) { UNAVAILABLE; }
s32 cellFiberPpuGetFiberState(CellFiber fiber, u32* state) { UNAVAILABLE; }
s32 cellFiberPpuAttributeInitialize(CellFiberAttribute* attr) { UNAVAILABLE; }
s32 cellFiberPpuSleep(void) { UNAVAILABLE; }
s32 cellFiberPpuWakeup(CellFiber fiber) { UNAVAILABLE; }
#endif
