#include "../rsx_draw_batches.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    rsx_draw_batch* packets = NULL;
    uint32_t used = 0, capacity = 0;
    for (uint32_t i = 0; i < 5000; ++i)
        assert(rsx_draw_batch_append(&packets, &used, &capacity, i * 256, 256 - i % 7));
    assert(used == 5000 && capacity >= used);
    for (uint32_t i = 0; i < used; ++i) {
        assert(packets[i].first == i * 256);
        assert(packets[i].count == 256 - i % 7);
    }
    rsx_draw_batch* retained = packets;
    used = 0;
    assert(rsx_draw_batch_append(&packets, &used, &capacity, 17, 3));
    assert(packets == retained && used == 1 && packets[0].first == 17 && packets[0].count == 3);
    free(packets);
    puts("Draw packet growth: 5,000 ordered packets and reset reuse passed");
    return 0;
}
