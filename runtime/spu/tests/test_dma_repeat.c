/* Regression: repeated transfers are legal, and watchdogs belong to one SPU. */
#include "../spu_dma.h"
#include <assert.h>

int main(void)
{
    mfc_engine a, b;
    mfc_engine_init(&a);
    a.repeat_limit = 0;
    for (unsigned i = 0; i < 10000; ++i)
        assert(!mfc_repeat_limit_reached(&a, 0x10000, 0x3000, 16, MFC_GET_CMD));

    a.repeat_limit = 3;
    mfc_engine_init(&b);
    b.repeat_limit = 3;
    assert(!mfc_repeat_limit_reached(&a, 0x10000, 0x3000, 16, MFC_GET_CMD));
    assert(!mfc_repeat_limit_reached(&b, 0x10000, 0x3000, 16, MFC_GET_CMD));
    assert(!mfc_repeat_limit_reached(&a, 0x10000, 0x3000, 16, MFC_GET_CMD));
    assert(mfc_repeat_limit_reached(&a, 0x10000, 0x3000, 16, MFC_GET_CMD));
    assert(!mfc_repeat_limit_reached(&b, 0x10000, 0x3000, 16, MFC_GET_CMD));
    /* Direction, size, full 64-bit EA, and LS address distinguish commands. */
    assert(!mfc_repeat_limit_reached(&a, 0x10000, 0x3000, 16, MFC_PUT_CMD));
    assert(!mfc_repeat_limit_reached(&a, 0x10000, 0x3000, 32, MFC_PUT_CMD));
    assert(!mfc_repeat_limit_reached(&a, 0x100010000ull, 0x3000, 32, MFC_PUT_CMD));
    assert(!mfc_repeat_limit_reached(&a, 0x100010000ull, 0x3010, 32, MFC_PUT_CMD));
    puts("DMA repeat watchdog contract passed");
    return 0;
}
