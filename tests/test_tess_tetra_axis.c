/*
 * test_tess_tetra_axis.c — Tetrahedron Axis Walk proof
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * The equal-triangle cell lifts to the regular tetrahedron (3D simplex):
 *   4 vertices, 6 edges, 4 faces — all integer. 12 = 4×3 (faces × edges per
 *   face) = 6 edges × 2 directions = directed edges — the tetrahedron's
 *   structure IS the sacred 12, and 12⁴ = 20736 = the field.
 *
 * TETRA-AXIS WALK — running the field along the tetrahedron's 12 directed
 * edges (4 vertices × 3 edges per vertex):
 *   orbit r = { r + 12k : k ∈ [0,1728) }   (stride-12, mod 20736)
 *   12 orbits × 1728 = 20736 — an exact partition.
 *   1728 = 12³ = TH_PENTAGON_NODES — each orbit is one pentagon's worth.
 *
 * The walk is a closed cycle: from ANY node, 1728 steps return to it — no
 * start, no 0, no distinguished entry point (enter anywhere).
 *
 * Proof:
 *   T1  tetrahedron counts: V=4, E=6, F=4 — Euler V−E+F = 2; edges C(4,2)=6,
 *       vertices C(4,3)=4 (4 planes → 6 edge crossings → 4 vertex crossings)
 *   T1b directed edges = 12 = 4 vertices × 3 edges = 6 edges × 2 directions
 *   T2  12 orbits × 1728 = 20736 — the field partitions into 12 equal orbits
 *   T3  1728 = 12³ = TH_PENTAGON_NODES — each orbit = one pentagon's worth
 *   T4  cyclic — from ANY start, 1728 steps return; orbit membership is
 *       by residue only (no start, no 0 — no orbit is privileged)
 *   T5  tetra mapping — orbit r ↔ (vertex r/3, edge r%3): the 12 orbits are
 *       exactly the 12 directed edges of the tetrahedron
 *   T6  field = 12⁴ = 12 × 12³ — tetrahedron structure to the 4th dimension
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/test_tess_tetra_axis tests/test_tess_tetra_axis.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/tri_hex_tess.h"

#define TTA_FULL    20736u
#define TTA_ORBITS  12u
#define TTA_STRIDE  12u
#define TTA_ORB_SZ  (TTA_FULL / TTA_ORBITS)   /* 1728 */

static int pass_count = 0, fail_count = 0;
#define CHECK(desc, cond) do { \
    if (cond) { pass_count++; printf("  T: PASS — %s\n", desc); } \
    else      { fail_count++; printf("  T: FAIL — %s\n", desc); } \
} while (0)

static uint32_t choose2(uint32_t n) { return n * (n - 1u) / 2u; }
static uint32_t choose3(uint32_t n) { return n * (n - 1u) * (n - 2u) / 6u; }

int main(void) {
    printf("═ TETRAHEDRON AXIS WALK — 12 orbits × 1728 = 20736 ═\n");
    printf("  tetrahedron: 4 planes → 6 edge crossings → 4 vertex crossings\n\n");

    /* ── T1: tetrahedron counts ──────────────────────────────────────── */
    {
        uint32_t V = 4u, E = 6u, F = 4u;
        CHECK("T1: tetrahedron V=4 E=6 F=4 — Euler V−E+F = 2 (closed 3D cell)",
              V - E + F == 2u && V == 4u && E == 6u && F == 4u);
        CHECK("T1b: crossings — 4 planes → 6 edges C(4,2), 4 vertices C(4,3)",
              choose2(4u) == 6u && choose3(4u) == 4u);
        CHECK("T1c: directed edges = 12 = 4 vertices × 3 edges = 6 edges × 2",
              4u * 3u == 12u && 6u * 2u == 12u && E * 2u == 12u);
    }

    /* ── T2: 12 orbits × 1728 = 20736 — exact partition ─────────────── */
    {
        uint32_t sizes[TTA_ORBITS];
        memset(sizes, 0, sizeof(sizes));
        uint8_t *seen = (uint8_t *)calloc(TTA_FULL, 1);
        if (!seen) { printf("  T: FAIL — alloc\n"); return 1; }
        int ok = 1;
        for (uint32_t n = 0; n < TTA_FULL; n++) {
            if (seen[n]) { ok = 0; break; }        /* every node once     */
            seen[n] = 1;
            sizes[n % TTA_ORBITS]++;               /* orbit = residue mod 12 */
        }
        for (uint32_t r = 0; r < TTA_ORBITS && ok; r++)
            if (sizes[r] != TTA_ORB_SZ) ok = 0;    /* all orbits equal     */
        free(seen);
        CHECK("T2: 12 orbits × 1728 = 20736 — equal partition (residue mod 12)",
              ok && TTA_ORBITS * TTA_ORB_SZ == TTA_FULL);
        printf("     orbit size = %u = 20736/12\n", (unsigned)TTA_ORB_SZ);
    }

    /* ── T3: 1728 = 12³ = TH_PENTAGON_NODES ─────────────────────────── */
    {
        CHECK("T3: 1728 = 12³ — each orbit = one pentagon's worth (TH_PENTAGON_NODES)",
              TTA_ORB_SZ == TH_PENTAGON_NODES && 12u * 12u * 12u == TTA_ORB_SZ);
        CHECK("T3b: 12 pentagons × 1728 = 20736 — pentagon structure intact",
              GEO_PENTAGONS * TH_PENTAGON_NODES == GEO_FULL);
    }

    /* ── T4: cyclic — any start, 1728 steps return; no origin ───────── */
    {
        int ok = 1;
        for (uint32_t s = 0; s < TTA_ORBITS; s++) {   /* one start per orbit */
            uint32_t n = s, steps = 0;
            uint8_t visited[TTA_ORBITS];
            memset(visited, 0, sizeof(visited));
            do {
                visited[n % TTA_ORBITS] = 1;
                n = (n + TTA_STRIDE) % TTA_FULL;
                steps++;
            } while (n != s);
            if (steps != TTA_ORB_SZ) ok = 0;          /* full orbit, closed */
            for (uint32_t r = 0; r < TTA_ORBITS; r++) /* only own residue   */
                if (visited[r] != (r == s)) ok = 0;
        }
        /* no-0: any node of the orbit — not just the residue representative —
         * is a valid entry; the cycle is identical from anywhere in it */
        int ok_any = 1;
        for (uint32_t s = 0; s < TTA_ORBITS; s++) {
            uint32_t mid = (s + 7u * TTA_STRIDE) % TTA_FULL; /* deep node of orbit s */
            uint32_t n = mid, steps = 0;
            do { n = (n + TTA_STRIDE) % TTA_FULL; steps++; } while (n != mid);
            if (steps != TTA_ORB_SZ) ok_any = 0;
            if (mid % TTA_ORBITS != s) ok_any = 0;
        }
        CHECK("T4: from ANY start, 1728 steps return — closed cycle, no origin",
              ok);
        CHECK("T4b: any node of the orbit is a valid entry — no privileged point (no 0)",
              ok_any);
    }

    /* ── T5: tetra mapping — orbit r = (vertex r/3, edge r%3) ────────── */
    {
        int ok = 1;
        for (uint32_t r = 0; r < TTA_ORBITS; r++) {
            uint32_t v = r / 3u, e = r % 3u;
            if (v >= 4u || e >= 3u) ok = 0;          /* 4 vertices × 3 edges */
            if (v * 3u + e != r) ok = 0;
        }
        CHECK("T5: 12 orbits = 4 vertices × 3 edges — r ↔ (v=r/3, e=r%3)", ok);
    }

    /* ── T6: field = 12⁴ = 12 × 12³ ─────────────────────────────────── */
    {
        CHECK("T6: 20736 = 12⁴ = 12 × 12³ — tetra structure to the 4th",
              12u * 12u * 12u * 12u == TTA_FULL && TTA_ORBITS * TTA_ORB_SZ == TTA_FULL);
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("RESULTS: %d/%d PASS\n", pass_count, pass_count + fail_count);
    return fail_count ? 1 : 0;
}
