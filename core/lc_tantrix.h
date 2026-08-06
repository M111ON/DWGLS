/*
 * lc_tantrix.h — Tantrix 256 Routing Layer
 *
 * 1 byte = 1 tile = 1 routing instruction
 * 256 states: 252 normal + 4 special (NULL/CROSS/MERGE/SPLIT)
 */

#ifndef LC_TANTRIX_H
#define LC_TANTRIX_H

#include <stdbool.h>
#include <stdint.h>

/* ── tile classes ───────────────────────────────────────────── */
typedef enum {
    TANTRIX_CLASS_NORMAL  = 0,
    TANTRIX_CLASS_SKIP    = 1,
    TANTRIX_CLASS_MIRROR  = 2,
    TANTRIX_CLASS_SPECIAL = 3,
} TantrixClass;

/* ── special tiles ──────────────────────────────────────────── */
#define TANTRIX_NULL   0x00u
#define TANTRIX_CROSS  0xAAu
#define TANTRIX_MERGE  0x55u
#define TANTRIX_SPLIT  0xFFu

typedef uint8_t TantrixTile;

/* ── decode ─────────────────────────────────────────────────── */
static inline uint8_t tantrix_entry(TantrixTile t)  { return  t       & 0x3u; }
static inline uint8_t tantrix_exit(TantrixTile t)   { return (t >> 2) & 0x3u; }
static inline uint8_t tantrix_spoke(TantrixTile t)  { return (t >> 4) & 0x3u; }
static inline uint8_t tantrix_class(TantrixTile t)  { return (t >> 6) & 0x3u; }

/* ── encode ─────────────────────────────────────────────────── */
static inline TantrixTile tantrix_make(uint8_t entry, uint8_t exit,
                                        uint8_t spoke, TantrixClass cls) {
    return (TantrixTile)((entry & 0x3u)
                       | ((exit  & 0x3u) << 2)
                       | ((spoke & 0x3u) << 4)
                       | ((cls   & 0x3u) << 6));
}

/* ── special checks ─────────────────────────────────────────── */
static inline bool tantrix_is_null(TantrixTile t)    { return t == TANTRIX_NULL; }
static inline bool tantrix_is_cross(TantrixTile t)   { return t == TANTRIX_CROSS; }
static inline bool tantrix_is_merge(TantrixTile t)   { return t == TANTRIX_MERGE; }
static inline bool tantrix_is_split(TantrixTile t)   { return t == TANTRIX_SPLIT; }
static inline bool tantrix_is_special(TantrixTile t) {
    return t == TANTRIX_NULL || t == TANTRIX_CROSS
        || t == TANTRIX_MERGE || t == TANTRIX_SPLIT;
}

/* ── Wang edge compatibility ────────────────────────────────── */
static inline bool tantrix_connects(TantrixTile left, TantrixTile right) {
    if (tantrix_is_null(left) || tantrix_is_null(right)) return false;
    if (tantrix_is_split(left) || tantrix_is_split(right)) return true;
    return tantrix_exit(left) == tantrix_entry(right);
}

/* ── gate routing ───────────────────────────────────────────── */
typedef enum {
    TANTRIX_ROUTE_FORWARD,
    TANTRIX_ROUTE_DROP,
    TANTRIX_ROUTE_INVERT,
    TANTRIX_ROUTE_BROADCAST,
    TANTRIX_ROUTE_MERGE,
} TantrixRouteResult;

static inline TantrixRouteResult tantrix_route(TantrixTile t,
                                                uint8_t  incoming_gate,
                                                uint8_t *out_gate) {
    if (tantrix_is_null(t))  { *out_gate = 0; return TANTRIX_ROUTE_DROP; }
    if (tantrix_is_split(t)) { *out_gate = incoming_gate; return TANTRIX_ROUTE_BROADCAST; }
    if (tantrix_is_merge(t)) { *out_gate = 0; return TANTRIX_ROUTE_MERGE; }
    if (tantrix_is_cross(t)) {
        static const uint8_t cross_map[4] = {1, 0, 3, 2};
        *out_gate = cross_map[incoming_gate & 3u];
        return TANTRIX_ROUTE_FORWARD;
    }

    if (tantrix_entry(t) != incoming_gate) {
        *out_gate = 0;
        return TANTRIX_ROUTE_DROP;
    }

    uint8_t exit = tantrix_exit(t);
    if (tantrix_class(t) == TANTRIX_CLASS_SKIP)
        exit ^= 0x3u;
    if (tantrix_class(t) == TANTRIX_CLASS_MIRROR)
        exit = (uint8_t)(((exit & 1u) << 1) | ((exit >> 1) & 1u));

    *out_gate = exit;
    return TANTRIX_ROUTE_FORWARD;
}

/* ── spoke mask ─────────────────────────────────────────────── */
static inline uint8_t tantrix_active_spokes(TantrixTile t) {
    static const uint8_t spoke_mask[4] = {0x09u, 0x12u, 0x24u, 0x3Fu};
    return spoke_mask[tantrix_spoke(t)];
}

/* ── verify ─────────────────────────────────────────────────── */
static inline int lc_tantrix_verify(void) {
    for (uint16_t i = 1u; i < 253u; i++) {
        TantrixTile t = (TantrixTile)i;
        TantrixTile r = tantrix_make(tantrix_entry(t), tantrix_exit(t),
                                     tantrix_spoke(t),
                                     (TantrixClass)tantrix_class(t));
        if (r != t) return -1;
    }
    if (!tantrix_is_null(TANTRIX_NULL))   return -2;
    if (!tantrix_is_cross(TANTRIX_CROSS)) return -3;
    if (!tantrix_is_merge(TANTRIX_MERGE)) return -4;
    if (!tantrix_is_split(TANTRIX_SPLIT)) return -5;
    return 0;
}

#endif /* LC_TANTRIX_H */
