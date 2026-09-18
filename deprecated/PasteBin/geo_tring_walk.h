/*
 * geo_tring_walk.h — TRing walk: tile index → TRing enc position
 * ═══════════════════════════════════════════════════════════════
 * Maps tile index i (0..NT-1) → enc (0..719) for spoke routing.
 *
 * Design:
 *   enc(i) = (i * TRING_WALK_STRIDE) % TRING_WALK_CYCLE
 *
 *   STRIDE = 37 (prime, coprime to 720)
 *   → uniform spoke distribution for any NT
 *   → max spoke imbalance ≤ ceil(NT/6) - floor(NT/6) tiles
 *
 * Usage:
 *   uint16_t enc = tring_walk_enc(i);          // enc 0..719
 *   uint8_t  spk = tring_walk_spoke(i);        // spoke 0..5
 *   uint8_t  pol = tring_walk_polarity(i);     // 0=ROUTE 1=GROUND
 *
 * SEAM 3 patch pattern:
 *   // BEFORE (raster):
 *   int tx = i % TW, ty = i / TW;
 *
 *   // AFTER (TRing walk — pixel coords unchanged, enc for dispatch):
 *   int tx = i % TW, ty = i / TW;              // pixel coords: raster
 *   uint16_t enc = tring_walk_enc(i);           // dispatch enc: walk
 *
 * Sacred constants (frozen):
 *   TRING_WALK_CYCLE  = 720   (6 spokes × 120)
 *   TRING_WALK_STRIDE = 37    (prime, gcd(37,720)=1)
 *   TRING_WALK_SPOKE_SZ = 120
 *
 * No malloc. No heap. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════
 */

#ifndef GEO_TRING_WALK_H
#define GEO_TRING_WALK_H

#include <stdint.h>

#define TRING_WALK_CYCLE    720u
#define TRING_WALK_STRIDE    37u   /* prime, coprime to 720 */
#define TRING_WALK_SPOKE_SZ 120u   /* positions per spoke   */
#define TRING_WALK_SPOKES     6u

/* ── enc: tile i → TRing position 0..719 ─────────────────────── */
static inline uint16_t tring_walk_enc(uint32_t i)
{
    return (uint16_t)((i * TRING_WALK_STRIDE) % TRING_WALK_CYCLE);
}

/* ── spoke: 0..5 ─────────────────────────────────────────────── */
static inline uint8_t tring_walk_spoke(uint32_t i)
{
    return (uint8_t)(tring_walk_enc(i) / TRING_WALK_SPOKE_SZ);
}

/* ── polarity: 0=ROUTE 1=GROUND  (enc%120 >= 60) ─────────────── */
static inline uint8_t tring_walk_polarity(uint32_t i)
{
    return (uint8_t)((tring_walk_enc(i) % 120u) >= 60u);
}

/* ── spoke histogram verify (debug) ─────────────────────────────
 * Returns max imbalance across 6 spokes for NT tiles.
 * Expect ≤ ceil(NT/6) - floor(NT/6) for any NT.
 */
static inline uint32_t tring_walk_spoke_imbalance(uint32_t NT)
{
    uint32_t hist[TRING_WALK_SPOKES] = {0};
    for (uint32_t i = 0u; i < NT; i++)
        hist[tring_walk_spoke(i)]++;
    uint32_t mn = hist[0], mx = hist[0];
    for (uint32_t s = 1u; s < TRING_WALK_SPOKES; s++) {
        if (hist[s] < mn) mn = hist[s];
        if (hist[s] > mx) mx = hist[s];
    }
    return mx - mn;
}

#endif /* GEO_TRING_WALK_H */
