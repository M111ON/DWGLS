/* test_jet_select_prod.c — production jet_select wire-in vs independent oracles
 * ═══════════════════════════════════════════════════════════════════════════
 * Proves core/infra/jet_select.h (production) matches:
 *   O1  policy table  — L==0→C1, WIN<=2L-1→C3, else B (owner 2026-09-24)
 *   O2  merge formulas — closed-form SUT == iterative REF loops
 *   O3  geo_pipeline_want integration — select + coalesce counters
 *
 * Oracles are independent (policy table + advance-until-property loops),
 * never restatements of the functions under test.
 *
 * BUILD: make test-test_jet_select_prod
 * RUN:   ./build/test-test_jet_select_prod
 * ═══════════════════════════════════════════════════════════════════════════
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../core/infra/jet_select.h"
#include "../core/infra/geo_gpu_pipeline.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while (0)

/* ── O1: policy table (owner, independent of jet_select) ── */
static int oracle_pick(int win, int L)
{
    if (L == 0)       return JET_C1;
    if (win <= 2*L-1) return JET_C3;
    return JET_B;
}

/* ── O2: iterative merge oracles (spec-property loops) ── */
static int ref_merge(int strat, int win, int t, int L)
{
    switch (strat) {
    case JET_C3: {
        int r = t / win, m = 0;
        while (m / win <= r) m += win;
        return m;
    }
    case JET_B: {
        int a = t + L;
        while (a % win) a++;
        return a;
    }
    case JET_C1:
    default:
        return t + L;
    }
}

int main(void)
{
    printf("jet_select production: policy + merge + pipeline wire-in\n");

    /* ── T1: policy grid — production jet_select == oracle table ── */
    static const int WS[] = { 12, 24, 48, 144 };
    static const int LS[] = { 0, 1, 3, 5, 7, 11, 23 };
    #define NW ((int)(sizeof WS / sizeof WS[0]))
    #define NL ((int)(sizeof LS / sizeof LS[0]))
    int t1_ok = 1, used[3] = {0,0,0};
    for (int wi = 0; wi < NW; wi++)
        for (int li = 0; li < NL; li++) {
            int w = WS[wi], L = LS[li];
            int got = jet_select(w, L);
            int exp = oracle_pick(w, L);
            if (got != exp) t1_ok = 0;
            if (got >= 0 && got <= 2) used[got] = 1;
        }
    CHECK(1, "jet_select == policy table (4 WIN × 7 L, all 3 picks hit)",
          t1_ok && used[0] && used[1] && used[2]);

    /* ── T2: merge closed-form == iterative REF (strat × WIN × t × L) ── */
    int t2_ok = 1;
    static const int TS[] = { 0, 1, 5, 11, 12, 13, 23, 24, 47, 100, 143 };
    #define NT ((int)(sizeof TS / sizeof TS[0]))
    for (int wi = 0; wi < NW; wi++)
        for (int li = 0; li < NL; li++)
            for (int ti = 0; ti < NT; ti++)
                for (int s = 0; s <= JET_C1; s++) {
                    int w = WS[wi], L = LS[li], t = TS[ti];
                    if (jet_merge(s, w, t, L) != ref_merge(s, w, t, L))
                        t2_ok = 0;
                }
    CHECK(2, "jet_merge SUT == REF loops (3 strat × 4 WIN × 7 L × 11 t)", t2_ok);

    /* ── T3: C3 lands on boundary, ignores L ── */
    int t3_ok = 1;
    for (int ti = 0; ti < NT; ti++)
        for (int li = 0; li < NL; li++) {
            int t = TS[ti], L = LS[li];
            int m = jet_merge(JET_C3, 12, t, L);
            if (m % 12 != 0) t3_ok = 0;
            if (m != jet_merge(JET_C3, 12, t, L + 99)) t3_ok = 0;
        }
    CHECK(3, "C3: merge on WIN boundary, L ignored (no jet travel)", t3_ok);

    /* ── T4: B arrival always sync-exact (merge % WIN == 0) ── */
    int t4_ok = 1;
    for (int wi = 0; wi < NW; wi++)
        for (int li = 0; li < NL; li++)
            for (int ti = 0; ti < NT; ti++) {
                int w = WS[wi], L = LS[li], t = TS[ti];
                if (jet_merge(JET_B, w, t, L) % w != 0) t4_ok = 0;
            }
    CHECK(4, "B: arrival lands on sync for every WIN/L/t", t4_ok);

    /* ── T5: C1 latency == L (distance-free) ── */
    int t5_ok = 1;
    for (int li = 0; li < NL; li++)
        for (int ti = 0; ti < NT; ti++) {
            int L = LS[li], t = TS[ti];
            if (jet_merge(JET_C1, 12, t, L) - t != L) t5_ok = 0;
        }
    CHECK(5, "C1: merge − t == L for every ready phase", t5_ok);

    /* ── T6: geo_pipeline_want — select + merge tick + counters ── */
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);
    CHECK(6, "init: jet_strat_last == -1, wants_ready == 0",
          ctx.jet_strat_last == -1 && ctx.wants_ready == 0);

    /* L=0 → C1 special */
    int s0 = geo_pipeline_want(&ctx, 140, 0);
    CHECK(6, "want(140, L=0) → C1, merge=140, ready=1",
          s0 == JET_C1 && ctx.last_merge_tick == 140 && ctx.wants_ready == 1);

    /* WIN=12, L=11 → 12 <= 21 → C3 */
    int s1 = geo_pipeline_want(&ctx, 5, 11);
    CHECK(6, "want(5, L=11) → C3 (WIN<=2L-1), merge on boundary",
          s1 == JET_C3 && ctx.last_merge_tick % 12 == 0 && ctx.wants_ready == 2);

    /* WIN=12, L=3 → 12 <= 5? no → B */
    int s2 = geo_pipeline_want(&ctx, 7, 3);
    int exp_B = jet_merge(JET_B, 12, 7, 3);
    CHECK(6, "want(7, L=3) → B, merge sync-exact, ready=3",
          s2 == JET_B && ctx.last_merge_tick == (uint32_t)exp_B &&
          ctx.last_merge_tick % 12 == 0 && ctx.wants_ready == 3);

    /* ── T7: bridge coalesces ready wants into dispatch ── */
    geo_pipeline_init(&ctx);
    uint8_t data[64];
    memset(data, 0xA5, 64);
    for (int i = 0; i < 11; i++) {
        geo_pipeline_want(&ctx, (uint32_t)i, 3);   /* all → B */
        geo_pipeline_build_index(&ctx, (uint32_t)i, 64, data, 64);
        geo_pipeline_tick(&ctx);
    }
    CHECK(7, "11 wants + 11 ticks → 1 bridge, 11 coalesced, 0 ready",
          ctx.bridges == 1 && ctx.wants_coalesced == 11 &&
          ctx.wants_ready == 0);

    /* stats expose jet counters */
    GeoPipelineStats st = geo_pipeline_stats(&ctx);
    CHECK(7, "stats: wants_coalesced=11, jet_strat_last=B",
          st.wants_coalesced == 11 && st.jet_strat_last == JET_B);

    printf("\nRESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}
