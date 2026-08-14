/*
 * tw_rewind_bridge.h — SID node_id ↔ geo_rewind.h Bridge
 * ═══════════════════════════════════════════════════════════════
 *
 * Connects the SID Y-triangle system (20736-slot TWFaceRewind with node_id)
 * to the temporal rewind buffer (972-slot RewindBuffer with GEO_WALK enc).
 *
 *   SID world          → TWFaceRewind (20736 slots, 8-byte keys, node_id)
 *   TStream world      → RewindBuffer (972 slots, 4104B chunks, GEO_WALK enc)
 *
 * node_id → enc mapping:
 *   local = node_id % 1728  (position within pentagon, 1728 = GEO_FULL/12)
 *   walk_pos = local % 720  (hex walk cycle within pentagon)
 *   enc = GEO_WALK[walk_pos]
 *
 * Unified lookup flow:
 *   1. Look up TWFaceRewind by node_id (O(1), 20736 slots)
 *   2. Also check RewindBuffer by derived enc (O(1), 972 slots)
 *   3. Return combined result
 *
 * Include this AFTER both tw_face_bridge.h and core/core/geo_rewind.h.
 *
 * No malloc. No float. O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef TW_REWIND_BRIDGE_H
#define TW_REWIND_BRIDGE_H

#include <stdint.h>
#include <string.h>
#include "tw_face_bridge.h"
#include "core/core/geo_rewind.h"

/* ══════════════════════════════════════════════════════════════
   SID node_id ↔ packed enc conversion
   ══════════════════════════════════════════════════════════════ */

/* One pentagon = 1728 nodes, hex walk = 720 positions */
#define SID_PENTAGON_NODES  (GEO_FULL / GEO_PENTAGONS)  /* 1728 */

/*
 * Convert SID node_id (0..20735) to packed GEO_WALK enc value.
 *
 * local = node_id % 1728  (position within pentagon)
 * walk_pos = local % 720  (hex walk cycle)
 * enc = GEO_WALK[walk_pos]
 *
 * Returns packed enc for use with rewind_store/rewind_find in geo_rewind.h.
 * Returns 0xFFFFFFFF if the position is out of range or unmappable.
 */
static inline uint32_t tw_sid_node_to_enc(uint32_t node_id) {
    if (node_id >= GEO_FULL) return 0xFFFFFFFFu;

    uint16_t local = (uint16_t)(node_id % SID_PENTAGON_NODES);
    uint16_t walk_pos = local % 720u;

    return GEO_WALK[walk_pos];
}

/*
 * Convert packed GEO_WALK enc to SID walk position (0..719).
 *
 * Uses tring_pos() from geo_temporal_ring.h to get the walk position.
 * Returns the hex-only walk position (0..711).
 */
static inline uint16_t tw_enc_to_sid_walk(uint32_t enc) {
    return tring_pos(enc);
}

/*
 * Check whether a given enc has a valid mapping.
 * Returns 1 if valid, 0 if the enc doesn't correspond to any walk position.
 */
static inline int tw_enc_is_valid(uint32_t enc) {
    return (enc & 0x7FFu) < 2048u && GEO_WALK_IDX[enc & 0x7FFu] != 0xFFFFu;
}

/* ══════════════════════════════════════════════════════════════
   Unified store/find
   ══════════════════════════════════════════════════════════════ */

/*
 * Unified rewind buffer lookup result.
 *
 * Fields:
 *   tw_key    — packed key from TWFaceRewind (0 = not found)
 *   node_id   — node_id from TWFaceRewind (0 = not found)
 *   has_geo   — geo_rewind.h also had this tensor
 *   geo_chunk — TStreamChunk from geo_rewind.h (valid only if has_geo=1)
 */
typedef struct {
    uint64_t        tw_key;
    uint32_t        node_id;
    int             has_geo;
    TStreamChunk    geo_chunk;
} TWBRewindResult;

/*
 * Store a node_id + key in both TWFaceRewind and RewindBuffer.
 *
 * Always stored in TWFaceRewind (by node_id).
 * Also stored in RewindBuffer (by GEO_WALK enc) if enc is valid.
 *
 * Returns the enc used for geo_rewind.h (0xFFFFFFFF if skipped).
 */
static inline uint32_t tw_bridge_rewind_store(TWFaceRewind *tw_rb,
                                               RewindBuffer *geo_rb,
                                               uint64_t key,
                                               uint32_t node_id)
{
    if (!tw_rb || node_id >= GEO_FULL) return 0xFFFFFFFFu;

    tw_rewind_store(tw_rb, key, node_id);

    if (geo_rb) {
        uint32_t enc = tw_sid_node_to_enc(node_id);
        if (enc != 0xFFFFFFFFu) {
            TStreamChunk ch;
            memset(&ch, 0, sizeof(ch));
            memcpy(ch.data, &key, 8);
            ch.size = 8;
            rewind_store(geo_rb, enc, &ch);
            return enc;
        }
    }

    return 0xFFFFFFFFu;
}

/*
 * Unified lookup by SID node_id.
 *
 * First checks TWFaceRewind (fast O(1), 20736 slots).
 * Then checks RewindBuffer by derived enc (O(1), 972 slots).
 *
 * Returns a TWBRewindResult struct with combined results.
 */
static inline TWBRewindResult tw_bridge_rewind_find(TWFaceRewind *tw_rb,
                                                     RewindBuffer *geo_rb,
                                                     uint32_t node_id)
{
    TWBRewindResult r;
    memset(&r, 0, sizeof(r));

    r.tw_key = tw_rewind_find(tw_rb, node_id);
    if (r.tw_key) {
        r.node_id = node_id;

        if (geo_rb) {
            uint32_t enc = tw_sid_node_to_enc(node_id);
            if (enc != 0xFFFFFFFFu) {
                const TStreamChunk *p = rewind_find(geo_rb, enc);
                if (p) {
                    r.geo_chunk = *p;
                    r.has_geo = 1;
                }
            }
        }
    }

    return r;
}

/*
 * Check if a SID node_id is present in either store.
 * Returns 1 if found in TWFaceRewind OR RewindBuffer, 0 if neither.
 */
static inline int tw_bridge_rewind_has(TWFaceRewind *tw_rb,
                                        RewindBuffer *geo_rb,
                                        uint32_t node_id)
{
    if (tw_rewind_has(tw_rb, node_id)) return 1;

    if (geo_rb) {
        uint32_t enc = tw_sid_node_to_enc(node_id);
        if (enc != 0xFFFFFFFFu)
            return rewind_has(geo_rb, enc);
    }

    return 0;
}

/*
 * Evict a SID position from the RewindBuffer.
 * TWFaceRewind is write-once (key is overwritten on next store at same node_id).
 * Returns the enc that was evicted (0xFFFFFFFF if not found/skipped).
 */
static inline uint32_t tw_bridge_rewind_evict_geo(TWFaceRewind *tw_rb,
                                                    RewindBuffer *geo_rb,
                                                    uint32_t node_id)
{
    (void)tw_rb;
    if (!geo_rb || node_id >= GEO_FULL) return 0xFFFFFFFFu;

    uint32_t enc = tw_sid_node_to_enc(node_id);
    if (enc == 0xFFFFFFFFu) return 0xFFFFFFFFu;

    uint16_t slot = tring_pos(enc) % REWIND_SLOTS;
    if (geo_rb->slots[slot].valid && geo_rb->slots[slot].enc == enc) {
        geo_rb->slots[slot].valid = false;
        geo_rb->slots[slot].enc = 0;
    }

    return enc;
}

/* ══════════════════════════════════════════════════════════════
   Stats
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t tw_occupied;    /* valid slots in TWFaceRewind */
    uint32_t tw_stored;      /* total stores in TWFaceRewind */
    uint32_t geo_occupied;   /* valid slots in RewindBuffer */
    uint32_t geo_stored;     /* total chunks in RewindBuffer */
    uint32_t geo_pinned;     /* pinned slots in RewindBuffer */
} TWBridgeStats;

/*
 * Collect stats from both buffers.
 */
static inline void tw_bridge_stats(TWFaceRewind *tw_rb,
                                    RewindBuffer *geo_rb,
                                    TWBridgeStats *st)
{
    memset(st, 0, sizeof(*st));

    if (tw_rb) {
        st->tw_stored   = tw_rb->stored;
        st->tw_occupied = tw_rewind_occupied(tw_rb);
    }

    if (geo_rb) {
        RewindStats rs;
        rewind_stats(geo_rb, &rs);
        st->geo_occupied = rs.occupied;
        st->geo_stored   = rs.stored;
        st->geo_pinned   = rs.pinned;
    }
}

#endif /* TW_REWIND_BRIDGE_H */
