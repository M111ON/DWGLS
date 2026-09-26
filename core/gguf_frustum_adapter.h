/* Direct GGUF catalog lookup for Frustum route consumers. */
#ifndef DWGLS_GGUF_FRUSTUM_ADAPTER_H
#define DWGLS_GGUF_FRUSTUM_ADAPTER_H

#include <stdint.h>
#include "gguf_box.h"
#include "frustum_route.h"

typedef struct {
    const char *name;
    const uint8_t *data;
    uint32_t size;
    uint32_t tensor_id;
    uint8_t dtype;
} GgufFrustumSpan;

static inline int gguf_frustum_resolve(const GGUFBox *box,
                                       const FrustumRouteEvent *route,
                                       GgufFrustumSpan *out)
{
    if (!box || !box->is_open || !route || !out) return -1;
    if (route->tensor_id >= box->n_tensors) return -2;

    const GGUFBoxEntry *entry = &box->entries[route->tensor_id];
    if (!entry->data || entry->size == 0u) return -3;
    if (route->span_offset > entry->size ||
        route->span_size > entry->size - route->span_offset) return -4;

    *out = (GgufFrustumSpan){
        .name = entry->name,
        .data = entry->data + route->span_offset,
        .size = route->span_size,
        .tensor_id = entry->idx,
        .dtype = entry->dtype
    };
    return 0;
}

#endif
