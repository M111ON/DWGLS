/*
 * rdh_288_unified.h — Unified API: old 1440-cycle + new 288-cell pipeline
 * ════════════════════════════════════════════════════════════════════════
 *
 * Provides BOTH pipelines in one header:
 *   - OLD: flat_key % 1440 → frame_at → (face, slot, phase, ico_idx)
 *   - NEW: bridge_288 → (face, direction, cell_pos) → frame_1728_at
 *
 * Usage:
 *   #include "rdh_288_unified.h"
 *
 *   // Old pipeline (lossy, 1440-cycle)
 *   uint16_t enc = rdh_capture_to_enc(data, len, &cfg);
 *   DualFrame f = frame_at(enc);
 *
 *   // New pipeline (lossless, 288-cell)
 *   Cell288Addr ca = rdh_capture_to_288(data, len, &cfg);
 *   Frame1728 fr = rdh_capture_to_frame1728(data, len, &cfg);
 *
 * No malloc. No float. O(1). Backward compatible.
 * ════════════════════════════════════════════════════════════════════════
 */

#ifndef RDH_288_UNIFIED_H
#define RDH_288_UNIFIED_H

#include "rdh_capture.h"
#include "rdh_288_bridge.h"
#include "collection/dgls/geo/include/geo_frame_seek.h"
#include "collection/dgls/geo/include/geo_frame_seek_1728.h"

/* ══════════════════════════════════════════════════════════════
   NEW API: rdh_capture → 288-cell (LOSSLESS)
   ══════════════════════════════════════════════════════════════ */

/* Capture data → flat_key → 288-cell address (LOSSLESS) */
static inline Cell288Addr rdh_capture_to_288(const uint8_t *data, size_t len,
                                              const RDHConfig *cfg)
{
    int64_t key = rdh_capture(data, len, cfg);
    return bridge_288(key);
}

/* Capture data → flat_key → 288-cell → Frame1728 (LOSSLESS) */
static inline Frame1728 rdh_capture_to_frame1728(const uint8_t *data, size_t len,
                                                   const RDHConfig *cfg)
{
    int64_t key = rdh_capture(data, len, cfg);
    return frame_1728_from_key(key);
}

/* Capture data → flat_key (full 20736 range, no truncation) */
static inline int64_t rdh_capture_to_key(const uint8_t *data, size_t len,
                                          const RDHConfig *cfg)
{
    return rdh_capture(data, len, cfg);
}

/* ══════════════════════════════════════════════════════════════
   OLD API: rdh_capture → enc (LOSSY, kept for backward compat)
   ══════════════════════════════════════════════════════════════ */

/* These are already defined in rdh_capture.h and geo_frame_seek.h */
/* rdh_capture_to_enc() — flat_key % 1440 */
/* frame_at(enc) — decompose 1440-cycle enc */

/* ══════════════════════════════════════════════════════════════
   COMPARISON: old vs new for same data
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t enc_1440;      /* old: flat_key % 1440 (LOSSY) */
    DualFrame frame_old;    /* old: frame_at(enc_1440) */
    Cell288Addr cell_288;   /* new: bridge_288 (LOSSLESS) */
    Frame1728 frame_new;    /* new: frame_1728_from_key */
    int64_t flat_key;       /* full key (20736 range) */
} PipelineComparison;

static inline PipelineComparison rdh_compare(const uint8_t *data, size_t len,
                                             const RDHConfig *cfg)
{
    PipelineComparison c;
    c.flat_key   = rdh_capture(data, len, cfg);
    c.enc_1440   = (uint16_t)((uint64_t)c.flat_key % 1440);
    c.frame_old  = frame_at(c.enc_1440);
    c.cell_288   = bridge_288(c.flat_key);
    c.frame_new  = frame_1728_from_key(c.flat_key);
    return c;
}

#endif /* RDH_288_UNIFIED_H */
