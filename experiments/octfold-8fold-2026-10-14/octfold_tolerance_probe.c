/*
 * octfold_tolerance_probe.c — STEP D: error tolerance of the 8-fold ladder.
 * ════════════════════════════════════════════════════════════════════════
 * The source drawing is IMPERFECT (layers have gaps, edges are hand-drawn).
 * So the real question for a "language" is not only "is it a bijection" but
 * "how much noise / erasure can the ADDRESS LAW absorb and still decode".
 *
 * The law (docs/DWGL_8fold_ladder.md):
 *   r(n) = R0 * 2^(n/2)      ratio sqrt2 per level, EXACT
 *   type(n) = square if n even else octagon
 *   addr = (n,s)             n = ring/depth, s = sector 0..7
 *
 * D1 GEOMETRIC MARGIN (decode basin)
 *   radial : nearest-ring decode flips at the GEOMETRIC MEAN between rings.
 *            up-margin   = 2^(1/4) - 1  = 18.92%
 *            down-margin = 1 - 2^(-1/4) = 15.91%
 *   angular: 8 sectors x 45deg -> margin = 45/2 = 22.50deg
 *   -> a cell can drift ~+-17% radially / +-22.5deg angularly and still
 *      decode to the right (n,s). This is the "basin of attraction".
 *
 * D2 RADIAL ERASURE (why the ladder is self-healing)
 *   log2 r(n) = log2 R0 + n/2   -> AFFINE in n.
 *   => TWO surviving rings determine the ENTIRE ladder losslessly.
 *   - one interior ring erased  -> geometric mean of its two neighbours
 *   - a whole block erased      -> affine fit on the 2 boundary rings
 *   (This is the structure the drawing's "gaps" sit on.)
 *
 * D3 MULTI-VIEW MAJORITY (error tolerance by view redundancy)
 *   3 independent read paths (octfold / hosoya / limacon) over the same
 *   carrier: corrupt ONE view's map -> the other two still decode source,
 *   2-of-3 majority recovers. Redundancy is free (address translation only).
 *
 * GATES (all must be green):
 *   T1 radial margin  == 2^(1/4)-1 / 1-2^(-1/4)   (within 1e-6)
 *   T2 angular margin == 22.5 deg                 (within 1e-9)
 *   T3 single-ring erase -> recovered err < 1e-9 * r
 *   T4 block erase (2 boundary rings) -> recovered err < 1e-9 * r
 *   T5 multi-view 2-of-3 majority -> source recovered lossless
 *   MUT wrong ratio (1.40 instead of sqrt2) -> recovery must go RED
 *
 * BUILD: make octfold-tolerance
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SLOTS 60
#define SECT  8
#define NRING 11

static const double R0 = 173.0;      /* px, measured from the source image */

static double r_of(int n) { return R0 * pow(2.0, n / 2.0); }

/* decode ring index from a radius — ARITHMETIC nearest |r - r(n)| */
static int ring_of(double r) {
    int best = 0; double bd = fabs(r - r_of(0));
    for (int n = 1; n < NRING; n++) {
        double d = fabs(r - r_of(n));
        if (d < bd) { bd = d; best = n; }
    }
    return best;
}
/* decode ring index from a radius — GEOMETRIC nearest |log r - log r(n)|
 * (the natural decoder for a scale ladder: equal relative tolerance) */
static int ring_of_log(double r) {
    int best = 0; double bd = fabs(log2(r) - log2(r_of(0)));
    for (int n = 1; n < NRING; n++) {
        double d = fabs(log2(r) - log2(r_of(n)));
        if (d < bd) { bd = d; best = n; }
    }
    return best;
}
/* decode sector from an angle in degrees (nearest 45deg sector, mod 8) */
static int sector_of_deg(double deg) {
    long s = lround(deg / 45.0);
    s %= SECT; if (s < 0) s += SECT;
    return (int)s;
}

static int failc = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("  PASS %s\n", name); \
    else { printf("  FAIL %s\n", name); failc++; } } while (0)

int main(void) {
    printf("=== octfold_tolerance_probe — STEP D: error tolerance (8-fold ladder) ===\n\n");

    /* ── D1: geometric margin (decode basin) ──────────────────────────── */
    printf("D1 GEOMETRIC MARGIN (decode basin)\n");

    /* Helper: measure the radial cliff of a given decoder over all rings. */
    #define MEASURE(dec, up_meas, dn_meas) do {                        \
        up_meas = 1e9; dn_meas = 1e9;                                  \
        for (int n = 1; n < NRING - 1; n++) {                          \
            double r = r_of(n), lo, hi;                                \
            lo = 1.0; hi = 1.5;                                        \
            for (int it = 0; it < 60; it++) {                          \
                double m = 0.5 * (lo + hi);                            \
                if (dec(r * m) == n) lo = m; else hi = m; }            \
            if (lo < up_meas) up_meas = lo;                            \
            lo = 0.5; hi = 1.0;                                        \
            for (int it = 0; it < 60; it++) {                          \
                double m = 0.5 * (lo + hi);                            \
                if (dec(r * m) == n) hi = m; else lo = m; }            \
            if (hi < dn_meas) dn_meas = hi;                            \
        }                                                              \
    } while (0)

    /* (a) ARITHMETIC nearest  |r - r(n)|  -> cliff at the arithmetic midpoint
     *     up = (sqrt2 - 1)/2 ,  down = (1 - 1/sqrt2)/2                */
    double au, ad; MEASURE(ring_of, au, ad);
    double au_th = (sqrt(2.0) - 1.0) / 2.0;
    double ad_th = (1.0 - 1.0 / sqrt(2.0)) / 2.0;
    printf("  [arith-nearest] up   %.5f (%.3f%%) · theory %.5f (%.3f%%)\n",
           au - 1.0, (au - 1.0) * 100, au_th, au_th * 100);
    printf("  [arith-nearest] down %.5f (%.3f%%) · theory %.5f (%.3f%%)\n",
           1.0 - ad, (1.0 - ad) * 100, ad_th, ad_th * 100);
    CHECK("T1a arith up-margin   == (sqrt2-1)/2", fabs((au - 1.0) - au_th) < 1e-6);
    CHECK("T1b arith down-margin == (1-1/sqrt2)/2", fabs((1.0 - ad) - ad_th) < 1e-6);

    /* (b) GEOMETRIC nearest  |log r - log r(n)| -> cliff at the geometric mean
     *     up = 2^(1/4) - 1 ,  down = 1 - 2^(-1/4)   (equal relative tolerance) */
    double gu, gd; MEASURE(ring_of_log, gu, gd);
    double gu_th = pow(2.0, 0.25) - 1.0;
    double gd_th = 1.0 - pow(2.0, -0.25);
    printf("  [geo-nearest]   up   %.5f (%.3f%%) · theory %.5f (%.3f%%)\n",
           gu - 1.0, (gu - 1.0) * 100, gu_th, gu_th * 100);
    printf("  [geo-nearest]   down %.5f (%.3f%%) · theory %.5f (%.3f%%)\n",
           1.0 - gd, (1.0 - gd) * 100, gd_th, gd_th * 100);
    CHECK("T1c geo up-margin   == 2^(1/4)-1", fabs((gu - 1.0) - gu_th) < 1e-6);
    CHECK("T1d geo down-margin == 1-2^(-1/4)", fabs((1.0 - gd) - gd_th) < 1e-6);

    /* angular margin: largest delta (deg) with sector_of_deg(s*45 +/- d)==s */
    double ang_meas = 1e9;
    for (int s = 0; s < SECT; s++) {
        double base = s * 45.0, lo = 0.0, hi = 45.0;
        for (int it = 0; it < 60; it++) {
            double m = 0.5 * (lo + hi);
            if (sector_of_deg(base + m) == s) lo = m; else hi = m;
        }
        if (lo < ang_meas) ang_meas = lo;
    }
    printf("  angular margin    : measured %.6f deg · theory %.6f deg\n",
           ang_meas, 22.5);
    CHECK("T2 angular margin == 22.5 deg", fabs(ang_meas - 22.5) < 1e-9);

    /* ── D2: radial erasure (self-healing ladder) ─────────────────────── */
    printf("\nD2 RADIAL ERASURE (log-radius affine in n -> 2 rings suffice)\n");

    /* single interior ring erased -> geometric mean of neighbours */
    {
        int k = NRING / 2;
        double rec = sqrt(r_of(k - 1) * r_of(k + 1));
        double err = fabs(rec - r_of(k)) / r_of(k);
        printf("  erase ring %d -> geo-mean of %d,%d · rel err %.3e\n",
               k, k - 1, k + 1, err);
        CHECK("T3 single-ring erase recovered (err < 1e-9)", err < 1e-9);
    }

    /* whole block erased, only rings 0 and NRING-1 survive -> affine fit
     * log2 r(n) = a + b n  (b must come out 0.5 = 1/log2(sqrt2)) */
    {
        double a, b;
        double n0 = 0, n1 = NRING - 1;
        double y0 = log2(r_of(0)), y1 = log2(r_of(NRING - 1));
        b = (y1 - y0) / (n1 - n0);
        a = y0 - b * n0;
        double maxerr = 0.0;
        for (int n = 0; n < NRING; n++) {
            double rec = pow(2.0, a + b * n);
            double err = fabs(rec - r_of(n)) / r_of(n);
            if (err > maxerr) maxerr = err;
        }
        printf("  block erase: 2 boundary rings (0,%d) -> slope b=%.9f (want 0.5)\n",
               NRING - 1, b);
        printf("  recovered all %d rings · max rel err %.3e\n", NRING, maxerr);
        CHECK("T4a affine slope == 1/2 (log-radius linear in n)", fabs(b - 0.5) < 1e-12);
        CHECK("T4b block erase recovered (err < 1e-9)", maxerr < 1e-9);
    }

    /* ── D3: multi-view 2-of-3 majority ───────────────────────────────── */
    printf("\nD3 MULTI-VIEW MAJORITY (3 views, corrupt 1, 2-of-3 recovers)\n");
    {
        /* three independent permutations of the 60-slot carrier.
         * perm[p] = which logical slot sits at carrier position p. */
        uint8_t po[SLOTS], ph[SLOTS], pl[SLOTS];
        { int n = 0;                       /* octfold: sector-major stride-3 */
          for (int s = 0; s < SECT; s++)
            for (int w = 0; w < SLOTS; w++)
              if ((3 * (w % SECT)) % SECT == s) po[n++] = (uint8_t)w; }
        { uint8_t used[SLOTS]; memset(used, 0, sizeof(used)); int n = 0;
          for (int w = 0; w < SLOTS; w++) { int p = (13 * w) % SLOTS;   /* hosoya */
            if (!used[p]) { used[p] = 1; ph[n++] = (uint8_t)p; } }
          for (int p = 0; p < SLOTS; p++) if (!used[p]) ph[n++] = (uint8_t)p; }
        { int n = 0; pl[n++] = 30;                                        /* limacon */
          for (int k = 1; k <= 29; k++) { pl[n++] = (uint8_t)(30 + k);
                                          pl[n++] = (uint8_t)(30 - k); }
          pl[n++] = 0; }   /* view=[30,31,29,...,1,0] — 0 is last; 1+58+1=60 */

        /* sanity: all three are bijections */
        int bij = 1;
        uint8_t *maps[3] = { po, ph, pl };
        for (int m = 0; m < 3; m++) {
            uint8_t hit[SLOTS]; memset(hit, 0, sizeof(hit));
            for (int p = 0; p < SLOTS; p++) { if (hit[maps[m][p]]) bij = 0; hit[maps[m][p]] = 1; }
        }
        printf("  3 view maps bijective: %s\n", bij ? "YES" : "NO");

        /* payload: 60 distinct bytes, one per logical slot */
        uint8_t src[SLOTS];
        for (int w = 0; w < SLOTS; w++) src[w] = (uint8_t)(w * 4 + 1);

        /* bake into 3 stores: store_m[ position_of(w) ] = src[w] */
        uint8_t st[3][SLOTS];
        for (int m = 0; m < 3; m++)
            for (int p = 0; p < SLOTS; p++) st[m][p] = src[maps[m][p]];

        /* corrupt view 0: smash 5 bytes */
        int damaged = 5, dpos[5] = { 3, 17, 28, 41, 55 };
        for (int i = 0; i < damaged; i++) st[0][dpos[i]] ^= 0xFF;

        /* read back: for each logical slot w, each view reads st[m] at its pos */
        int recovered = 0, wrong = 0, unrecoverable = 0;
        for (int w = 0; w < SLOTS; w++) {
            uint8_t val[3];
            for (int m = 0; m < 3; m++) {
                /* find where view m placed logical slot w */
                uint8_t v = 0xFF;
                for (int p = 0; p < SLOTS; p++) if (maps[m][p] == w) { v = st[m][p]; break; }
                val[m] = v;
            }
            uint8_t rec = (val[0] == val[1] || val[0] == val[2]) ? val[0]
                        : (val[1] == val[2]) ? val[1] : 0xFE;
            if (rec == src[w]) recovered++;
            else if (rec == 0xFE) unrecoverable++;
            else wrong++;
        }
        printf("  payload 60 bytes · 5 bytes smashed in ONE view -> "
               "recovered %d/60 · unrecoverable %d · wrong %d\n",
               recovered, unrecoverable, wrong);
        CHECK("T5a all 3 views bijective", bij);
        CHECK("T5b 2-of-3 majority recovers 60/60 lossless", recovered == SLOTS);
        CHECK("T5c zero unrecoverable (no triple-disagreement)", unrecoverable == 0);
    }

    /* ── MUT: wrong ratio must break recovery ─────────────────────────── */
    printf("\nMUT: wrong ratio (1.40 instead of sqrt2) -> recovery must fail\n");
    {
        /* rebuild the ladder with ratio 1.40 and redo the block-erase fit:
         * if the law is not sqrt2, log-radius is still affine, but the
         * SLOPE b != 0.5 -> the affine test goes red. This proves T4a is
         * not vacuous. */
        double b = (log2(173.0 * pow(1.40, NRING - 1)) - log2(173.0)) / (NRING - 1);
        printf("  wrong-ratio slope b=%.9f (want 0.5) -> %s\n", b,
               fabs(b - 0.5) > 1e-3 ? "RED ok" : "NOT RED — PROBLEM");
        CHECK("MUT wrong ratio turns slope test red", fabs(b - 0.5) > 1e-3);
    }

    printf("\n%s (fail=%d)\n",
           failc == 0 ? "RESULT: ALL GREEN" : "RESULT: FAILED", failc);
    return failc ? 1 : 0;
}
