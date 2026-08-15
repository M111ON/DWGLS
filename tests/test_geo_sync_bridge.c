/*
 * test_geo_sync_bridge.c — geo_jump ↔ KIS sync bridge bijectivity
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Proves core/geo_sync_bridge.h: the geo_jump decomposition
 * (node = face·1728 + tick·144 + local, face/tick ∈ [0,12), local ∈ [0,144))
 * and the KIS mixed-radix decomposition (node = hi·81 + lo with
 * w = 9H + L, pos = 9h + l) address the SAME 20736 slots, and the bridge
 * maps between them bijectively in both directions — pure integer, lossless.
 *
 *   T1  geo_jump decomposition roundtrips — node → (face,tick,local) → node
 *       identity over all 20736; every coordinate in range
 *   T2  KIS decomposition roundtrips — node → (w,pos) → node identity
 *   T3  the bridge is a bijection — (face,tick,local) → (w,pos) hits every
 *       one of the 20736 (w,pos) slots exactly once
 *   T4  the bridge inverts — (w,pos) → (face,tick,local) → (w,pos) identity
 *   T5  factorizations agree — 12·12·144 = 144·144 = 20736; each face = 12
 *       ticks × 144 = 1728
 *   T6  canonical partition note — geo_jump faces (f·1728 blocks) and KIS
 *       tetra orbits (residue mod 12) are different sets (see
 *       test_tess_12x1728.c) — the bridge maps coordinates, not partitions
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_geo_sync_bridge tests/test_geo_sync_bridge.c
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/geo_sync_bridge.h"

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

int main(void) {
    printf("═ GEO_JUMP ↔ KIS SYNC BRIDGE — face×tick×local ↔ (w,pos) ═\n");
    printf("  node = face·1728 + tick·144 + local  =  hi·81 + lo  (mixed radix)\n\n");

    /* ── T1: geo_jump decomposition roundtrip ─────────────────────────── */
    {
        int ok = 1;
        for (uint32_t node = 0; node < GSB_FULL && ok; node++) {
            uint32_t f = gsb_face_of(node), t = gsb_tick_of(node), l = gsb_local_of(node);
            if (f >= GSB_FACES || t >= GSB_TICKS || l >= GSB_TOWER) { ok = 0; break; }
            if (gsb_node_of(f, t, l) != node) { ok = 0; break; }
            if (gsb_split(node, &f, &t, &l), gsb_node_of(f, t, l) != node) { ok = 0; break; }
        }
        CHECK("T1: (face,tick,local) ↔ node roundtrips losslessly over all 20736 — all coords in range",
              ok);
    }

    /* ── T2: KIS decomposition roundtrip ──────────────────────────────── */
    {
        int ok = 1;
        for (uint32_t node = 0; node < GSB_FULL && ok; node++) {
            uint32_t w = gsw_scale_of_node(node), p = gsw_pos_of_node(node);
            if (w >= GSW_LOCAL || p >= GSW_LOCAL) { ok = 0; break; }
            if (gsw_node_of_scale(w, p) != node) { ok = 0; break; }
        }
        CHECK("T2: (w,pos) ↔ node roundtrips losslessly over all 20736 (KIS mixed radix)", ok);
    }

    /* ── T3: bridge (face,tick,local) → (w,pos) is a bijection ────────── */
    {
        uint8_t *seen = (uint8_t *)calloc(GSB_FULL, 1);
        int ok = 1, count = 0;
        if (!seen) { printf("  T: FAIL — alloc\n"); return 1; }
        for (uint32_t f = 0; f < GSB_FACES && ok; f++) {
            for (uint32_t t = 0; t < GSB_TICKS && ok; t++) {
                for (uint32_t l = 0; l < GSB_TOWER; l++) {
                    uint32_t w, p;
                    gsb_to_wpos(f, t, l, &w, &p);
                    if (w >= GSW_LOCAL || p >= GSW_LOCAL) { ok = 0; break; }
                    uint32_t slot = w * GSW_LOCAL + p;
                    if (seen[slot]) { ok = 0; break; }
                    seen[slot] = 1;
                    count++;
                }
            }
        }
        uint32_t filled = 0;
        for (uint32_t i = 0; i < GSB_FULL; i++) filled += seen[i];
        free(seen);
        CHECK("T3: (face,tick,local) → (w,pos) is a bijection — all 20736 (w,pos) slots hit exactly once",
              ok && count == GSB_FULL && filled == GSB_FULL);
    }

    /* ── T4: bridge inverts — (w,pos) → (face,tick,local) → (w,pos) ───── */
    {
        int ok = 1;
        for (uint32_t w = 0; w < GSW_LOCAL && ok; w++) {
            for (uint32_t p = 0; p < GSW_LOCAL; p++) {
                uint32_t f, t, l, w2, p2;
                gsb_to_face_tick_local(w, p, &f, &t, &l);
                gsb_to_wpos(f, t, l, &w2, &p2);
                if (w2 != w || p2 != p) { ok = 0; break; }
            }
        }
        CHECK("T4: (w,pos) → (face,tick,local) → (w,pos) identity over all 20736", ok);
    }

    /* ── T5: factorizations agree ─────────────────────────────────────── */
    {
        CHECK("T5a: 12·12·144 = 144·144 = 20736 — both decompositions tile the same field",
              GSB_FACES * GSB_TICKS * GSB_TOWER == GSB_FULL &&
              GSW_LOCAL * GSW_LOCAL == GSB_FULL);
        CHECK("T5b: each face = 12 ticks × 144 = 1728 = 12³",
              GSB_FACE_NODES == GSB_TICKS * GSB_TICK_NODES && GSB_FACE_NODES == 1728u);
    }

    /* ── T6: partitions differ — the bridge maps coordinates, not sets ── */
    {
        /* node 0 and 1: same geo_jump face (block 0), different KIS residue */
        int same_face = gsb_face_of(0) == gsb_face_of(1);
        int diff_res  = (0u % 12u) != (1u % 12u);
        /* node 0 and 1728: same residue, different face */
        int same_res  = (0u % 12u) == (1728u % 12u);
        int diff_face = gsb_face_of(0) != gsb_face_of(1728);
        CHECK("T6: face partition ≠ residue partition — bridge is coordinate-level, either side can express either partition",
              same_face && diff_res && same_res && diff_face);
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
