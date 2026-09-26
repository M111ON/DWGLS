/* Frustum route -> adaptive tensor-memory reference adapter. */
#ifndef DWGLS_FRUSTUM_MEMORY_ADAPTER_H
#define DWGLS_FRUSTUM_MEMORY_ADAPTER_H

#include <stdint.h>
#include "adaptive_memory_bridge.h"
#include "frustum_route.h"

typedef int (*FrustumMemoryResolveFn)(const FrustumRouteEvent *route,
                                      AdaptiveMemoryRef *out,
                                      void *ctx);

static inline int frustum_route_to_memory_ref(const FrustumRouteEvent *route,
                                              AdaptiveMemoryRef *out)
{
    if (!route || !out || route->span_size == 0u) return -1;
    out->node_id = route->node_id;
    out->capo_key = route->capo_key;
    out->tensor_id = route->tensor_id;
    out->span_offset = route->span_offset;
    out->span_size = route->span_size;
    out->access_kind = 1u;
    out->generation = route->level;
    return 0;
}

static inline int frustum_route_to_memory_event(const FrustumRouteEvent *route,
                                                AdaptiveRouteEvent *out)
{
    if (!route || !out) return -1;
    if (frustum_route_to_memory_ref(route, &out->memory) != 0) return -1;
    out->level = route->level;
    out->branch_path = route->branch_path;
    out->view = route->view;
    return 0;
}

/* Resolve storage identity at the integration boundary, not in geometry. */
static inline int frustum_route_to_resolved_event(const FrustumRouteEvent *route,
                                                  FrustumMemoryResolveFn resolve,
                                                  void *ctx,
                                                  AdaptiveRouteEvent *out)
{
    if (!route || !resolve || !out) return -1;
    if (resolve(route, &out->memory, ctx) != 0) return -1;
    out->level = route->level;
    out->branch_path = route->branch_path;
    out->view = route->view;
    return out->memory.span_size ? 0 : -1;
}

#endif
