#ifndef DWGLS_GEO_WANG_LATCH_H
#define DWGLS_GEO_WANG_LATCH_H
/* B-latch: one-way flow latch over the wang6 branch space (144 cell x 72 choice).
 *
 * Spec (owner 2026-10-04):
 *   - latch id = cell*72 + choice, cell in [0,144), choice in [0,72) -> [0,10368)
 *   - never traversed = OPEN; traversed = SHUT (monotone within an epoch)
 *   - only CLEAR reopens; epoch reset = CLEAR of touched ids (or replay from empty log)
 *   - reserved ids 0 (all-open = major input) and 10367 (all-shut = major output)
 *     are excluded from the minor pool -> 10366 free branches per anchor
 *   - 10368 x 2 = 20736: half-field identity of the GJ family
 *
 * Persistence: P3 route log (core/geo_id_route_log.h), no new file format.
 *   Records are tagged WL_NAME_HASH so latch events can never collide with
 *   regular identity records that reuse the slot field.
 *   action WRITE (slot=latch_id) = traversed -> SHUT
 *   action CLEAR (slot=latch_id) = reopen that id
 * The in-memory bitset below is a DERIVED cache only — replay = recompute
 * (computation-over-storage: nothing stored that cannot be rederived).
 *
 * mm_wang.h / mm_wang6.h stay untouched: color = "can this path go" (stateless),
 * latch = "has flow ever passed here" (state above the valve, never inside it).
 */
#include <stdint.h>
#include <string.h>
#include "geo_id_route_log.h"

#define WL_COUNT       10368u
#define WL_RESERVED_LO 0u
#define WL_RESERVED_HI 10367u
#define WL_FREE        (WL_COUNT - 2u)   /* 10366 minor branches */
#define WL_WORDS       (WL_COUNT / 64u)  /* 162 */
#define WL_BYTES       (WL_WORDS * 8u)   /* 1296 B */

/* records tagged with this name_hash are latch events, not identity events */
#define WL_NAME_HASH   0x77616E676C617463ull  /* "wanglatc" */

typedef struct {
    uint64_t w[WL_WORDS];
} wl_latch_t;

static inline uint32_t wl_id(uint32_t cell, uint32_t choice) {
    return cell * 72u + choice;
}
static inline int wl_is_reserved(uint32_t id) {
    return id == WL_RESERVED_LO || id == WL_RESERVED_HI;
}
static inline void wl_reset(wl_latch_t *L) {
    memset(L, 0, sizeof *L);
}
static inline int wl_is_open(const wl_latch_t *L, uint32_t id) {
    if (id >= WL_COUNT) return 0;
    return (L->w[id >> 6] >> (id & 63u)) & 1u ? 0 : 1;
}
/* traverse: OPEN -> SHUT, monotone, idempotent. Reserved ids refused (major-only). */
static inline int wl_traverse(wl_latch_t *L, uint32_t id) {
    if (id >= WL_COUNT || wl_is_reserved(id)) return 0;
    L->w[id >> 6] |= 1ull << (id & 63u);
    return 1;
}
/* CLEAR: the only reopen. */
static inline int wl_clear(wl_latch_t *L, uint32_t id) {
    if (id >= WL_COUNT) return 0;
    L->w[id >> 6] &= ~(1ull << (id & 63u));
    return 1;
}
/* Apply one P3 record. Foreign name_hash / out-of-range slot = no-op. */
static inline void wl_apply_record(wl_latch_t *L, const IR_log_record_t *r) {
    if (r->name_hash != WL_NAME_HASH) return;
    if (r->action == IR_LOG_ACTION_WRITE)      wl_traverse(L, r->slot);
    else if (r->action == IR_LOG_ACTION_CLEAR) wl_clear(L, r->slot);
}
/* Replay = recompute: fresh latch + byte buffer -> state. */
static inline void wl_replay(wl_latch_t *L, const uint8_t *buf, size_t n_bytes) {
    wl_reset(L);
    if (!buf || (n_bytes % IR_LOG_RECORD_SIZE) != 0) return;
    for (size_t i = 0; i + IR_LOG_RECORD_SIZE <= n_bytes; i += IR_LOG_RECORD_SIZE) {
        IR_log_record_t r;
        ir_log_deserialize(buf + i, &r);
        wl_apply_record(L, &r);
    }
}

#endif /* DWGLS_GEO_WANG_LATCH_H */
