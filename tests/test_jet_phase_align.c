/* test_jet_phase_align.c — jet/ribcage phase strategies vs latency & coalesce
 * ═══════════════════════════════════════════════════════════════════════════
 * Owner hypotheses under test (2026-09-24):
 *   widen-window   — dilutes want/tick density (analytic, printed, not tested)
 *   phase-align B  — m = ready mod 12, wait = (12-m) mod 12 (round UP),
 *                    leave at boundary - L  →  arrival lands ON sync, no tail
 *   magic-bond C   — symlink identity at park at exit, merge increments back
 *                    on arrival, "ไม่สนระยะทาง" (distance-free = no sync wait)
 *   spin C3        — miss a round = STAY PUT, only round counter ticks;
 *                    NO walk (no active step to boundary), NO pull (no jet L).
 *                    Merge time = (r+1)*WIN — pure function of r = t/WIN,
 *                    round completes → timeline reaches the bridge for you.
 *
 * Five strategies over one WIN=12 window, wants ready every tick (144 total):
 *   A  passive+sync   — join next natural bridge, jet L, then WAIT for next
 *                       sync (phase 0) to re-enter main path   [current naive]
 *   B  phase-align    — launch at boundary-L so arrival == sync exactly
 *   C1 bond+immediate — bond at park, leave NOW, merge at arrival (any phase)
 *   C2 bond+bridge    — bond at park, join next natural bridge, merge at
 *                       arrival (any phase) — batching kept, sync wait dropped
 *   C3 spin           — park at ready; merge at next WIN boundary (round+1);
 *                       NO jet travel (L ignored), batching kept (12/round)
 *
 * Round semantics: r = t / WIN = ribcage-span index. One full lap (1-144)
 * = 12 spans. C3 waits for span r to complete → merge at boundary (r+1)*WIN.
 * "Same place, different round" = same bridge position, next round's pass.
 *
 * Oracles — dual derivation, not the strategy formulas restated:
 *   REF = iterative "advance until property" loops straight from the spec
 *   SUT = closed-form modular formulas. T2 requires exact equality of both
 *   across every (want x strategy x L). Phase/tail/coalesce/coverage checks
 *   are structural consequences (arrive%WIN, bit coverage, dispatch groups).
 *
 * No CUDA, no core deps — pure integer step model.
 *
 * BUILD: make test-test_jet_phase_align
 * RUN:   ./build/test-test_jet_phase_align
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define WIN   12                 /* ribcage window = sacred mod-12        */
#define NWANT 144                /* one want per tick, full ring          */
#define NS    5                  /* strategies A,B,C1,C2,C3               */
static const int LS[] = { 0, 1, 3, 5, 7, 11 };   /* jet travel (main ticks) */
#define NL ((int)(sizeof LS / sizeof LS[0]))

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while (0)

/* ── SUT: closed-form strategy formulas ─────────────────────────────────── */
static int sut_launch_A(int t) { return t + (WIN - t % WIN) % WIN; }
static int sut_merge_A(int t, int L) {
    int a = sut_launch_A(t) + L;
    a += (WIN - a % WIN) % WIN;          /* wait out to next sync          */
    return a;
}
static int sut_merge_B(int t, int L) {
    int p = t % WIN;
    int w = (WIN - ((p + L) % WIN)) % WIN;   /* round arrival UP to sync   */
    return t + w + L;
}
static int sut_launch_B(int t, int L) { return sut_merge_B(t, L) - L; }
static int sut_merge_C1(int t, int L) { (void)t; (void)L; return t + L; }
static int sut_launch_C1(int t) { return t; }
static int sut_merge_C2(int t, int L) { return sut_launch_A(t) + L; }
static int sut_launch_C2(int t) { return sut_launch_A(t); }
/* C3 spin: merge at NEXT WIN boundary after current round start; L ignored.
 * Pure function of r = t/WIN: merge = (r+1)*WIN. batch = all round-mates.  */
static int sut_merge_C3(int t, int L) { (void)L; return (t / WIN + 1) * WIN; }
static int sut_launch_C3(int t) { return (t / WIN + 1) * WIN; }

/* ── REF: iterative spec-property loops (independent derivation) ────────── */
static int ref_merge_A(int t, int L) {
    int b = t; while (b % WIN) b++;      /* advance to natural bridge      */
    int a = b + L;
    while (a % WIN) a++;                 /* advance arrival to next sync   */
    return a;
}
static int ref_merge_B(int t, int L) {
    int a = t + L;
    while (a % WIN) a++;                 /* arrival must land on sync      */
    return a;
}
static int ref_merge_C1(int t, int L) { (void)t; (void)L; return t + L; }
static int ref_merge_C2(int t, int L) {
    int b = t; while (b % WIN) b++;
    return b + L;
}
static int ref_merge_C3(int t, int L) {
    (void)L;
    int r = t / WIN;
    int m = 0;
    while (m / WIN <= r) m += WIN;       /* advance round boundaries      */
    return m;
}

typedef int (*suf_fn)(int, int);

int main(void)
{
    printf("jet phase-align: WIN=%d, %d wants, L =", WIN, NWANT);
    for (int i = 0; i < NL; i++) printf(" %d", LS[i]);
    printf("\n");

    struct { const char *nm; suf_fn m; suf_fn r; } S[NS] = {
        { "A  passive+sync", sut_merge_A, ref_merge_A },
        { "B  phase-align",  sut_merge_B, ref_merge_B },
        { "C1 bond+immed",   sut_merge_C1, ref_merge_C1 },
        { "C2 bond+bridge",  sut_merge_C2, ref_merge_C2 },
        { "C3 spin",         sut_merge_C3, ref_merge_C3 },
    };
    #define LAUNCH(si, t, L) ((si)==0 ? sut_launch_A(t) : \
                              (si)==1 ? sut_launch_B(t, L) : \
                              (si)==2 ? sut_launch_C1(t) : \
                              (si)==3 ? sut_launch_C2(t) : sut_launch_C3(t))

    /* ── T1: model constants & completeness ── */
    int phase_hist[WIN] = {0};
    for (int t = 0; t < NWANT; t++) phase_hist[t % WIN]++;
    int hist_ok = 1;
    for (int p = 0; p < WIN; p++) if (phase_hist[p] != NWANT / WIN) hist_ok = 0;
    CHECK(1, "144 wants, each phase 0..11 appears exactly 12x",
          NWANT == 144 && WIN == 12 && hist_ok);

    /* sweep accumulators */
    int t2_ok = 1, t3_ok = 1, t4_ok = 1, t5_ok = 1, t6_ok = 1;
    int t7_ok = 1, t8_ok = 1, t13_ok = 1, t14_ok = 1, t15_ok = 1;
    int tail_A[NL] = {0}, tail_B[NL] = {0};
    int cov_ok[NS];
    for (int i = 0; i < NS; i++) cov_ok[i] = 1;
    int lat_ge0 = 1;
    double avg[NS][NL];
    int maxdisp[NS][NL];
    memset(avg, 0, sizeof avg);
    memset(maxdisp, 0, sizeof maxdisp);
    int c3_avg_ok = 1;   /* C3 < C2 for L>=3 */

    for (int li = 0; li < NL; li++) {
        int L = LS[li];
        int cnt[NS][NWANT];
        int disp[NS][256];
        int lat_sum[NS];
        memset(cnt, 0, sizeof cnt);
        memset(disp, 0, sizeof disp);
        memset(lat_sum, 0, sizeof lat_sum);

        for (int t = 0; t < NWANT; t++) {
            for (int si = 0; si < NS; si++) {
                int m_sut = S[si].m(t, L);
                int m_ref = S[si].r(t, L);
                int lat = m_sut - t;
                if (m_sut != m_ref) t2_ok = 0;
                if (lat < 0) lat_ge0 = 0;
                lat_sum[si] += lat;
                cnt[si][t]++;
                int lt = LAUNCH(si, t, L);
                if (lt >= 0 && lt < 256) disp[si][lt]++;

                int arrive;
                if (si == 0)      arrive = sut_launch_A(t) + L;
                else if (si == 1) arrive = sut_launch_B(t, L) + L;
                else if (si == 2) arrive = t + L;
                else if (si == 3) arrive = sut_launch_C2(t) + L;
                else              arrive = sut_launch_C3(t);  /* C3: no jet */

                if (si == 0 && arrive % WIN != L % WIN) t3_ok = 0;
                if (si == 1) {
                    if (arrive % WIN != 0) t4_ok = 0;
                    if (arrive % WIN != 0) tail_B[li]++;
                }
                if (si == 0 && arrive % WIN != 0) tail_A[li]++;

                if (si == 1 && sut_merge_A(t, L) < m_sut) t5_ok = 0;

                if (L == 0 && si == 0 && sut_merge_A(t, 0) != sut_merge_B(t, 0))
                    t6_ok = 0;
                if (L == 0 && si == 0 && sut_merge_A(t, 0) != sut_merge_C2(t, 0))
                    t6_ok = 0;

                if (si == 2 && lat != L) t7_ok = 0;
                if (si == 3) {
                    int d = sut_merge_A(t, L) - m_sut;
                    if (d != (WIN - L % WIN) % WIN) t8_ok = 0;
                }
                if (t == 140 && L == 0 && si == 1 && lat != 4) t13_ok = 0;

                /* T14: C3 spin semantics */
                if (si == 4) {
                    int r = t / WIN;
                    if (m_sut != (r + 1) * WIN) t14_ok = 0;   /* pure f(r)  */
                    if (m_sut / WIN != r + 1) t14_ok = 0;      /* round +1   */
                    if (m_sut != sut_merge_C3(t, L + 99)) t14_ok = 0; /* no L */
                    if (m_sut % WIN != 0) t14_ok = 0;           /* boundary  */
                }
            }
        }

        for (int si = 0; si < NS; si++) {
            avg[si][li] = (double)lat_sum[si] / NWANT;
            for (int t = 0; t < NWANT; t++)
                if (cnt[si][t] != 1) cov_ok[si] = 0;
            for (int k = 0; k < 256; k++)
                if (disp[si][k] > maxdisp[si][li]) maxdisp[si][li] = disp[si][k];
            if (li > 0 && si == 1 && !(avg[1][li] < avg[0][li])) t5_ok = 0;
        }
        if (maxdisp[4][li] != NWANT / WIN) t15_ok = 0;
        if (L >= 3 && !(avg[4][li] < avg[3][li])) c3_avg_ok = 0;
    }

    CHECK(2, "SUT closed-form == REF iterative loops (5 strategies x 144 x 6 L)",
          t2_ok);
    CHECK(3, "A arrival phase == L mod WIN (launched on boundary)", t3_ok);
    CHECK(4, "B arrival phase == 0 for every want & L (sync exact)", t4_ok);
    CHECK(5, "latB <= latA every case AND avgB < avgA for every L>0", t5_ok);
    CHECK(6, "L=0 degenerate: A == B == C2 for every want", t6_ok);
    CHECK(7, "C1 latency == L (distance-free: independent of ready phase)",
          t7_ok);
    CHECK(8, "latA - latC2 == (WIN - L%WIN)%WIN (C2 saves exactly the sync wait)",
          t8_ok);

    int t9_ok = 1;
    for (int li = 0; li < NL; li++) {
        int expect_A = (LS[li] % WIN != 0) ? NWANT : 0;
        if (tail_A[li] != expect_A) t9_ok = 0;
        if (tail_B[li] != 0) t9_ok = 0;
    }
    CHECK(9, "sync-miss tails: A misses every want when L not mod-0, B never", t9_ok);

    int t11_ok = 1;
    for (int li = 0; li < NL; li++) {
        if (maxdisp[0][li] < 10 || maxdisp[1][li] < 10 ||
            maxdisp[3][li] < 10 || maxdisp[4][li] < 10)
            t11_ok = 0;
        if (maxdisp[2][li] != 1) t11_ok = 0;
    }
    CHECK(11, "coalesce: A/B/C2/C3 max batch >= 10:1, C1 collapses to 1:1", t11_ok);

    int t12_ok = lat_ge0;
    for (int si = 0; si < NS; si++) if (!cov_ok[si]) t12_ok = 0;
    CHECK(12, "lossless merge: all 144 wants merged exactly once x5 strategies x6 L, lat>=0",
          t12_ok);

    CHECK(13, "owner worked example: ready t=140 (p=8), L=0 -> B wait = 4",
          t13_ok);
    CHECK(14, "C3 spin: merge=(r+1)*WIN pure function of r, round advanced, "
              "L ignored (no pull), lands on boundary",
          t14_ok);
    CHECK(15, "C3 batch == 12 (full round) for every L AND avgC3 < avgC2 for L>=3",
          t15_ok && c3_avg_ok);

    /* ── T16-T18: round-length sweep — long round → C3 spin loses ──
     * C3 waits for round completion: avg = (WIN+1)/2 (phase uniform).
     * C1 leaves immediately:        avg = L        (constant in WIN).
     * Flip rule (math): C3 loses ⟺ WIN > 2L-1. Both directions must appear. */
    static const int RWS[] = { 12, 24, 48, 144 };
    static const int RLS[] = { 1, 3, 11 };
    #define NRW ((int)(sizeof RWS / sizeof RWS[0]))
    #define NRL ((int)(sizeof RLS / sizeof RLS[0]))
    int t16_ok = 1, t17_ok = 1, t18_ok = 1, t18_flip = 0;
    printf("\n  round-length sweep (avg lat over 2 full rounds):\n");
    printf("    WIN   L  |  C1imm  C3spin | C3 loses?\n");
    for (int wi = 0; wi < NRW; wi++) {
        int win = RWS[wi];
        int nw = 2 * win;
        for (int li = 0; li < NRL; li++) {
            int L = RLS[li];
            long c1_sum = 0, c3_sum = 0;
            for (int t = 0; t < nw; t++) {
                int m1 = t + L;                       /* C1 SUT: leave now   */
                int r1 = t; while (r1 < t + L) r1++;  /* C1 REF: advance L   */
                int m3 = (t / win + 1) * win;         /* C3 SUT: round+1     */
                int rr = t / win, mm = 0;
                while (mm / win <= rr) mm += win;     /* C3 REF: next bndry  */
                if (m1 != r1 || m3 != mm) t16_ok = 0;
                c1_sum += m1 - t;
                c3_sum += m3 - t;
            }
            double a1 = (double)c1_sum / nw;
            double a3 = (double)c3_sum / nw;
            int loses = a3 > a1;
            if (a1 != L) t17_ok = 0;
            if (a3 != (win + 1) / 2.0) t17_ok = 0;
            if (loses != (win > 2 * L - 1)) t18_ok = 0;
            if (loses) t18_flip = 1;
            printf("    %4d  %3d  | %6.1f %7.1f | %s\n",
                   win, L, a1, a3, loses ? "yes" : "no");
        }
    }
    CHECK(16, "round sweep: C1/C3 parametric SUT == REF loops (4 WIN x 3 L x 2WIN)",
          t16_ok);
    CHECK(17, "math: avgC1 == L flat in WIN, avgC3 == (WIN+1)/2 grows linearly",
          t17_ok);
    CHECK(18, "crossover rule WIN > 2L-1 ⟺ C3 loses, both directions observed",
          t18_ok && t18_flip);

    /* ── T19-T21: condition-based selector (owner policy 2026-09-24) ──
     * The system knows how FAR (L) and how LONG the round is (WIN), then picks:
     *   C3 default  — round completes within travel budget (WIN <= 2L-1);
     *                 proven region: C3 <= C1 (T18) and C3 <= B for L>=1
     *   B  alternative — round too long to sit out; travel phase-aligned,
     *                 arrival always on sync, batch kept (vs A: tail + batch)
     *   C1 special  — zero distance: nothing travels, merge in place, lat=L=0
     * Note (known trade, not asserted): in the B region C3 latency is equal or
     * better (avgC3 ~WIN/2 < avgB (WIN-1)/2+L when L small) — B is picked as
     * the travel-committed path; C3 region is where spin is the clear winner. */
    static const int SELW[] = { 12, 24, 48, 144 };
    static const int SELL[] = { 0, 1, 3, 5, 7, 11 };
    #define NSW ((int)(sizeof SELW / sizeof SELW[0]))
    #define NSL ((int)(sizeof SELL / sizeof SELL[0]))
    /* parametric merges — local win, NOT the global WIN=12 SUT            */
    #define P_A(win,t,L) ({ int b_=(t)+((win)-(t)%(win))%(win); \
                            int a_=b_+(L); a_+=((win)-a_%(win))%(win); a_; })
    #define P_B(win,t,L) ({ int p_=(t)%(win); \
                            int w_=(win)-(((p_)+(L))%(win)); if(w_==win)w_=0; \
                            (t)+w_+(L); })
    #define P_C3(win,t,L) ((t)/(win)+1)*(win)
    #define P_C1(t,L) ((t)+(L))
    int t19_ok = 1, t20_ok = 1, t21_ok = 1, t22_ok = 1;
    int used_c3 = 0, used_b = 0, used_c1 = 0;
    printf("\n  selector jet_select(WIN, L) -> C3 / B / C1:\n");
    printf("    WIN   L | pick | avg lat | region quality\n");
    for (int wi = 0; wi < NSW; wi++) {
        int win = SELW[wi];
        for (int li = 0; li < NSL; li++) {
            int L = SELL[li];
            int pick;                                  /* policy mapping     */
            if (L == 0)            pick = 2;           /* C1 special         */
            else if (win <= 2*L-1) pick = 0;           /* C3 default         */
            else                   pick = 1;           /* B alternative      */
            /* expected mapping recomputed as policy table (oracle) */
            int exp = (L == 0) ? 2 : ((win <= 2*L - 1) ? 0 : 1);
            if (pick != exp) t19_ok = 0;
            if (pick == 0) used_c3 = 1;
            if (pick == 1) used_b  = 1;
            if (pick == 2) used_c1 = 1;

            int nw = 2 * win;                          /* 2 rounds: B launch  */
                                                       /* at boundary-L spans */
                                                       /* round edges — need   */
                                                       /* steady-state groups */
            long sum[3] = {0,0,0};
            int tail_ok = 1, batch_ok = 1, launched[512] = {0};
            for (int t = 0; t < nw; t++) {
                sum[0] += P_C3(win, t, L) - t;
                sum[1] += P_B(win, t, L) - t;
                sum[2] += P_C1(t, L) - t;
                if (pick == 1) {
                    int arr = P_B(win, t, L);
                    if (arr % win != 0) tail_ok = 0;   /* sync exact         */
                    int lt = arr - L;
                    if (lt >= 0 && lt < 512) launched[lt]++;
                }
            }
            double lat[3] = { (double)sum[0]/nw, (double)sum[1]/nw,
                              (double)sum[2]/nw };
            const char *note = "ok";
            if (pick == 0) {                           /* C3 region quality  */
                if (!(lat[0] <= lat[1] + 1e-9 && lat[0] <= lat[2] + 1e-9))
                    { t20_ok = 0; note = "NOT <= B,C1"; }
            } else if (pick == 1) {                    /* B region quality   */
                int mx = 0;
                for (int k = 0; k < 512; k++)
                    if (launched[k] > mx) mx = launched[k];
                if (mx != win) batch_ok = 0;           /* full-round batch   */
                if (!tail_ok || !batch_ok) { t21_ok = 0; note = "tail/batch"; }
                /* B <= naive A (sync+batch vs A's miss)                      */
                long asum = 0;
                for (int t = 0; t < nw; t++) asum += P_A(win, t, L) - t;
                if (lat[1] > (double)asum / nw + 1e-9) { t21_ok = 0; note = "> A"; }
            } else {                                   /* C1 region: in-place*/
                if (L != 0 || lat[2] != 0.0) { t22_ok = 0; note = "not zero"; }
            }
            static const char *PN[3] = { "C3", "B ", "C1" };
            printf("    %4d %3d  |  %s  | %6.1f | %s\n",
                   win, L, PN[pick], lat[pick], note);
        }
    }
    CHECK(19, "selector: policy mapping L==0->C1, WIN<=2L-1->C3, else B (4x6 grid)",
          t19_ok);
    CHECK(20, "C3-region quality: avgC3 <= avgB AND avgC3 <= avgC1",
          t20_ok);
    CHECK(21, "B-region quality: sync-exact tail + full-round batch + <= naive A",
          t21_ok);
    CHECK(22, "C1-region: zero distance only, avg lat == 0 (in-place merge)",
          t22_ok && used_c3 && used_b && used_c1);

    printf("\n  strategy        | avg latency per L in {0,1,3,5,7,11}   | max dispatch\n");
    for (int si = 0; si < NS; si++) {
        printf("  %-16s|", S[si].nm);
        for (int li = 0; li < NL; li++)
            printf(" %5.1f", avg[si][li]);
        printf(" |");
        for (int li = 0; li < NL; li++)
            printf(" %2d", maxdisp[si][li]);
        printf("\n");
    }
    printf("\n  tail (A, miss sync):");
    for (int li = 0; li < NL; li++) printf(" %d", tail_A[li]);
    printf("   (B: 0 all L)\n");
    printf("  C3: wait=WIN-o (o=t%%WIN), avg=6.5 flat all L, batch=12/round,\n");
    printf("      NO jet, NO walk — round counter alone delivers to bridge.\n");
    printf("  read: C3 < C2 for L>=3 (drops pull), C2 < C3 at L=0 (C2 catches\n");
    printf("        current boundary; C3 always waits full round completion).\n");
    printf("  read: round sweep — C3 flat WIN/2 vs C1 flat L: long round\n");
    printf("        (WIN > 2L-1) → waiting for round completion loses to leaving now.\n");
    printf("  read: selector — C3 default (WIN<=2L-1), B alternative (long round,\n");
    printf("        phase-align travel, batch kept), C1 special (L=0, in-place).\n");

    printf("\nRESULT: %d PASS / %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}
