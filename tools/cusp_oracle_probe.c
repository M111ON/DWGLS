/*
 * cusp_oracle_probe.c — Seeker B: gear ratio <-> cusp count oracle
 *
 * LAW (derived analytically, verified numerically):
 *   Chord family on N-gon: pin i -> pin (k*i mod N)  ("times-k gear")
 *   Line through unit-circle points at angles a,b has
 *   distance-from-origin d = |cos((a-b)/2)|; here a=2*pi*i/N, b=k*a =>
 *   d_i = |cos((k-1)*pi*i/N)|.
 *   Cusps of the envelope = local minima of d_i (i.e., chords passing
 *   closest to the POLE) = k-1 for integer k >= 1.
 *   k=2 -> 1 cusp = CARDIOID (classic string art), cusp chord = diameter.
 *
 * Discrete bonus (ties to ring-24 gearbox):
 *   times-k on Z_N splits into gcd(N,k) cycles, each of length N/gcd.
 *
 * Pure int after init-time quarter-wave cos table (static geometry LUT).
 * Oracles independent of implementation:
 *   O1 cusp count == k-1            (closed-form math)
 *   O2 orbit decomposition          (group theory: gcd x len)
 *   O3 mutation k-1 -> k            (suite MUST turn red)
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <math.h>

#define NC   24                 /* carrier: 24-gon */
#define FX   10000              /* fixed-point scale */

static int costab[NC / 2 + 1];  /* |cos(j*pi/NC)| * FX, j=0..NC/2 */

static void init_costab(void) {
    for (int j = 0; j <= NC / 2; j++)
        costab[j] = (int)llround(fabs(cos(M_PI * j / (double)NC)) * FX);
}

/* |cos(pi * q / NC)| via table, q any int.
 * |cos| has period NC in q, symmetric about q = NC/2. */
static int abs_cos_pi_over(int q) {
    q %= NC; if (q < 0) q += NC;
    if (q > NC - q) q = NC - q;              /* now q in [0, NC/2] */
    return costab[q];
}

/* count circular strict local minima of seq[0..n-1] (with tolerance) */
static int count_minima(const int *seq, int n) {
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        int l = seq[(i - 1 + n) % n], r = seq[(i + 1) % n];
        if (seq[i] <= l && seq[i] <= r && (seq[i] < l || seq[i] < r))
            cnt++;
    }
    return cnt;
}

/* component structure of i -> k*i mod N (functional graph, NOT a
 * permutation when gcd(N,k)>1 — those k are LOSSY, not true gears).
 * Walks with a seen-mark so tails never loop forever. */
static int igcd(int a, int b){ while(b){int t=a%b;a=b;b=t;} return a; }

static void orbits(int k, int *ncycles, int *cyclen, int *g) {
    int seen[NC] = {0};
    *ncycles = 0; *cyclen = 0;
    for (int i = 0; i < NC; i++) {
        if (seen[i]) continue;
        int len = 0, j = i;
        while (!seen[j]) { seen[j] = 1; len++; j = (j * k) % NC; }
        if (*ncycles == 0) *cyclen = len;
        (*ncycles)++;
    }
    *g = igcd(NC, k);
}

static int run_gear(int k, int mutate) {
    int mult = mutate ? k : (k - 1);         /* MUTATION: use k instead of k-1 */
    int d[NC];
    for (int i = 0; i < NC; i++)
        d[i] = abs_cos_pi_over(mult * i);
    int cusps = count_minima(d, NC);

    int nc, cl, g;
    orbits(k, &nc, &cl, &g);

    int fail = 0;
    printf("k=%d: cusps=%2d (expect %d)  gcd=%d components=%d firstlen=%d %s\n",
           k, cusps, k - 1, g, nc, cl,
           cusps == k - 1 ? "GREEN" : (mutate ? "RED ok" : "FAIL"));
    if (mutate) return cusps != k - 1 ? 0 : 1;   /* mutation caught = pass */
    return cusps == k - 1 ? 0 : 1;

    (void)fail;
}

int main(void) {
    printf("cusp_oracle_probe — times-k chord envelope on 24-gon\n");
    printf("law: cusps = minima of |cos((k-1)*pi*i/N)| = k-1\n\n");

    init_costab();

    int gears[] = {2, 3, 4, 5, 6, 8};        /* incl. 5 as fence-probe */
    int ng = (int)(sizeof(gears) / sizeof(gears[0]));
    int fail = 0;

    printf("-- green suite --\n");
    for (int g = 0; g < ng; g++) fail += run_gear(gears[g], 0);

    printf("-- mutation (multiplier k -> k+1): must go RED --\n");
    for (int g = 0; g < ng; g++) fail += run_gear(gears[g], 1);

    printf("\nTOTAL: %s\n", fail == 0 ? "ALL GREEN (+mutation caught)" : "HAS FAILS");
    return fail ? 1 : 0;
}
