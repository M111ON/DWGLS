/*
 * limacon_seek_probe.c — Seeker A: limaçon radial address space
 *
 * Family measured this session (fixed-frame fold of the linear 24 model):
 *   rho(phi) = (RC - RS) - RS*cos(phi)      [phi in [0, pi]]
 *   amplitude == RS exact, rho_min == RC - 2RS   (both verified numerically)
 *   RS == RC/2  ->  cardioid (cusp touches the pole)
 *
 * This probe turns that curve into an INT address space:
 *   slot i in [0,M] -> phi = pi*i/M -> addr(i) = round(KK * rho(phi))
 *   The [0,pi] branch is STRICTLY MONOTONE for every (RC,RS)
 *   => bijective => invertible by plain binary search
 *      (no hash, no lookup table — coordinate IS the address)
 *
 * Regimes read straight off the sign of addr(0):
 *   addr(0) > 0   limaçon (convex/dimpled)      RS < RC/2
 *   addr(0) = 0   cardioid (cusp at pole)       RS = RC/2
 *   addr(0) < 0   inner loop (pole crossed)     RS > RC/2
 *
 * Oracles (independent, per AGENTS.md test-integrity):
 *   F1 strict monotonicity           — math property of -cos on [0,pi]
 *   F3 endpoints  rho(0)=RC-2RS,
 *                 rho(pi)=RC         — closed form, integer arithmetic
 *   F4 midpoint   rho(pi/2)=RC-RS     — cos(pi/2)=0
 *   F6 mutation self-test             — flipped cos-sign MUST turn suite red
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#define M      144               /* angular slots on [0,pi] (=TESS_CELLS) */
#define KK     100000LL          /* addr quantum: 1e-5 radius units */

#define RC_X1E4 38197            /* RC = 24/(2*pi) = 3.81972..., x1e4 */

static int g_rs     = 19098;     /* default: closest int to RC/2 (see main) */
static int g_mutate = 0;         /* mutation switch for F6 */

/* forward map: slot -> radial address (the ONLY core logic) */
static long long fwd(int i) {
    double phi = M_PI * (double)i / (double)M;
    double r = (RC_X1E4 - g_rs) / 10000.0
             - (g_rs / 10000.0) * cos(phi);
    if (g_mutate) r += 2.0 * (g_rs / 10000.0) * cos(phi); /* MUTATION: sign flip */
    return (long long)llround(r * (double)KK);
}

/* inverse map: binary search on the monotone sequence (O(log M), no table) */
static int inv(long long target) {
    int lo = 0, hi = M;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (fwd(mid) < target) lo = mid + 1; else hi = mid;
    }
    return (lo <= M && fwd(lo) == target) ? lo : -1;
}

static int near_ll(long long a, long long b, long long tol) {
    return llabs(a - b) <= tol;
}

/* full suite at current (g_rs, g_mutate); returns fail count */
static int check_suite(void) {
    int fail = 0;
    int i;

    /* F1: strict monotone increasing on [0,M] */
    for (i = 0; i < M; i++) {
        if (fwd(i + 1) <= fwd(i)) {
            fail++; printf("  FAIL F1 monotone @i=%d (%lld -> %lld)\n",
                           i, fwd(i), fwd(i + 1));
            break;
        }
    }

    /* F2: inverse recovers every slot exactly (wiring proof) */
    for (i = 0; i <= M; i++) {
        int j = inv(fwd(i));
        if (j != i) {
            fail++; printf("  FAIL F2 inverse @i=%d got %d\n", i, j);
            break;
        }
    }

    /* F3: endpoints vs independent INTEGER closed-form oracle */
    long long emin = (long long)(RC_X1E4 - 2 * g_rs) * KK / 10000;
    long long emax = (long long)(RC_X1E4)           * KK / 10000;
    if (!near_ll(fwd(0), emin, 2)) {
        fail++; printf("  FAIL F3 rho(0)=%lld expected~%lld\n", fwd(0), emin);
    }
    if (!near_ll(fwd(M), emax, 2)) {
        fail++; printf("  FAIL F3 rho(pi)=%lld expected~%lld\n", fwd(M), emax);
    }

    /* F4: midpoint rho(pi/2) = RC - RS (cos(pi/2)=0, hand-derived) */
    long long emid = (long long)(RC_X1E4 - g_rs) * KK / 10000;
    if (!near_ll(fwd(M / 2), emid, 2)) {
        fail++; printf("  FAIL F4 rho(pi/2)=%lld expected~%lld\n",
                       fwd(M / 2), emid);
    }

    return fail;
}

int main(void) {
    printf("limacon_seek_probe — Seeker A: limacon radial address space\n");
    printf("rho(phi)=(RC-RS)-RS*cos(phi), M=%d slots, quantum=%lld/unit\n\n",
           M, KK);

    /* --- regime sweep across the whole limacon family ------------------ */
    int rss[]   = { 5000, 10000, 15000, 19098, 19099, 25000 };
    const char* reg[] = { "convex ", "convex ", "dimple ",
                          "CARDIOID", "loop(barely)", "inner-loop" };
    int nsweep = (int)(sizeof(rss) / sizeof(rss[0]));
    int sweep_fail = 0;

    printf("regime sweep (RC=%.4f, RC/2=%.4f):\n", RC_X1E4 / 1e4, RC_X1E4 / 2e4);
    printf("  RS     addr(0)      regime        suite\n");
    for (int s = 0; s < nsweep; s++) {
        g_rs = rss[s];
        int f = check_suite();
        sweep_fail += f;
        printf("  %-5d %-11lld  %-12s  %s\n",
               g_rs, fwd(0), reg[s], f == 0 ? "GREEN" : "FAIL");
    }

    /* --- default config: nearest-int cardioid -------------------------- */
    /* RC_X1E4 is odd => RC/2 not representable in int x1e4:
       g_rs=19098 -> addr(0)=+1 (1e-5 off the pole) — visible fence,
       fail-loud instead of silent rounding. Suite runs here:            */
    g_rs = 19098;
    printf("\ncardioid-nearest config RS=19098:\n");
    int green_fail = check_suite();
    printf("  inverse demo: addr(fwd(36)) -> slot %d  |  "
           "addr(fwd(108)) -> slot %d\n",
           inv(fwd(36)), inv(fwd(108)));

    /* --- F6 mutation self-test: suite MUST turn red -------------------- */
    g_mutate = 1;
    int mut_fail = check_suite();
    g_mutate = 0;
    printf("\nmutation check (mirrored angle): %s (%d fails caught)\n",
           mut_fail > 0 ? "RED ok" : "NOT CAUGHT — SUITE BROKEN", mut_fail);

    printf("\nTOTAL: %s  (sweep_fail=%d green_fail=%d mutation_caught=%s)\n",
           (sweep_fail + green_fail == 0 && mut_fail > 0) ? "ALL GREEN"
                                                          : "HAS FAILS",
           sweep_fail, green_fail, mut_fail > 0 ? "yes" : "NO");

    return (sweep_fail + green_fail == 0 && mut_fail > 0) ? 0 : 1;
}
