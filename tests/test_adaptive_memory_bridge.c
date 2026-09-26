#include <stdio.h>
#include <string.h>
#include "../core/adaptive_memory_bridge.h"

static int check(int ok, const char *name)
{
    if (!ok) printf("FAIL: %s\n", name);
    return ok;
}

int main(void)
{
    const AdaptiveRouteEvent source[] = {
        {0, 0, 0, {1728, 37, 4, 0x1200, 4096, 1, 9}},
        {1, 3, 0, {1728, 37, 4, 0x1200, 4096, 1, 9}},
        {2, 15, 1, {6912, 144, 12, 0x8800, 8192, 2, 10}},
    };
    AdaptiveRouteEvent replay[sizeof(source) / sizeof(source[0])];
    size_t count = sizeof(source) / sizeof(source[0]);
    int pass = 0;

    pass += check(adaptive_route_replay(source, count, replay, count) == 3,
                  "replay count");
    for (size_t i = 0; i < count; i++)
        pass += check(adaptive_route_event_equal(&source[i], &replay[i]),
                      "route reference replay");
    pass += check(source[0].memory.tensor_id == source[1].memory.tensor_id,
                  "multiple routes share one tensor record");
    pass += check(source[0].memory.span_offset == 0x1200,
                  "storage span remains explicit");

    printf("adaptive-memory-bridge: %d/%d PASS\n", pass, (int)(count + 3));
    return pass == (int)(count + 3) ? 0 : 1;
}
