/*
 * adaptive_memory_bridge.h - route references for an exact tensor memory layer
 *
 * The route layer remembers how to reach a tensor. The tensor memory layer
 * remains responsible for bytes, lifecycle, and persistence.
 */
#ifndef DWGLS_ADAPTIVE_MEMORY_BRIDGE_H
#define DWGLS_ADAPTIVE_MEMORY_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t node_id;
    uint16_t capo_key;
    uint32_t tensor_id;
    uint64_t span_offset;
    uint32_t span_size;
    uint8_t access_kind;
    uint32_t generation;
} AdaptiveMemoryRef;

typedef struct {
    uint32_t level;
    uint32_t branch_path;
    uint16_t view;
    AdaptiveMemoryRef memory;
} AdaptiveRouteEvent;

static inline int adaptive_route_event_equal(const AdaptiveRouteEvent *a,
                                             const AdaptiveRouteEvent *b)
{
    return a && b && memcmp(a, b, sizeof(*a)) == 0;
}

/* Route replay must reproduce the same storage reference, not tensor bytes. */
static inline int adaptive_route_replay(const AdaptiveRouteEvent *events,
                                        size_t count,
                                        AdaptiveRouteEvent *out,
                                        size_t out_capacity)
{
    if ((!events && count) || (!out && count) || out_capacity < count) return -1;
    if (count) memcpy(out, events, count * sizeof(*events));
    return (int)count;
}

#endif
