/*
 * invert_bridge_probe.c — Seeker C: pole inversion bridges the two sides
 *
 * CLASSIC FACT (checked here in ints):
 *   Cardioid  r = a(1+cos phi), cusp AT THE POLE
 *     --invert in circle radius S centered at pole-->  (r,phi)->(S^2/r,phi)
 *   gives the PARABOLA  rho = (S^2/a)/(1+cos phi),
 *   whose focus IS the pole:  rho + x == L,  L = S^2/a  (focus-directrix,
 *   eccentricity 1). Bounded shape <-> unbounded shape; inversion is a
 *   bijection on every ray => LOSSLESS bridge between the two sides.
 *
 * Oracles (AGENTS.md: independent of the construction path):
 *   O1 inversion roundtrip  S^2/(S^2/r) == r
 *      tolerance ANALYTIC: two chained divisions amplify by r^2/S^2
 *      => |back - r| <= 2*r^2/S^2 + 4
 *   O2 focus-directrix law  rho + x == L (closed-form L = S^2/a)
 *      tolerance covers r-floor amplified through S^2/r near the fence
 *   O3 cusp fence: exactly ONE slot has r==0 -> infinity;
 *      green: at slot M (cardioid). mutation: at slot 0 (limacon flip).
 *   FENCE: slots with r < R_FENCE are too close to the pole for fixed-
 *       point inversion (error ~ S^2/r^2 explodes) -> counted, printed,
 *       excluded. They ARE the hyperbolic direction made visible.
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define M      144                 /* angular slots on [0,pi] */
#define FXC    1000000LL           /* trig table scale */
#define S      10000LL             /* inversion radius */
#define A      19098               /* cardioid parameter (scale 1e-4) */
#define S2     (S * S)
#define R_FENCE 2000               /* min r for stable inversion */

static long long costab[M + 1], sintab[M + 1];

static void init_trigtab(void) {
    for (int j = 0; j <= M; j++) {
        costab[j] = llround(cos(M_PI * j / (double)M) * (double)FXC);
        sintab[j] = llround(sin(M_PI * j / (double)M) * (double)FXC);
    }
}

static long long rr(int j, int mutate) {
    if (mutate) return A * (FXC - costab[j]) / FXC;   /* limacon flip */
    return             A * (FXC + costab[j]) / FXC;   /* cardioid     */
}

/* returns fail count; mut==0 expects GREEN suite, mut==1 expects O2+O3 RED */
static int run(int mut) {
    long long L = S2 / A;                 /* closed-form directrix const */
    int fails = 0, cusps = 0, fenced = 0, cusp_pos = -1, o2_red = 0;
    long long o1_worst = 0, o2_worst = 0;

    printf("%s (L=%lld):\n", mut ? "mutated limacon (expect RED)"
                                 : "cardioid (expect GREEN)", L);
    for (int j = 0; j <= M; j++) {
        long long r = rr(j, mut);
        if (r == 0) { cusps++; cusp_pos = j; continue; }      /* -> infinity */
        if (r < R_FENCE) { fenced++; continue; }              /* pole fence  */

        long long rho = S2 / r;
        long long x   = rho * costab[j] / FXC;

        /* O1: roundtrip with analytic bound */
        long long back = S2 / rho;
        long long v1 = llabs(back - r);
        long long b1 = 2 * r * r / S2 + 4;
        if (v1 > b1) { fails++; printf("  FAIL O1 @j=%d (%lld vs %lld b=%lld)\n", j, back, r, b1); }
        if (v1 > o1_worst) o1_worst = v1;

        /* O2: parabola focus-directrix law */
        long long v2 = llabs(rho + x - L);
        if (v2 > 30) {
            if (!mut) { fails++; printf("  FAIL O2 @j=%d (rho+x=%lld vs L=%lld)\n", j, rho + x, L); }
            else o2_red++;                       /* violation = mutation caught */
        }
        if (v2 > o2_worst) o2_worst = v2;
    }

    /* O3: exactly one cusp, at the expected end */
    int want_pos = mut ? 0 : M;
    if (cusps != 1 || cusp_pos != want_pos) {
        fails++;
        printf("  FAIL O3 cusps=%d pos=%d (want 1 @ %d)\n", cusps, cusp_pos, want_pos);
    }
    if (mut && o2_red == 0) {
        fails++;
        printf("  MUTATION NOT CAUGHT: parabola law still holds\n");
    }
    printf("  cusp@%d ->infinity, fenced=%d (r<%d), "
           "O1 worst=%lld/%lld, O2 worst=%lld, O2 red=%d  %s\n",
           cusp_pos, fenced, R_FENCE, o1_worst, 2 * A * 2 * A / S2 + 4,
           o2_worst, o2_red,
           mut ? (o2_red > 0 ? "RED ok" : "NOT CAUGHT")
               : (fails == 0 ? "GREEN" : "FAIL"));
    return fails;
}

int main(void) {
    printf("invert_bridge_probe — cardioid ->(pole invert)-> parabola\n");
    printf("r=a(1+cos), a=%d, S=%lld, M=%d, fence r<%d\n\n",
           A, S, M, R_FENCE);

    init_trigtab();

    int f_green = run(0);
    int f_red   = run(1);      /* counts failures OF the mutation check */

    printf("TOTAL: %s  (green_fails=%d, mutation_check_fails=%d)\n",
           (f_green == 0 && f_red == 0) ? "ALL GREEN"
                                        : "HAS FAILS",
           f_green, f_red);
    return (f_green == 0 && f_red == 0) ? 0 : 1;
}
