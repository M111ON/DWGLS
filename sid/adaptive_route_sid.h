/*
 * adaptive_route_sid.h - connect an ephemeral route to the SID tensor store
 *
 * The route owns navigation metadata. SID owns the tensor record and bytes.
 * This adapter deliberately does not copy or transform tensor payloads.
 */
#ifndef ADAPTIVE_ROUTE_SID_H
#define ADAPTIVE_ROUTE_SID_H

#include <stdint.h>
#include <stddef.h>
#include "zone_card_sid.h" /* vendored: flattened from ../zone_card_sid.h */
#include "tensor_memory.h"

typedef struct {
    uint32_t level;
    uint32_t branch_path;
    uint16_t view;
    uint32_t node_id;
    uint16_t capo_key;
    uint8_t access_kind;
    uint32_t generation;
} AdaptiveRouteSIDEvent;

static inline ZoneCardSID adaptive_route_sid_make(
    const AdaptiveRouteSIDEvent *event,
    const ZoneCard *card,
    int64_t resid_x,
    int64_t resid_y,
    uint16_t tick,
    uint8_t flags)
{
    (void)event->level;
    (void)event->branch_path;
    (void)event->view;
    (void)event->access_kind;
    (void)event->generation;
    return zcsid_make(card, event->node_id, event->capo_key,
                      tick, flags, resid_x, resid_y);
}

static inline int adaptive_route_sid_append(
    TensorMemStore *store,
    const AdaptiveRouteSIDEvent *event,
    const ZoneCard *card,
    const char *name,
    const uint8_t *data,
    size_t data_size,
    int64_t resid_x,
    int64_t resid_y,
    uint16_t tick,
    uint8_t flags)
{
    if (!store || !event || !card || (!data && data_size)) return -1;
    ZoneCardSID sid = adaptive_route_sid_make(event, card, resid_x, resid_y,
                                              tick, flags);
    return tmem_append_raw(store, &sid, name, data, data_size);
}

#endif
