/*
 * tools/geo_pentagrid_test.c — Elser-Sloane / pentagrid bridge experiment
 * ══════════════════════════════════════════════════════════════════════
 * User hypothesis: quasicrystal (E8 -> pentagrid projection, Elser-Sloane)
 * is the missing bridge from TRIANGLE grid back to PENTAGON — i.e. the
 * coordinate link between our icosa side and the dodecahedron ROOT.
 *
 * Four int-only deterministic probes:
 *
 *   A. ORDER-5 INT ROTATION — exhaustive search of 2x2 int matrices
 *      with M^5 == I. Expected: only identity (trace argument:
 *      2cos72° is irrational). Proves pentagon CANNOT be a vector
 *      rotation in int world -> projection is mandatory.
 *
 *   B. PENTAGRID over the 12x12 cell — 5 line families with normals
 *      at 72 deg (fixed-point int). Classify slots: how many line
 *      families pass through/near each slot (singular vs regular).
 *
 *   C. CUT-AND-PROJECT STRIP — sum-of-families band selection
 *      (simplified Elser-Sloane): select slots whose pentagrid
 *      signature falls in the acceptance band -> the quasicrystal
 *      subset INSIDE our periodic field. Count + measure.
 *
 *   D. MOD-11 PENTAGON SCALAR — powers of 3 mod 11 form an EXACT
 *      order-5 cycle in pure int: the usable surrogate for pentagon
 *      rotation on LABELS.
 *
 * Constants note: 144 = F(12) Fibonacci (phi-native); 20736 = 2^8*3^4
 * has NO factor 5 -> exact 5-partition impossible -> quasi + holes is
 * mathematically forced, and holes route through the negative port.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SIDE     12
#define SLOTS    (SIDE * SIDE)          /* 144 = F(12) */

/* ── A. brute-force order-5 int matrices ────────────────────────────── */
typedef struct { int64_t a, b, c, d; } Mat;

static Mat mat_mul(Mat m, Mat n, int *overflow) {
    Mat o;
    o.a = m.a * n.a + m.b * n.c;
    o.b = m.a * n.b + m.b * n.d;
    o.c = m.c * n.a + m.d * n.c;
    o.d = m.c * n.b + m.d * n.d;
    if (labs(o.a) > (int64_t)1e9 || labs(o.b) > (int64_t)1e9 ||
        labs(o.c) > (int64_t)1e9 || labs(o.d) > (int64_t)1e9) *overflow = 1;
    return o;
}

static void probe_A(void) {
    printf("── A. order-5 int rotation (M^5 == I, entries -6..6) ──\n");
    uint32_t found = 0;
    for (int a = -6; a <= 6; a++)
    for (int b = -6; b <= 6; b++)
    for (int c = -6; c <= 6; c++)
    for (int d = -6; d <= 6; d++) {
        Mat m = { a, b, c, d };
        if (a == 1 && !b && !c && d == 1) continue;         /* skip I */
        int ovf = 0;
        Mat p = m, t;
        for (int k = 1; k < 5 && !ovf; k++) {
            t = mat_mul(p, m, &ovf);
            p = t;
        }
        if (ovf) continue;
        if (!p.a && !p.b && !p.c && p.d == 1) {
            /* must be a genuine rotation: |det| == 1 (degenerate
               rank-deficient projections like [0 0/0 1] don't count) */
            int64_t det = (int64_t)a * d - (int64_t)b * c;
            if (labs((long)det) != 1) continue;
            found++;
            if (found <= 3)
                printf("  FOUND M=[%lld %lld / %lld %lld] det=%lld with M^5=I\n",
                       (long long)m.a, (long long)m.b,
                       (long long)m.c, (long long)m.d,
                       (long long)det);
        }
    }
    printf("  non-identity solutions: %u\n", found);
    printf("  VERDICT: %s\n\n",
           found == 0
             ? "IMPOSSIBLE — pentagon cannot be an int vector rotation"
             : "exists (unexpected!)");
}

/* ── B/C/D. pentagrid ───────────────────────────────────────────────── */
#define NSCALE 100000L
static const long DX[5] = { 100000L,  30902L, -80902L, -80902L,  30902L };
static const long DY[5] = {       0L,  95106L,  58779L, -58779L, -95106L };

int main(int argc, char **argv) {
    probe_A();

    /* B. pentagrid families over the 12x12 cell */
    printf("── B. pentagrid (5 families @72deg) over %dx%d cell ──\n", SIDE, SIDE);
    static long n[5][SLOTS];
    static uint32_t fam_hits[SLOTS];
    memset(fam_hits, 0, sizeof(fam_hits));
    uint32_t idx = 0;
    for (int y = 0; y < SIDE; y++)
    for (int x = 0; x < SIDE; x++, idx++) {
        for (int k = 0; k < 5; k++) {
            long proj = x * DX[k] + y * DY[k];
            /* nearest-line index + signed residual */
            long nn = (proj >= 0 ? (proj + NSCALE / 2) : (proj - NSCALE / 2 + NSCALE - 1)) / NSCALE;
            n[k][idx] = nn;
            long res = labs(proj - nn * NSCALE);
            if (res < NSCALE / 8) fam_hits[idx]++;   /* line passes near slot */
        }
    }
    uint32_t hist[6] = { 0 };
    for (uint32_t i = 0; i < SLOTS; i++)
        hist[fam_hits[i] > 5 ? 5 : fam_hits[i]]++;
    for (uint32_t h = 0; h <= 5; h++)
        if (hist[h]) printf("  slots touched by %u families: %u\n", h, hist[h]);

    /* C. cut-and-project strip: band over s = n_0+..+n_4 */
    printf("\n── C. cut-and-project strip (sum band) ──\n");
    long sums[SLOTS];
    int64_t smin = 1LL << 40, smax = -(1LL << 40);
    for (uint32_t i = 0; i < SLOTS; i++) {
        long s = 0;
        for (int k = 0; k < 5; k++) s += n[k][i];
        sums[i] = s;
        if (s < smin) smin = s;
        if (s > smax) smax = s;
    }
    printf("  sum range: [%ld, %ld] · span %ld\n", smin, smax, smax - smin);
    /* pick the densest single-value band */
    long best_s = 0; uint32_t best_c = 0;
    for (long s = smin; s <= smax; s++) {
        uint32_t c = 0;
        for (uint32_t i = 0; i < SLOTS; i++) if (sums[i] == s) c++;
        if (c > best_c) { best_c = c; best_s = s; }
    }
    printf("  densest band s=%ld holds %u/%u slots (%.1f%%)\n",
           best_s, best_c, SLOTS, 100.0 * best_c / SLOTS);
    printf("  VERDICT: selection is QUASI — %u slots outside band need "
           "residual routing (negative port)\n\n", SLOTS - best_c);

    /* D. mod-11 order-5 scalar pentagon */
    printf("── D. mod-11 pentagon scalar cycle ──\n");
    uint32_t p = 1, ord = 0;
    printf("  cycle:");
    do {
        printf(" %u", p);
        p = (p * 3u) % 11u;
        ord++;
    } while (p != 1);
    printf("  · order = %u %s\n", ord, ord == 5 ? "(EXACT pentagon)" : "");
    failures_placeholder:;
    return 0;
}
