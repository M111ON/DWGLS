/* core/infra/jet_select.h — Jet phase strategy selector (production)
 * ═══════════════════════════════════════════════════════════════════════════
 * Owner policy (2026-09-24), proven in tests/test_jet_phase_align.c 21/21:
 *
 *   Given ribcage window WIN and jet travel L, pick merge strategy:
 *     L == 0            → JET_C1  (in-place, lat = 0)
 *     WIN <= 2L - 1     → JET_C3  (spin: wait for round completion, no travel)
 *     else              → JET_B   (phase-align: launch at boundary-L, arrival
 *                                  lands exactly on sync, full-round batch)
 *
 * Crossover rule (math): C3 loses ⟺ WIN > 2L-1 (avgC3 = (WIN+1)/2 grows
 * with WIN; avgC1 = L flat). Both directions observed in T18.
 *
 * Merge formulas are closed-form SUT, dual-derived against iterative REF
 * loops in the test (T2: exact equality across 5 strategies × 144 × 6 L).
 *
 * Sacred: WIN = FS_TICKS_PER_CYCLE = 12 (mod-12 ribcage).
 * Header-only, static inline, no malloc, integer-only.
 * ═══════════════════════════════════════════════════════════════════════════
 */
#ifndef JET_SELECT_H
#define JET_SELECT_H

#include <stdint.h>

/* Strategy codes — match test_jet_phase_align.c pick indices */
#define JET_C3  0   /* spin: merge at next WIN boundary, L ignored (no travel) */
#define JET_B   1   /* phase-align: launch so arrival == sync, batch kept       */
#define JET_C1  2   /* in-place: leave now, merge at t+L (L==0 → lat=0)        */

/* Owner policy selector (T19: 4×6 grid, oracle = policy table) */
static inline int jet_select(int win, int L)
{
    if (L == 0)        return JET_C1;
    if (win <= 2*L-1)  return JET_C3;
    return JET_B;
}

/* Closed-form merge times (SUT) — proven == REF iterative loops (T2) ── */
static inline int jet_merge(int strat, int win, int t, int L)
{
    switch (strat) {
    case JET_C3:
        return (t / win + 1) * win;                    /* round+1 boundary   */
    case JET_B: {
        int p = t % win;
        int w = (win - ((p + L) % win)) % win;         /* round arrival UP   */
        return t + w + L;
    }
    case JET_C1:
    default:
        return t + L;                                  /* distance-free      */
    }
}

/* Launch time = merge − travel (C3 has no travel: launch == merge) ── */
static inline int jet_launch(int strat, int win, int t, int L)
{
    int m = jet_merge(strat, win, t, L);
    if (strat == JET_C3) return m;
    return m - L;
}

#endif /* JET_SELECT_H */
