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
    return 0;
}
