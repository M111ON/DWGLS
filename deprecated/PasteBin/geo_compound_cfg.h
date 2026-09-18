/*
 * geo_compound_cfg.h — Unified Geometry Configuration (Y-Triangle)
 * ═══════════════════════════════════════════════════════════════════
 *
 * MIGRATED from compound-of-5-tetra/octa duality to geo_jump Y-triangle.
 * No more TETRA/OCTA enum. No more face_base/frustum_divisor hack.
 * Single address space: GEO_FULL = 20736 = 576 Y-triangles × 36 tick.
 *
 * DEPRECATED enums/types removed:
 *   GeoCompoundType       — TETRA/OCTA duality (eliminated)
 *   GeoFaceBase           — TRIANGLE/SQUARE face base (no longer needed)
 *   frustum_divisor       — tri→quad conversion hack (eliminated)
 *   geo_face_route()      — cross-compound routing (eliminated)
 *   geo_addr_translate()  — tetra↔octa translation (eliminated)
 *   geo_compound_cfg()    — type selector (eliminated)
 *   geo_compound_cfg_verify() — junction check (eliminated)
 *
 * All new code should use geo_jump.h directly:
 *   geo_pentagon_id(node)  → face 1..12
 *   geo_clock_tick(node)   → tick 0..1439
 *   geo_shell_level(node)  → layer 0..11
 *   JUMP_INVERT            → bipolar mirror
 *   geo_capo(node, key)    → offset routing
 *   geo_field_climate()    → zone classification
 *
 * Backward-compat GeoCompoundCfg struct kept for code that hasn't
 * migrated yet — uses fixed values derived from geo_jump.h.
 *
 * No malloc. No float.
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef GEO_COMPOUND_CFG_H
#define GEO_COMPOUND_CFG_H

#include <stdint.h>

/* ══════════════════════════════════════════════════════════════════
   DEPRECATED — kept for backward compat, will be removed.
   GeoCompoundType no longer exists. All geometry is unified.
   ══════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════
   SINGLE CONFIG STRUCT — fixed from geo_jump constants
   ══════════════════════════════════════════════════════════════════ */
typedef struct {
    /* geometry — single compound, no dual */
    uint32_t compounds;        /* 12 pentagons (dodecahedron)            */
    uint32_t faces_per_comp;   /* 12 (unified, no tetra/octa split)     */
    uint32_t face_slots;       /* GEO_FULL / 12 = 1728                  */
    uint32_t total_slots;      /* GEO_FULL = 20736                      */

    /* address space */
    uint32_t addr_base;        /* 0                                     */
    uint32_t addr_range;       /* GEO_FULL = 20736                      */
    uint32_t junction;         /* GEO_FULL = 20736 (no split needed)    */

    /* spoke / lane geometry */
    uint32_t spokes;           /* 6 spokes                              */
    uint32_t slots_per_spoke;  /* GEO_FULL / 6 = 3456                   */

    /* sacred number family: 2^a × 3^b */
    uint8_t  pow2;
    uint8_t  pow3;
} GeoCompoundCfg;

/* ── SINGLE FROZEN CONFIG ─────────────────────────────────────── */

static const GeoCompoundCfg GEO_CFG = {
    .compounds       = 12u,
    .faces_per_comp  = 12u,        /* unified: 12 pentagon faces          */
    .face_slots      = 1728u,      /* GEO_FULL / 12                       */
    .total_slots     = 20736u,     /* GEO_FULL                            */
    .addr_base       = 0u,
    .addr_range      = 20736u,     /* GEO_FULL                            */
    .junction        = 20736u,     /* no split — whole space is unified   */
    .spokes          = 6u,
    .slots_per_spoke = 3456u,      /* 20736 / 6 = 3456 (per-spoke stride) */
    .pow2            = 8u,
    .pow3            = 4u,         /* 20736 = 2^8 × 3^4                   */
};

/* ══════════════════════════════════════════════════════════════════
   BACKWARD-COMPAT ADDRESS FUNCTIONS
   All operate on GEO_FULL = 20736 space with the single config.
   ══════════════════════════════════════════════════════════════════ */

/* spoke index from absolute address */
static inline uint32_t geo_cfg_spoke(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    return local / c->slots_per_spoke;
}

/* face index (0..11) from absolute address */
static inline uint32_t geo_cfg_face(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    return local / c->face_slots;
}

/* local slot within face */
static inline uint32_t geo_cfg_slot_in_face(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    return local % c->face_slots;
}

/* bipolar split: first half / second half of face_slots */
static inline uint8_t geo_cfg_polarity(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    uint32_t pentagon = local % c->face_slots;
    return (uint8_t)((pentagon % (c->face_slots / 2u)) >= (c->face_slots / 4u));
}

/* hilbert bucket — total_slots / spokes */
static inline uint32_t geo_cfg_hilbert(const GeoCompoundCfg *c, uint64_t addr)
{
    uint32_t local = (uint32_t)((addr - c->addr_base) % c->total_slots);
    return local / c->spokes;
}

/* ══════════════════════════════════════════════════════════════════
   REMOVED (no longer applicable):
   - GeoCompoundType         → unified single space
   - GeoFaceBase             → Y-triangle has no tri↔quad conversion
   - frustum_divisor         → frustum works natively with Y-triangle
   - geo_face_route()        → use JUMP_PENTAGON in geo_jump.h
   - geo_addr_translate()    → use geo_capo() in geo_jump.h
   - geo_compound_cfg()      → use &GEO_CFG directly
   - geo_compound_cfg_verify() → no junction crossing to verify
   - geo_cfg_frustum_unit()  → frustum is per-Y-triangle, not per-face
   ══════════════════════════════════════════════════════════════════ */

#endif /* GEO_COMPOUND_CFG_H */
