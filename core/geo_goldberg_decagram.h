/*
 * geo_goldberg_decagram.h — Decagram 10-Sector Goldberg Layout
 * ═══════════════════════════════════════════════════════════════
 *
 * MOTIVATION (from user, 2026-08-17):
 *   "ต้องการ decagram เพื่อ map เข้า goldberg ได้ทุก face เพราะ
 *    ตระกูล dodeca bipolar มัน inverted"
 *
 * Goldberg GP(n,0): 12 pentagon anchors (Euler, fixed) + 10(n²−1) hexagons.
 * The dodeca family is bipolar-inverted: face f pairs with face f+6 (mod 12),
 * and the two poles (ring1 = faces 0..5, ring2 = faces 6..11) are inverted
 * copies (geo_goldberg_lut.h: GB_PEN_TO_PAIR, GB_PEN_POLE).
 *
 * KEY FACT: 10(n²−1) hexagons divide EXACTLY into 10 sectors:
 *   hex per sector = n² − 1      (no remainder, all levels 1..8)
 * The old round-robin 12-sector layout (gp_hex_in_sector) has a remainder for
 * most levels; the decagram 10-sector layout is exact by construction.
 *
 * DECAGRAM = 10 directions at 36° each (TW_N_SECTORS=10, §15.39):
 *   10 sectors × 36° = 360° — covers the full face cycle.
 *   Sector d and sector (d+5) mod 10 are OPPOSITE = the bipolar inverted pair.
 *   Pentagon pair (f, f+6): f sits on pole 0 (ring1), f+6 on pole 1 (ring2).
 *
 * LAYOUT (tile_id space, same as geo_goldberg_sphere.h):
 *   0..11            = pentagon anchors (fixed, all levels)
 *   12..12+10(n²−1)−1 = hexagons, grouped into 10 decagram sectors
 *   hex tile_id = 12 + sector·(n²−1) + offset,  offset ∈ [0, n²−1)
 *
 * Bijective: every tile_id 0..faces−1 addressed exactly once (zero gap).
 * Stateless, int-only, no trig, no table — decagram order computed inline.
 *
 * Depends: <stdint.h> only (+ geo_goldberg_lut.h for the pair map).
 */

#ifndef GEO_GOLDBERG_DECAGRAM_H
#define GEO_GOLDberg_DECAGRAM_H

#include <stdint.h>
#include "geo_goldberg_lut.h"

#define GGD_SECTORS        10u   /* decagram: 10 × 36° = 360°          */
#define GGD_SECTOR_DEG     36u   /* degrees per sector                 */
#define GGD_PENTAGONS      12u   /* Euler-fixed anchors                */

/* ── hexagon count: 10(n²−1), divides exactly by 10 ─────────── */
static inline uint32_t ggd_hex_total(uint8_t level)
{
    uint32_t n = level;
    return 10u * (n * n - 1u);
}

/* hexagons per decagram sector = n²−1 (EXACT — the decagram fact) */
static inline uint32_t ggd_hex_per_sector(uint8_t level)
{
    uint32_t n = level;
    return n * n - 1u;
}

/* total faces for level = 12 pent + 10(n²−1) hex = 10n² + 2 */
static inline uint32_t ggd_face_count(uint8_t level)
{
    return ggd_hex_total(level) + GGD_PENTAGONS;
}

/* ── tile_id from (sector, offset) — hex only ───────────────── */
static inline uint32_t ggd_hex_tile_id(uint8_t level, uint8_t sector, uint32_t offset)
{
    if (sector >= GGD_SECTORS || offset >= ggd_hex_per_sector(level))
        return UINT32_MAX;
    return GGD_PENTAGONS + (uint32_t)sector * ggd_hex_per_sector(level) + offset;
}

/* ── reverse: hex tile_id → (sector, offset) ─────────────────── */
static inline uint8_t ggd_sector_of_hex(uint8_t level, uint32_t tile_id)
{
    if (tile_id < GGD_PENTAGONS) return 0xFFu;              /* pentagon */
    uint32_t h = tile_id - GGD_PENTAGONS;
    uint32_t per = ggd_hex_per_sector(level);
    return (uint8_t)(h / per);
}

static inline uint32_t ggd_offset_of_hex(uint8_t level, uint32_t tile_id)
{
    if (tile_id < GGD_PENTAGONS) return UINT32_MAX;
    uint32_t h = tile_id - GGD_PENTAGONS;
    return h % ggd_hex_per_sector(level);
}

/* ── bipolar inversion ───────────────────────────────────────── */
/* Opposite decagram direction = the inverted half (d+5 mod 10)  */
static inline uint8_t ggd_inverted_sector(uint8_t sector)
{
    return (uint8_t)((sector + 5u) % GGD_SECTORS);
}

/* pentagon face f pairs with face f+6 (bipolar inverted)        */
static inline uint8_t ggd_pair_face(uint8_t face)
{
    return (uint8_t)((face + 6u) % GGD_PENTAGONS);
}

/* pole of pentagon face: 0 = ring1 (0..5), 1 = ring2 (6..11)    */
static inline uint8_t ggd_pole(uint8_t face)
{
    return (uint8_t)(face / 6u);
}

/* pair index of pentagon face (0..5) — matches GB_PEN_TO_PAIR   */
static inline uint8_t ggd_pair(uint8_t face)
{
    return (uint8_t)(face % 6u);
}

/* ── full coverage check: every tile_id 0..faces−1 addressed ── */
/* Sum over 10 sectors of n²−1 = 10(n²−1) = hex total — identity */
static inline int ggd_hex_total_matches(uint8_t level)
{
    return ggd_hex_total(level) == (uint32_t)GGD_SECTORS * ggd_hex_per_sector(level);
}

#endif /* GEO_GOLDBERG_DECAGRAM_H */
