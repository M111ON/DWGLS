#include <stdio.h>
#include <string.h>
#include "../core/frustum_memory_adapter.h"

typedef struct {
    uint32_t position;
    uint8_t view;
    uint32_t calls;
} Catalog;

static int resolve(const FrustumRouteEvent *route,
                   AdaptiveMemoryRef *out, void *ctx)
{
    Catalog *c = (Catalog *)ctx;
    if (!out || !c || route->span_offset != c->position ||
        route->view != c->view) return -1;
    c->calls++;
    *out = (AdaptiveMemoryRef){
        .node_id = 1728u,
        .capo_key = 37u,
        .tensor_id = 19u,
        .span_offset = 4096u,
        .span_size = 16u,
        .access_kind = 1u,
        .generation = 2u
    };
    return 0;
}

int main(void)
{
    FrustumRouteEvent route = {0};
    AdaptiveRouteEvent event, replay;
    Catalog catalog = {.position = 512u, .view = 3u, .calls = 0u};
    route.level = 2u;
    route.branch_path = 4u;
    route.view = 3u;
    route.span_offset = catalog.position;
    int pass = 0;

    pass += frustum_route_to_resolved_event(&route, resolve, &catalog, &event) == 0;
    pass += event.memory.node_id == 1728u && event.memory.capo_key == 37u;
    pass += event.memory.tensor_id == 19u && event.memory.span_offset == 4096u;
    pass += adaptive_route_replay(&event, 1u, &replay, 1u) == 1;
    pass += adaptive_route_event_equal(&event, &replay);
    pass += catalog.calls == 1u;

    printf("frustum-memory-resolve: %d/6 PASS\n", pass);
    return pass == 6 ? 0 : 1;
}
