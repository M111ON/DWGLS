/* tests/test_wang_gate.c — Wang gate on the ANN candidate path.
 *
 * Insertion point (spec #7163): between anch_route (top-b buckets) and the
 * candidate scan. A candidate bucket is live only if its Wang gate faces
 * the query bucket with matching colors (mm_wang.h); shut gates open ON
 * DEMAND via mmw_override — that override IS the "called face" mechanism:
 * only the called face's subset is live.
 *
 * Edge semantics used here: buckets face each other E(query side) against
 * W(candidate side); slide comes from the query's cluster coordinate;
 * layer 0 (single universe). The grid cell-cross predicate
 * (mm_minor_cross) is NOT used — buckets are not grid cells; the gate
 * (mmw_open) is the Wang part and is used directly. Documented, not abused.
 *
 * Gates:
 *   W1 exclusion  : gated scores a strict subset of ungated (non-vacuous)
 *   W2 face-off   : queries whose top-1 bucket is shut get a different top-1
 *   W3 face-on    : mmw_override opens the shut gate, top-1 restored (== ungated)
 *   W4 hygiene    : mmw_clear resets; default colors deterministic (same slide
 *                   + same buckets => same gate, twice in a row)
 *
 * BUILD: gcc -O2 -I. -Icore -o build/test_wang_gate tests/test_wang_gate.c -lm
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/anchor_route.h"
#include "core/mm_wang.h"

#define NENTRY   600
#define DIM      4
#define KANCH    8
#define TOPB     2
#define NQUERY   200

static int g_pass = 0, g_fail = 0;
static void check_ok(int ok, const char *name) {
    if (ok) { g_pass++; printf("  PASS  %s\n", name); }
    else    { g_fail++; printf("  FAIL  %s\n", name); }
}

static uint32_t rng = 0x1234abcd;
static uint32_t next_u32(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static double dist2(const float *a, const float *b) {
    double d = 0;
    for (int j = 0; j < DIM; j++) { double e = (double)a[j] - b[j]; d += e * e; }
    return d;
}

/* Same system-intrinsic features as test_light_index (keeps the joint honest). */
static void feat(uint32_t i, uint32_t access, float *v) {
    uint32_t slot = (uint32_t)(((uint64_t)i * 37u) % 20736u);
    v[0] = (float)slot / 20736.0f;
    v[1] = (float)(slot % 144u) / 144.0f;
    v[2] = (float)access / 64.0f;
    v[3] = 64.0f / 128.0f;
}

static MVNode bnode(uint32_t bucket, uint32_t slide) {
    MVNode n;
    n.node = bucket; n.slide = slide; n.layer = 0; n.entry = 0; n.exit = 0;
    return n;
}

int main(void) {
    printf("=== WANG GATE (ANN candidate path) ===\n");
    mmw_clear();

    static float X[NENTRY * DIM];
    static uint32_t acc[NENTRY];
    for (uint32_t i = 0; i < NENTRY; i++) {
        acc[i] = next_u32() % 6u;
        feat(i, acc[i], X + (size_t)i * DIM);
    }
    static float C[KANCH * DIM];
    static int lab[NENTRY];
    if (anch_train(X, NENTRY, DIM, KANCH, C, lab) != 0) {
        printf("  FAIL: anch_train\n"); return 1;
    }

    static float q[DIM];
    static int buckets[TOPB > KANCH ? TOPB : KANCH];
    long scored_un = 0, scored_g = 0;
    int differ = 0, restored = 0, differed_total = 0;
    static int qbucket[NQUERY], qslide[NQUERY], qtop1[NQUERY];

    /* Boundary queries: midpoint between a member and its nearest
     * out-of-bucket neighbor, so the true top-1 often sits across the
     * bucket fence — the only place a cross gate can change the answer. */
    static uint32_t bpair[NQUERY][2];
    for (int qi = 0; qi < NQUERY; qi++) {
        uint32_t src = (uint32_t)((uint64_t)qi * 7919u % NENTRY);
        int best = -1; double bd = 1e300;
        for (uint32_t j = 0; j < NENTRY; j++) {
            if (lab[j] == lab[src]) continue;
            double d = dist2(X + (size_t)src * DIM, X + (size_t)j * DIM);
            if (d < bd) { bd = d; best = (int)j; }
        }
        bpair[qi][0] = src; bpair[qi][1] = best < 0 ? src : (uint32_t)best;
    }

    for (int qi = 0; qi < NQUERY; qi++) {
        uint32_t a = bpair[qi][0], b = bpair[qi][1];
        for (int j = 0; j < DIM; j++)
            q[j] = (X[(size_t)a * DIM + j] + X[(size_t)b * DIM + j]) * 0.5f;
        uint32_t slide = ((uint32_t)(q[1] * 143.0f)) % 144u;
        int qb = anch_assign(q, C, KANCH, DIM);
        int got = anch_route(q, C, KANCH, DIM, TOPB, buckets);
        qbucket[qi] = qb; qslide[qi] = (int)slide;

        /* ungated top-1 over top-b buckets */
        int ub = -1; double ud = 1e300;
        for (int t = 0; t < got; t++)
            for (uint32_t i = 0; i < NENTRY; i++) {
                if (lab[i] != buckets[t]) continue;
                scored_un++;
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < ud) { ud = d; ub = (int)i; }
            }
        qtop1[qi] = ub;

        /* gated: the query's own bucket is home (no crossing, always
         * live); other buckets need an open E->W facing gate. */
        MVNode qa = bnode((uint32_t)qb, slide);
        int gb = -1; double gd = 1e300;
        for (int t = 0; t < got; t++) {
            if (buckets[t] != qb) {
                MVNode bc = bnode((uint32_t)buckets[t], slide);
                if (!mmw_open(&qa, MMW_EDGE_E, &bc, MMW_EDGE_W)) continue;
            }
            for (uint32_t i = 0; i < NENTRY; i++) {
                if (lab[i] != buckets[t]) continue;
                scored_g++;
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < gd) { gd = d; gb = (int)i; }
            }
        }
        if (gb != ub) { differ++; }
    }
    printf("    scored ungated=%ld gated=%ld, top-1 differs on %d/%d queries\n",
           scored_un, scored_g, differ, NQUERY);
    check_ok(scored_g < scored_un, "W1 gated scores a strict subset (non-vacuous)");
    check_ok(differ > 0, "W2 at least one query loses its top-1 behind a shut gate");

    /* W3: call the face — override opens query->top1-bucket gates, top-1 restored. */
    int need = 0;
    for (int qi = 0; qi < NQUERY; qi++) {
        uint32_t src = 0;
        /* re-derive the same query deterministically is complex; instead
         * re-run: recompute buckets for stored query is unavailable, so
         * override per (query-bucket, top1-bucket) pair seen to differ. */
        (void)src;
        if (qtop1[qi] < 0) continue;
        int tb = lab[qtop1[qi]];
        if (tb == qbucket[qi]) continue;   /* same bucket needs no cross gate */
        MVNode qa = bnode((uint32_t)qbucket[qi], (uint32_t)qslide[qi]);
        MVNode bc = bnode((uint32_t)tb, (uint32_t)qslide[qi]);
        if (!mmw_open(&qa, MMW_EDGE_E, &bc, MMW_EDGE_W)) {
            mmw_override((uint32_t)qbucket[qi], MMW_EDGE_E, 1);
            need++;
        }
    }
    printf("    W3: opened %d shut query-faces via override\n", need);
    /* re-run gated pass with overrides in place */
    rng = 0x1234abcd;
    for (uint32_t i = 0; i < NENTRY; i++) {
        acc[i] = next_u32() % 6u;
        feat(i, acc[i], X + (size_t)i * DIM);
    }
    int still_differ = 0, checked = 0;
    for (int qi = 0; qi < NQUERY; qi++) {
        uint32_t a = bpair[qi][0], b = bpair[qi][1];
        for (int j = 0; j < DIM; j++)
            q[j] = (X[(size_t)a * DIM + j] + X[(size_t)b * DIM + j]) * 0.5f;
        uint32_t slide = ((uint32_t)(q[1] * 143.0f)) % 144u;
        int qb = anch_assign(q, C, KANCH, DIM);
        int got = anch_route(q, C, KANCH, DIM, TOPB, buckets);
        int ub = -1; double ud = 1e300;
        for (int t = 0; t < got; t++)
            for (uint32_t i = 0; i < NENTRY; i++) {
                if (lab[i] != buckets[t]) continue;
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < ud) { ud = d; ub = (int)i; }
            }
        MVNode qa = bnode((uint32_t)qb, slide);
        int gb = -1; double gd = 1e300;
        for (int t = 0; t < got; t++) {
            if (buckets[t] != qb) {
                MVNode bc = bnode((uint32_t)buckets[t], slide);
                if (!mmw_open(&qa, MMW_EDGE_E, &bc, MMW_EDGE_W)) continue;
            }
            for (uint32_t i = 0; i < NENTRY; i++) {
                if (lab[i] != buckets[t]) continue;
                double d = dist2(q, X + (size_t)i * DIM);
                if (d < gd) { gd = d; gb = (int)i; }
            }
        }
        /* only compare queries whose top-1 bucket differs from query bucket
         * (same-bucket top-1 never needed a cross gate) */
        if (ub >= 0 && lab[ub] != qb) {
            checked++;
            if (gb == ub) restored++;
            else still_differ++;
        }
    }
    differed_total = checked;
    printf("    W3: cross-bucket queries %d, restored %d, still differ %d\n",
           checked, restored, still_differ);
    check_ok(checked > 0, "W3 cross-bucket queries exist to test override");
    check_ok(still_differ == 0 && restored == checked,
             "W3 override restores ungated top-1 on every cross-bucket query");

    /* W4: hygiene — clear resets to color defaults; defaults deterministic. */
    mmw_clear();
    MVNode a0 = bnode(3, 41), b0 = bnode(5, 41);
    int d1 = mmw_open(&a0, MMW_EDGE_E, &b0, MMW_EDGE_W);
    int d2 = mmw_open(&a0, MMW_EDGE_E, &b0, MMW_EDGE_W);
    check_ok(d1 == d2, "W4 default gate deterministic across calls");
    mmw_override(3, MMW_EDGE_E, !d1);
    int d3 = mmw_open(&a0, MMW_EDGE_E, &b0, MMW_EDGE_W);
    mmw_clear();
    int d4 = mmw_open(&a0, MMW_EDGE_E, &b0, MMW_EDGE_W);
    check_ok(d3 != d1 && d4 == d1, "W4 override flips, clear restores default");
    (void)differed_total;

    printf("=== RESULTS: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    printf("Receipt: wang{scored_un=%ld scored_g=%ld differ=%d restored=%d}\n",
           scored_un, scored_g, differ, restored);
    return g_fail ? 1 : 0;
}