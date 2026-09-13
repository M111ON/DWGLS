/*
 * dual_loop.h — Ico(20) <-> Dodec(12) alternation (design §714, built 2026-09-13)
 * ═══════════════════════════════════════════════════════════════════════════
 * THE LOOP (uniform scale throughout, #814 — duality never rescales):
 *   Phase 20 (traveling): 20 seeds, one per icosa face (fresh seeds enter
 *     here every round — new planet, new scale, new model).
 *   Convert (settle): homes[v] = XOR of the 5 face-seeds touching vertex v.
 *     Exact because every icosa vertex touches exactly 5 faces (definition).
 *   Phase 12 (standing): 12 settled homes (PlanetSys-compatible indexing).
 *   Re-seed (split): seeds[f] = homes[A]^homes[B]^homes[C] (face incidence).
 *
 * MATHEMATICAL TEETH (proven in test, not asserted):
 *   - Conservation: XOR(homes[12]) == XOR(seeds[20]) (each seed counted 3x,
 *     once per vertex of its face; odd count preserves).
 *   - Settle is IDEMPOTENT: merge(split(h)) == h exactly (5x self = self,
 *     2x neighbors vanish). Homes are fixed points; seeds are the orbit.
 *   - Asymmetric: split(merge(s)) != s in general (vertex-neighbors mix).
 *     The loop's energy comes from FRESH seeds each round, never from
 *     cycling old ones. No perpetual motion.
 *
 * Single source of truth: GP16N_ICO_FACES (gp16_neighbors.h). The
 * vertex->faces table is derived once, never duplicated.
 *
 * Header-only, int-only, zero dependencies beyond gp16_neighbors.h.
 */
#ifndef DUAL_LOOP_H
#define DUAL_LOOP_H

#include <stdint.h>
#include "gp16_neighbors.h"

#define DUAL_SEEDS 20u
#define DUAL_HOMES 12u
#define DUAL_FACES_PER_VERT 5u
#define DUAL_VERTS_PER_FACE 3u

static uint8_t dual_vf[DUAL_HOMES][DUAL_FACES_PER_VERT];
static uint8_t dual_vf_built = 0;

static inline void dual_build(void) {
    if (dual_vf_built) return;
    uint8_t n[DUAL_HOMES] = {0};
    for (uint32_t f = 0; f < DUAL_SEEDS; f++) {
        for (uint32_t e = 0; e < DUAL_VERTS_PER_FACE; e++) {
            uint32_t v = GP16N_ICO_FACES[f][e];
            if (v < DUAL_HOMES && n[v] < DUAL_FACES_PER_VERT)
                dual_vf[v][n[v]++] = (uint8_t)f;
        }
    }
    dual_vf_built = 1;
}

/* settle: 20 seeds -> 12 homes (5->1 XOR per vertex) */
static inline void dual_seed_to_home(const uint32_t seeds[DUAL_SEEDS],
                                     uint32_t homes[DUAL_HOMES]) {
    dual_build();
    if (!seeds || !homes) return;
    for (uint32_t v = 0; v < DUAL_HOMES; v++) {
        uint32_t h = 0;
        for (uint32_t k = 0; k < DUAL_FACES_PER_VERT; k++)
            h ^= seeds[dual_vf[v][k]];
        homes[v] = h;
    }
}

/* split: 12 homes -> 20 seeds (face incidence XOR) */
static inline void dual_home_to_seed(const uint32_t homes[DUAL_HOMES],
                                     uint32_t seeds[DUAL_SEEDS]) {
    if (!homes || !seeds) return;
    for (uint32_t f = 0; f < DUAL_SEEDS; f++) {
        seeds[f] = homes[GP16N_ICO_FACES[f][0]] ^
                   homes[GP16N_ICO_FACES[f][1]] ^
                   homes[GP16N_ICO_FACES[f][2]];
    }
}

/* ═══════════════ 32-UNIT FIELD VIEW (20+12 summed) ═══════════════
 * 20 faces + 12 vertices = 32 = the whole icosahedron counted at once
 * (V+F = 12+20; Euler 12-30+20=2). 32 x 648 = 20736 is a Goldberg level;
 * each unit splits 648 = 8 x 81 = KIS wheel x Peano ladder (the dual-rail
 * skeleton #141, per unit). So the loop's two phases summed ARE a field
 * tiling whose cells carry the dual rail. View only — moves no data. */
#define DUAL32_UNITS 32u
#define DUAL32_SLOTS 648u
#define DUAL32_WHEEL 8u
#define DUAL32_LADDER 81u

static inline uint32_t d32_face(uint32_t flat) { return flat / DUAL32_SLOTS; }
static inline uint32_t d32_local(uint32_t flat) { return flat % DUAL32_SLOTS; }
static inline uint32_t d32_wheel(uint32_t flat) {
    return d32_local(flat) / DUAL32_LADDER;
}
static inline uint32_t d32_ladder(uint32_t flat) {
    return d32_local(flat) % DUAL32_LADDER;
}
static inline uint32_t d32_flat(uint32_t face, uint32_t wheel, uint32_t ladder) {
    return face * DUAL32_SLOTS + wheel * DUAL32_LADDER + ladder;
}

#endif /* DUAL_LOOP_H */
