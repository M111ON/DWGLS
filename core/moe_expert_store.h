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

/* Store expert metadata at geometry address (per wtype slot) */
static inline int moe_store_meta(DtSlotRegion *r, uint32_t layer,
                                  uint32_t expert, uint32_t wtype,
                                  const MoeExpertMeta *meta) {
    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
    return dt_slot_put(r, flat, (const uint8_t*)meta, MOE_META_SZ) ? 0 : -1;
}

/* Load expert metadata from geometry address (per wtype slot) */
static inline int moe_load_meta(DtSlotRegion *r, uint32_t layer,
                                 uint32_t expert, uint32_t wtype,
                                 MoeExpertMeta *meta) {
    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
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

/* ═══════════════ OFFSET MODE ═══════════════
 * For large weights that don't fit in a single slot.
 * Each wtype slot stores its own MoeExpertMeta {offset, size, quant_type}
 * pointing to actual weight data in the backing file's weight pool.
 * Caller provides backing_fd (open file descriptor to weights file). */

static inline int moe_store_weight_offset(DtSlotRegion *r, uint32_t layer,
    uint32_t expert, uint32_t wtype, const void *data, size_t sz,
    uint64_t file_offset, int backing_fd) {
#ifdef _WIN32
    HANDLE h = (HANDLE)(intptr_t)backing_fd;
    DWORD written = 0;
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)file_offset;
    ov.OffsetHigh = (DWORD)(file_offset >> 32);
    if (!WriteFile(h, data, (DWORD)sz, &written, &ov) || written != sz) return -1;
#else
    ssize_t w = pwrite(backing_fd, data, sz, (off_t)file_offset);
    if (w != (ssize_t)sz) return -1;
#endif
    MoeExpertMeta meta = {0};
    meta.offset = (uint32_t)file_offset;
    meta.size   = (uint32_t)sz;
    /* store metadata at this wtype's own flat slot, not gate slot */
    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
    return dt_slot_put(r, flat, (const uint8_t*)&meta, MOE_META_SZ) ? 0 : -1;
}

static inline int moe_load_weight_offset(DtSlotRegion *r, uint32_t layer,
    uint32_t expert, uint32_t wtype, void *dst, size_t cap, int backing_fd) {
    uint32_t flat = moe_expert_to_flat(layer, expert, wtype);
    MoeExpertMeta meta;
    if (dt_slot_get(r, flat, (uint8_t*)&meta, MOE_META_SZ) != 0) return -1;
    if (meta.size > cap) return -2;
#ifdef _WIN32
    HANDLE h = (HANDLE)(intptr_t)backing_fd;
    DWORD read = 0;
    OVERLAPPED ov = {0};
    ov.Offset = meta.offset;
    ov.OffsetHigh = (DWORD)((uint64_t)meta.offset >> 32);
    if (!ReadFile(h, dst, meta.size, &read, &ov) || read != meta.size) return -3;
#else
    ssize_t r2 = pread(backing_fd, dst, meta.size, (off_t)meta.offset);
    if (r2 != (ssize_t)meta.size) return -3;
#endif
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
