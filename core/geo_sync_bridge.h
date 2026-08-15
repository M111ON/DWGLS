/*
 * geo_sync_bridge.h — geo_jump ↔ KIS decomposition bridge
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * geo_jump (FGLS_new/collection/geo_jump_module) and the KIS timeline
 * address the SAME 20736 slots, but decompose the address differently:
 *
 *   geo_jump:  node = face · 1728 + tick · 144 + local
 *              face  ∈ [0,12)   (GEO_PENTAGONS)    — pentagon block
 *              tick  ∈ [0,12)   (GEO_SHELL_TICK)   — shell level
 *              local ∈ [0,144)  (GEO_TOWER)        — slot in the tower
 *   KIS:       node = hi · 81 + lo,  hi = 16H + h,  lo = 9L + l
 *              w = 9H + L (scale position), pos = 9h + l (local position)
 *
 * Both are canonical 3-level factorizations of the same integer:
 *
 *     12 · 12 · 144 = 1728 · 12 = 20736        (geo_jump)
 *     144 · 144     = 20736                    (KIS (w,pos) grid)
 *
 * This header is the SYNC BRIDGE: mapping between the two decompositions is
 * pure integer arithmetic (no table, no float) and roundtrips losslessly in
 * both directions — bijectivity proved in test_geo_sync_bridge.c over all
 * 20736 slots. The KIS side reuses the mixed-radix bridge (geo_scale_wire.h);
 * the geo_jump side is implemented here from its documented constants, so
 * this header is self-contained (no external include needed).
 *
 * Canonical partition note (see test_tess_12x1728.c): the geo_jump face
 * partition (blocks f·1728) and the KIS tetra partition (residue mod 12)
 * are BOTH uniform 12 × 1728 but DIFFERENT sets. The bridge does not pick
 * one — it maps coordinates, so either partition can be expressed on either
 * side through node ↔ (face,tick,local) ↔ (w,pos).
 */

#ifndef GEO_SYNC_BRIDGE_H
#define GEO_SYNC_BRIDGE_H

#include <stdint.h>
#include "geo_scale_wire.h"

#define GSB_FULL        GSW_FULL       /* 20736 — the shared field       */
#define GSB_FACES       12u            /* geo_jump GEO_PENTAGONS        */
#define GSB_TICKS       12u            /* geo_jump GEO_SHELL_TICK       */
#define GSB_TOWER       144u           /* geo_jump GEO_TOWER = window   */
#define GSB_FACE_NODES  (GSB_FULL / GSB_FACES)   /* 1728 = 12³           */
#define GSB_TICK_NODES  GSB_TOWER                /* 144 = 12²            */

/* ── geo_jump decomposition: node = face·1728 + tick·144 + local ──────── */
static inline uint32_t gsb_node_of(uint32_t face, uint32_t tick, uint32_t local) {
    return face * GSB_FACE_NODES + tick * GSB_TICK_NODES + local;
}

static inline uint32_t gsb_face_of(uint32_t node) { return node / GSB_FACE_NODES; }
static inline uint32_t gsb_tick_of(uint32_t node) { return (node / GSB_TICK_NODES) % GSB_TICKS; }
static inline uint32_t gsb_local_of(uint32_t node) { return node % GSB_TICK_NODES; }

static inline void gsb_split(uint32_t node, uint32_t *face, uint32_t *tick, uint32_t *local) {
    *face  = gsb_face_of(node);
    *tick  = gsb_tick_of(node);
    *local = gsb_local_of(node);
}

/* ── KIS side (reuses geo_scale_wire.h) ───────────────────────────────── */
/* node → (w, pos) — gsw_scale_of_node / gsw_pos_of_node (already inline)  */
/* (w, pos) → node — gsw_node_of_scale (already inline)                    */

/* ── THE BRIDGE: (face,tick,local) ↔ (w,pos) through node ─────────────── */
static inline void gsb_to_wpos(uint32_t face, uint32_t tick, uint32_t local,
                               uint32_t *w, uint32_t *pos) {
    uint32_t node = gsb_node_of(face, tick, local);
    *w   = gsw_scale_of_node(node);
    *pos = gsw_pos_of_node(node);
}

static inline void gsb_to_face_tick_local(uint32_t w, uint32_t pos,
                                          uint32_t *face, uint32_t *tick, uint32_t *local) {
    uint32_t node = gsw_node_of_scale(w, pos);
    gsb_split(node, face, tick, local);
}

#endif /* GEO_SYNC_BRIDGE_H */
