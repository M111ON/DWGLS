/*
 * gnn_fan24_features.h — Fan24 Context Features for GNN Tile Connectivity
 * ═══════════════════════════════════════════════════════════════════════════
 * Bridges fan24_gear.h (ring-24 CRT bijection) into GNN node features.
 *
 * Each tile at position pos ∈ [0,20736) gets 6 Fan24 features:
 *   [0] gear_sin: sin(2π·gear/24) — circular ring-24 position
 *   [1] gear_cos: cos(2π·gear/24) — circular ring-24 position
 *   [2] crt_kis:  (pos % 24) % 8 / 7 — KIS cube wheel [0,7]
 *   [3] crt_hyp:  (pos % 24) % 3 / 2 — hyperbolic axis wheel [0,2]
 *   [4] lang_id:  pos % 9 / 8 — 9-language identity [0,8]
 *   [5] dist_center: |pos - 10368| / 10368 — distance from inner sanctuary
 *
 * These features encode:
 *   - Ring-24 gear position (sin/cos for circular topology)
 *   - CRT decomposition (8×3 split = KIS × hyperbolic)
 *   - 9-language self-repair network identity
 *   - Proximity to inner sanctuary (center of 20736 field)
 *
 * BUILD: included by gnn_fan24_model.h (auto-generated) or standalone.
 */
#ifndef GNN_FAN24_FEATURES_H
#define GNN_FAN24_FEATURES_H

#include <stdint.h>
#include <math.h>

/* ── Fan24 constants (must match fan24_gear.h) ──────────────── */
#define GNN_F24_RING      24u
#define GNN_F24_WHEEL_KIS  8u
#define GNN_F24_WHEEL_HYP  3u
#define GNN_F24_FULL    20736u
#define GNN_F24_LOCAL     144u
#define GNN_F24_NLANG      9u

/* ── Compute 6 Fan24 features from tile index ─────────────────
 * tile_idx: position in [0,20736) address space
 * f24_out[6]: [gear_sin, gear_cos, crt_kis, crt_hyp, lang_id, dist_center]
 *
 * All computations are O(1) — no loops, no lookup tables.
 * sin/cos use hardware FPU (fast on modern CPUs, negligible cost).
 */
static inline void gnn_f24_compute_features(
    uint32_t tile_idx, float f24_out[6]
) {
    uint32_t pos  = tile_idx % GNN_F24_FULL;
    uint32_t gear = pos % GNN_F24_RING;
    uint32_t dc   = gear % GNN_F24_WHEEL_KIS;    /* KIS cube wheel [0,7] */
    uint32_t dx   = gear % GNN_F24_WHEEL_HYP;    /* hyper axis wheel [0,2] */
    uint32_t lang = pos % GNN_F24_NLANG;          /* 9-language identity */

    float angle = 6.283185307f * (float)gear / (float)GNN_F24_RING;
    f24_out[0] = sinf(angle);                      /* gear_sin */
    f24_out[1] = cosf(angle);                      /* gear_cos */
    f24_out[2] = (float)dc / 7.0f;                /* crt_kis normalized */
    f24_out[3] = (float)dx / 2.0f;                /* crt_hyp normalized */
    f24_out[4] = (float)lang / 8.0f;              /* lang_id normalized */

    /* Distance from inner sanctuary (center of 20736 field) */
    float center = (float)(GNN_F24_FULL / 2u);
    float d = fabsf((float)pos - center) / center;
    f24_out[5] = d;                                /* dist_center */
}

/* ── CRT inverse (for reconstruction) ─────────────────────────
 * Given (dc mod 8, dx mod 3), reconstruct the ring-24 tooth.
 * Same formula as fg_crt in fan24_gear.h — included here for
 * self-contained GNN feature computation without fan24_gear.h. */
static inline uint8_t gnn_f24_crt(uint8_t dc, uint8_t dx) {
    uint8_t k = (uint8_t)(((dx + 3u - (dc % 3u)) % 3u) * 2u % 3u);
    return (uint8_t)(dc + 8u * k);
}

/* ── Wheel position queries ──────────────────────────────────── */
static inline uint32_t gnn_f24_wheel_kis(uint32_t pos) {
    return (pos % GNN_F24_FULL) % GNN_F24_RING % GNN_F24_WHEEL_KIS;
}
static inline uint32_t gnn_f24_wheel_hyp(uint32_t pos) {
    return (pos % GNN_F24_FULL) % GNN_F24_RING % GNN_F24_WHEEL_HYP;
}
static inline uint32_t gnn_f24_language(uint32_t pos) {
    return (pos % GNN_F24_FULL) % GNN_F24_NLANG;
}

#endif /* GNN_FAN24_FEATURES_H */
