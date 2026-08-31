/* moe_expert_store.h — MoE Expert Weight Storage on DtSlotRegion
 *
 * Wraps DtSlotRegion with MoE-specific operations:
 *   store: expert_id → geometry addr → DtSlotRegion write
 *   load:  expert_id → geometry addr → DtSlotRegion read
 *   metadata: {offset, size, quant_type} per expert
 *
 * Two modes:
 *   INLINE: small weights stored directly in slot (demo/testing)
 *   OFFSET: slot stores {offset, size} → actual weights on disk (production)
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef MOE_EXPERT_STORE_H
#define MOE_EXPERT_STORE_H

#include "moe_expert_addr.h"

/* DtSlotRegion — caller must include dramtile_store.h before this header */

/* ═══════════════ METADATA ═══════════════ */

typedef struct {
    uint32_t offset;     /* byte offset in backing file (0 = inline) */
    uint32_t size;       /* weight data size in bytes */
    uint8_t  quant_type; /* 0=f32, 1=f16, 2=q8_0, 3=q4_0 */
    uint8_t  flags;      /* reserved */
    uint16_t pad;
} MoeExpertMeta;

#define MOE_META_SZ  sizeof(MoeExpertMeta)

/* ═══════════════ STORE OPERATIONS ═══════════════ */

/* Store expert metadata at geometry address */
static inline int moe_store_meta(DtSlotRegion *r, uint32_t layer,
                                  uint32_t expert, const MoeExpertMeta *meta) {
    uint32_t flat = moe_expert_to_flat(layer, expert, 0); /* gate slot */
    return dt_slot_put(r, flat, (const uint8_t*)meta, MOE_META_SZ) ? 0 : -1;
}

/* Load expert metadata from geometry address */
static inline int moe_load_meta(DtSlotRegion *r, uint32_t layer,
                                 uint32_t expert, MoeExpertMeta *meta) {
    uint32_t flat = moe_expert_to_flat(layer, expert, 0);
    return dt_slot_get(r, flat, (uint8_t*)meta, MOE_META_SZ);
}

/* Store weight data at geometry address (inline mode) */
static inline int moe_store_weight(DtSlotRegion *r, uint32_t layer,
                                    uint32_t expert, uint32_t wtype,
                                    const void *data, size_t sz) {
    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
    return dt_slot_put(r, flat, (const uint8_t*)data, sz) ? 0 : -1;
}

/* Load weight data from geometry address (inline mode) */
static inline int moe_load_weight(DtSlotRegion *r, uint32_t layer,
                                   uint32_t expert, uint32_t wtype,
                                   void *dst, size_t sz) {
    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
    return dt_slot_get(r, flat, (uint8_t*)dst, sz);
}

/* ═══════════════ BATCH OPERATIONS ═══════════════ */

/* Store all 3 weight types for one expert */
static inline int moe_store_expert(DtSlotRegion *r, uint32_t layer,
                                    uint32_t expert,
                                    const void *gate, const void *up,
                                    const void *down, size_t w_sz) {
    if (moe_store_weight(r, layer, expert, MOE_WTYPE_GATE, gate, w_sz) != 0)
        return -1;
    if (moe_store_weight(r, layer, expert, MOE_WTYPE_UP, up, w_sz) != 0)
        return -1;
    if (moe_store_weight(r, layer, expert, MOE_WTYPE_DOWN, down, w_sz) != 0)
        return -1;
    return 0;
}

/* Load all 3 weight types for one expert */
static inline int moe_load_expert(DtSlotRegion *r, uint32_t layer,
                                   uint32_t expert,
                                   void *gate, void *up, void *down,
                                   size_t w_sz) {
    if (moe_load_weight(r, layer, expert, MOE_WTYPE_GATE, gate, w_sz) != 0)
        return -1;
    if (moe_load_weight(r, layer, expert, MOE_WTYPE_UP, up, w_sz) != 0)
        return -1;
    if (moe_load_weight(r, layer, expert, MOE_WTYPE_DOWN, down, w_sz) != 0)
        return -1;
    return 0;
}

/* ═══════════════ GEOMETRY ACCESS ═══════════════ */

/* Store by geometry coordinate directly */
static inline int moe_store_geom(DtSlotRegion *r, uint32_t tess,
                                  uint32_t cube, uint32_t slot,
                                  const void *data, size_t sz) {
    uint32_t flat = tess_to_flat(tess, cube, slot);
    return dt_slot_put(r, flat, (const uint8_t*)data, sz) ? 0 : -1;
}

/* Load by geometry coordinate directly */
static inline int moe_load_geom(DtSlotRegion *r, uint32_t tess,
                                 uint32_t cube, uint32_t slot,
                                 void *dst, size_t sz) {
    uint32_t flat = tess_to_flat(tess, cube, slot);
    return dt_slot_get(r, flat, (uint8_t*)dst, sz);
}

#endif /* MOE_EXPERT_STORE_H */
