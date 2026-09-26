#include <stdio.h>
#include "../core/frustum_memory_adapter.h"

int main(void)
{
    FrRouteCache cache;
    FrustumSeeker seeker;
    FrustumRouteEvent route;
    AdaptiveRouteEvent event;
    AdaptiveRouteEvent replay;
    int pass = 0;

    fr_cache_init(&cache);
    seeker.position = 1000u;
    seeker.view_id = 2u;
    seeker.voronoi_mask = 1u << vm_cell_of(seeker.position);
    seeker.frustum_depth = 2u;

    if (!fr_route_produce(&seeker, &cache, &route)) return 1;
    if (frustum_route_to_memory_event(&route, &event) != 0) return 1;
    if (event.memory.node_id != (uint32_t)route.node_id) return 1;
    if (event.memory.tensor_id != (uint32_t)route.tensor_id) return 1;
    if (event.memory.span_offset != route.span_offset ||
        event.memory.span_size != route.span_size) return 1;
    pass += event.memory.access_kind == 1u;
    pass += event.memory.generation == route.level;
    if (adaptive_route_replay(&event, 1u, &replay, 1u) != 1) return 1;
    pass += adaptive_route_event_equal(&event, &replay);

    printf("frustum-memory-adapter: %d/3 PASS\n", pass);
    return pass == 3 ? 0 : 1;
}
