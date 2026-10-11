/* anchor_arena.c — the anchor contest (owner 2026-10-10):
 * "ประลองหาผู้แข็งแกร่ง": every candidate on the SAME columns, same scale.
 *
 * Contestants (system combos, the way they would actually be picked):
 *   S1 active-full    A+w64, store 128B
 *   S2 active-narrow  A+w16, store rung
 *   S3 trap           B+w16, 4B steady identity + fp per pass
 *   S4 derived-table  D, no per-anchor anything (12KB table)
 *   S5 trained        F, status quo (regrow pipeline)
 * Columns (measured here unless tagged):
 *   build   ns per anchor (this box, timed)
 *   steady  bytes per anchor at rest (sizeof / derivation)
 *   recall  top-5 overlap/5 vs exact @budget8000 (TAG: query runs 2026-10-10)
 *   determ  replay same input -> same output? (tested here)
 *   redo    what must redo when the index changes ( Adams: documented/measured)
 *   upfront time+RAM for 1M UNWALKED anchors (projection from measured units)
 *
 * Usage: anchor_arena   (prints one standings table; exit 0)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "anchor_weight.h"
#include "anchor_trap.h"
#include "anchor_centroid.h"
#include "anchor_frustum.h"

static void fill(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b[i] = (uint8_t)s; }
}

/* mean ns per op over N timed ops (clock(), totals kept >> resolution) */
typedef void (*opfn)(uint8_t *s, int i);
static double timeit(opfn op, uint8_t *s, int n) {
    clock_t t0 = clock();
    for (int i = 0; i < n; i++) op(s, i);
    double tot = (double)(clock() - t0) / CLOCKS_PER_SEC;
    return tot * 1e9 / n;
}

static aw_anchor_t g_a;
static at_trap_t g_t;
static uint8_t g_seed16[AT_SEED_LEN];

static void op_build64(uint8_t *s, int i) {
    s[0] = (uint8_t)(s[0] + i);
    aw_anchor_build_w(s, 32, 0, 64, &g_a);
}
static void op_build16(uint8_t *s, int i) {
    s[0] = (uint8_t)(s[0] + i);
    aw_anchor_build_w(s, 32, 0, 16, &g_a);
}
static void op_register(uint8_t *s, int i) {
    (void)i;
    memcpy(g_seed16, s, AT_SEED_LEN);
    at_register(g_seed16, &g_t);
}
static void op_firstcatch(uint8_t *s, int i) {
    uint8_t seed[AT_SEED_LEN];
    memcpy(seed, s, AT_SEED_LEN);
    seed[0] = (uint8_t)(seed[0] + i);
    at_register(seed, &g_t);
    s[0] = (uint8_t)(s[0] + i + 1);
    at_catch(&g_t, s, 32, 0, 16, NULL, NULL);
}
static void op_verify(uint8_t *s, int i) {
    (void)i;
    int d = -1;
    at_catch(&g_t, s, 32, 0, 16, NULL, &d);
}
static float g_tab[8 * 384];
static uint8_t g_seeds[8 * 4];
static void op_table8(uint8_t *s, int i) {
    (void)s; (void)i;
    ac_build_table(g_seeds, 4, 8, 384, g_tab);
}

/* ---- A3: frustum ray-order top-k, measured here (not a tagged query number) ----
 * Build N anchors; for a query anchor descend its 4-level frustum ray from the
 * core outward and check the exact-nearest anchor (by anchor-weight distance) is
 * reachable through the descent's own ray order rather than a flat score cutoff.
 * The descent is pure geometry (af_anchor_descent/af_child_seed_bytes); this is a
 * hit-rate assertion, oracle = brute nearest over the same N. */
#define A3_N 4096
static aw_anchor_t g_anch[A3_N];
static uint8_t     g_seedbuf[A3_N][32];

static uint32_t aw_dist(const aw_anchor_t *a, const aw_anchor_t *b) {
    uint32_t d = 0;
    for (int i = 0; i < AW_CELLS; i++) {
        int t = (int)a->pattern[i] - (int)b->pattern[i];
        d += (uint32_t)(t < 0 ? -t : t);
    }
    return d;
}

/* returns hit-rate *100 (integer %) of exact-nearest reachable via ray order */
static uint32_t a3_ray_seed(const aw_anchor_t *a) {
    FrustumRouteEvent ev[FR_MAX_DEPTH];
    int n = af_anchor_descent(a, ev);
    if (n <= 0) return 0;
    /* ray identity = the anchor's DIRECTION chain across all levels,
     * not the folded major seed (which proved anchor-invariant: 1 bucket/4096).
     * Each level's branch_path is biased by aw_weight_bias(a,d) -> anchor-specific. */
    uint32_t h = 2166136261u;
    for (int d = 0; d < n; d++) {
        h ^= (uint32_t)ev[d].branch_path; h *= 16777619u;
        h ^= (uint32_t)ev[d].level;       h *= 16777619u;
    }
    return h;
}

static void a3_build_corpus(void) {
    /* deterministic corpus — MUST run before any a3_ray_seed() consumer,
     * including the bucket diagnostic (a zero corpus reports 1 bogus ray). */
    for (int i = 0; i < A3_N; i++) {
        fill(g_seedbuf[i], 32, (uint32_t)(i * 2654435761u + 7u));
        aw_anchor_build(g_seedbuf[i], 32, (i & 1), &g_anch[i]);
    }
}

static int a3_ray_topk(int k) {
    int hits = 0, qn = 256;
    int same_bucket = 0, rank_sum = 0, rank_n = 0;
    for (int q = 0; q < qn; q++) {
        int qi = (q * 7919) % A3_N;
        const aw_anchor_t *Q = &g_anch[qi];
        /* brute nearest (oracle) */
        uint32_t best = 0xFFFFFFFFu; int bi = -1;
        for (int i = 0; i < A3_N; i++) {
            if (i == qi) continue;
            uint32_t d = aw_dist(Q, &g_anch[i]);
            if (d < best) { best = d; bi = i; }
        }
        if (bi < 0) continue;
        uint32_t qseed = a3_ray_seed(Q);
        if (qseed == 0) continue;
        /* does the exact-nearest share Q's ray bucket at all? and at what rank
         * (corpus position) inside that bucket? */
        if (a3_ray_seed(&g_anch[bi]) == qseed) same_bucket++;
        int rank = 0;
        for (int i = 0; i < A3_N; i++) {
            if (i == qi) continue;
            if (a3_ray_seed(&g_anch[i]) == qseed) {
                rank++;
                if (i == bi) { rank_sum += rank; rank_n++; if (rank <= k) hits++; break; }
            }
        }
    }
    int hr = hits * 100 / qn, sb = same_bucket * 100 / qn;
    printf("A3 ray-order hits: k=%d %d%% (nearest shares ray=%d%%, mean bucket rank=%.1f)\n",
           k, hr, sb, rank_n ? (double)rank_sum / rank_n : 0.0);
    return hr;
}

int main(void) {
    uint8_t s[32];
    fill(s, sizeof s, 0xA11CEu);
    for (int k = 0; k < 8; k++) {
        uint32_t v = ac_gear_seed((uint32_t)k, 0, 0);
        g_seeds[k*4+0] = (uint8_t)v; g_seeds[k*4+1] = (uint8_t)(v >> 8);
        g_seeds[k*4+2] = (uint8_t)(v >> 16); g_seeds[k*4+3] = (uint8_t)(v >> 24);
    }

    double ns_b64 = timeit(op_build64, s, 2000);
    double ns_b16 = timeit(op_build16, s, 8000);
    double ns_reg = timeit(op_register, s, 200000);
    double ns_fc = timeit(op_firstcatch, s, 8000);
    /* steady trap for verify timing (defined once, verified often) */
    { uint8_t sd[AT_SEED_LEN]; fill(sd, sizeof sd, 9u); at_register(sd, &g_t); }
    double ns_ver = timeit(op_verify, s, 200000);
    double ns_tab = timeit(op_table8, s, 2000);

    /* determinism: rebuild equality (S1/S2/S4), arrival-dependence (S3) */
    aw_anchor_t a1, a2;
    aw_anchor_build_w(s, 32, 0, 64, &a1);
    aw_anchor_build_w(s, 32, 0, 64, &a2);
    int det_active = (memcmp(&a1, &a2, sizeof a1) == 0);
    at_trap_t t1, t2;
    uint8_t sd[AT_SEED_LEN];
    fill(sd, sizeof sd, 5u);
    uint8_t p1[32], p2[32];
    fill(p1, sizeof p1, 0xAAu); fill(p2, sizeof p2, 0xBBu);
    at_register(sd, &t1); at_register(sd, &t2);
    at_catch(&t1, p1, sizeof p1, 0, 16, NULL, NULL);
    at_catch(&t2, p1, sizeof p1, 0, 16, NULL, NULL);
    at_catch(&t1, p1, sizeof p1, 0, 16, NULL, NULL);
    int det_trap_same = (memcmp(&t1.anchor, &t2.anchor, sizeof t1.anchor) == 0);
    at_trap_t t3;
    at_register(sd, &t3);
    at_catch(&t3, p2, sizeof p2, 0, 16, NULL, NULL);
    int det_trap_diff = (memcmp(&t1.anchor, &t3.anchor, sizeof t1.anchor) != 0);

    printf("=== anchor arena (this box, N timed) ===\n");
    printf("build/anchor : active64 %.0f ns | active16 %.0f ns | trap-register %.0f ns | trap-first %.0f ns | trap-verify %.0f ns | table8x384 %.0f ns\n",
           ns_b64, ns_b16, ns_reg, ns_fc, ns_ver, ns_tab);
    printf("sizeofs      : trap-struct %lu B | anchor-struct %lu B | table8x384 %lu B | steady-identity ~4 B\n",
           (unsigned long)sizeof(at_trap_t), (unsigned long)sizeof(aw_anchor_t),
           (unsigned long)sizeof(g_tab));
    printf("determinism  : active-rebuild %d | trap-same-arrival %d | trap-diff-arrival-diverges %d\n",
           det_active, det_trap_same, det_trap_diff);
    printf("--- standings (shared columns; recall = query runs 2026-10-10 @budget8000) ---\n");
    printf("S1 active-full   : build %.0f ns | steady 128 B | recall 5.00/5(full-scan tier) | determ yes | redo: rebuild changed anchors | upfront 1M: %.0f s + 128 MB\n",
           ns_b64, ns_b64 * 1e6 / 1e9);
    printf("S2 active-narrow : build %.0f ns | steady 128 B(reserve) | recall same-tier-as-table-fed | determ yes | redo: rebuild changed anchors | upfront 1M: %.1f s + 128 MB\n",
           ns_b16, ns_b16 * 1e6 / 1e9);
    printf("S3 trap          : register %.0f ns, first %.0f ns, verify %.0f ns | steady 4 B + 16 B/pass fp | recall UNMEASURED(trap-painted table not yet run) | determ arrival-conditional | redo: nothing until walked | upfront 1M unwalked: ~0 s + ~16 MB seeds\n",
           ns_reg, ns_fc, ns_ver);
    printf("S4 derived-table : table %.0f ns one-time | steady 12 KB fixed | recall 2.17/5 pruned (=trained tier) | determ yes | redo: rebuild table ms | upfront 1M: ~0 s + 12 KB\n",
           ns_tab);
    printf("S5 trained       : regrow ~60 s documented | steady 12 KB fixed | recall 2.70/5 pruned, 5.00/5 full | determ retrain-dependent | redo: retrain ~60 s | upfront 1M: ~60 s + 12 KB\n");
    printf("--- A3 frustum ray-order top-k (MEASURED here, corpus N=%d, queries 256) ---\n", A3_N);
    a3_build_corpus();
    /* diagnostic: ray bucket size distribution (how many of N share one ray) */
    {
        int occ[64] = {0};
        int nb = 0;
        uint32_t seen[64]; int nseen = 0;
        for (int i = 0; i < A3_N; i++) {
            uint32_t s = a3_ray_seed(&g_anch[i]);
            int j = 0; for (; j < nseen; j++) if (seen[j] == s) break;
            if (j == nseen && nseen < 64) { seen[nseen++] = s; }
            if (j < 64) occ[j]++;
        }
        int distinct = nseen;
        int mx = 0; for (int j = 0; j < distinct; j++) if (occ[j] > mx) mx = occ[j];
        printf("A3 ray buckets   : %d distinct rays over %d anchors, max bucket %d (mean %.1f)\n",
               distinct, A3_N, mx, distinct ? (double)A3_N / distinct : 0.0);
        nb = distinct;
        (void)nb;
    }
    /* per-k line carries hit%, nearest-shares-ray%, mean bucket rank. NOTE:
     * the frustum descent ray is geometry-only and, as a routing key over
     * anchor-weight space, is nearly ORTHOGONAL to the metric that matters:
     * the exact-nearest neighbour shares the query's ray ~1% of the time
     * (mean bucket rank ~195), so ray order is NOT a valid retrieval order
     * (measured 2026-10-11; consistent with #7346/#7348 route-identity). */
    a3_ray_topk(1);
    a3_ray_topk(5);
    a3_ray_topk(16);
    return 0;
}
