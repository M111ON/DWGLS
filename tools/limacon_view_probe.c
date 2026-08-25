/*
 * limacon_view_probe.c — 9th language candidate: LIMACON on RID carrier
 * ════════════════════════════════════════════════════════════════════════
 * Origin: linear-24 fold discovery (Aug 25). Void boundary of aa-gon
 * stamps around a circle = limacon family rho(phi)=(RC-RS)-RS*cos(phi);
 * cardioid regime (RS=RC/2) has its CUSP at the pole.
 *
 * Grammar (int-only, one line):
 *   Cardioid r=a(1+cos phi). Slot w -> phi_w = 6 deg * w (60 slots).
 *   Radius rank from the cusp (w=30, r=0) ascending, MIRRORED across the
 *   symmetry axis — pair {30-k, 30+k} shares radius (cos even), read
 *   right branch first:
 *
 *     view_lim[p] = p==0 ? 30 : (p odd ? 30+(p+1)/2 : 30-p/2)
 *     = [30, 31,29, 32,28, ..., 59,1, 0]
 *
 * Gates (LANGUAGES.md G1-G5 here, G6 = gguf_roundtrip wiring next):
 *   G1 bijection sweep (bitset)
 *   G2 inverse exists, deterministic roundtrip BOTH directions
 *   G3 grammar is pure int (no float in mapping)
 *   G4 independent oracles: hand-computed head/tail + monotone non-decr
 *      radius vs integer cos LUT + mirror-pair equality
 *   G5 mutation (branch swap) must turn suite red
 */
#define __USE_MINGW_ANSI_STDIO 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SLOTS 60
#define HALF  30              /* cos(pi*j/30) half-period table */

static const char *g_mut_note = "";

/* THE grammar — pure int. p=0 cusp, p=59 rim (mirror of itself). */
static int lim_at(int p, int mutate) {
    if (p == 0) return 30;
    if (p == SLOTS - 1) return 0;
    if (mutate)                               /* MUTATION: pair offset off-by-one */
        return p & 1 ? 30 + p / 2 : 30 - p / 2;
    return p & 1 ? 30 + (p + 1) / 2 : 30 - p / 2;
}

static void build(int8_t *v, int mutate) {
    for (int p = 0; p < SLOTS; p++) v[p] = (int8_t)lim_at(p, mutate);
}

/* integer cos LUT: C[j]=round(1e6*cos(pi*j/30)), j=0..30 (static geometry) */
static long long C[HALF + 1];
static void init_costab(void) {
    for (int j = 0; j <= HALF; j++)
        C[j] = llround(cos(M_PI * (double)j / (double)HALF) * 1e6);
}
/* cos(pi*w/30) for any int w, folded into [0,30] */
static long long cos6w(int w) {
    w %= 2 * HALF; if (w < 0) w += 2 * HALF;
    return C[w <= HALF ? w : 2 * HALF - w];
}

static int failc = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("  PASS %s\n", name); \
    else { printf("  FAIL %s\n", name); failc++; } } while (0)

int main(void) {
    printf("=== limacon_view_probe — 9th language: radial-ascent-from-cusp ===\n");
    init_costab();

    int8_t v[SLOTS], inv[SLOTS];
    build(v, 0);

    /* head/tail print */
    printf("  view: [%d,%d,%d,%d,%d ... %d,%d,%d]\n",
           v[0], v[1], v[2], v[3], v[4], v[57], v[58], v[59]);

    /* G1: bijection */
    {
        uint8_t hit[SLOTS]; memset(hit, 0, sizeof(hit));
        int ok = 1;
        for (int p = 0; p < SLOTS; p++) {
            if (hit[(uint8_t)v[p]]) ok = 0;
            hit[(uint8_t)v[p]] = 1;
        }
        CHECK("G1 bijection sweep 60/60", ok);
    }

    /* G2: inverse roundtrip both directions */
    {
        memset(inv, -1, sizeof(inv));
        for (int p = 0; p < SLOTS; p++) inv[v[p]] = (int8_t)p;
        int ok = 1;
        for (int p = 0; p < SLOTS; p++) if (inv[v[p]] != p) ok = 0;
        for (int w = 0; w < SLOTS; w++) if (v[inv[w]] != w) ok = 0;
        CHECK("G2 inverse deterministic (fwd o inv = id = inv o fwd)", ok);
    }

    /* G3: grammar int-only — structurally true; assert domain/range ints */
    {
        int ok = 1;
        for (int p = 0; p < SLOTS; p++)
            if (v[p] < 0 || v[p] >= SLOTS) ok = 0;
        CHECK("G3 pure-int grammar (domain/range in [0,60))", ok);
    }

    /* G4a: hand-computed head and tail */
    CHECK("G4a hand-computed head [30,31,29,32,28]",
          v[0] == 30 && v[1] == 31 && v[2] == 29 && v[3] == 32 && v[4] == 28);
    CHECK("G4b hand-computed tail [...,59,1,0]",
          v[57] == 59 && v[58] == 1 && v[59] == 0);

    /* G4c: radius monotone non-decreasing along reading order (int LUT) */
    {
        int ok = 1;
        for (int p = 1; p < SLOTS; p++)
            if (cos6w(v[p]) < cos6w(v[p - 1])) ok = 0;
        CHECK("G4c monotone radius from cusp (vs int cos LUT)", ok);
    }

    /* G4d: mirror pairs share radius EXACTLY */
    {
        int ok = 1;
        for (int k = 1; k <= 29; k++)
            if (cos6w(30 - k) != cos6w(30 + k)) ok = 0;
        CHECK("G4d mirror pairs {30-k,30+k} equal radius", ok);
    }

    /* G5: mutation must go red */
    {
        int8_t m[SLOTS];
        build(m, 1); g_mut_note = "branch-swap";
        uint8_t hit[SLOTS]; memset(hit, 0, sizeof(hit));
        int collide = 0, mono_bad = 0;
        for (int p = 0; p < SLOTS; p++) {
            if (hit[(uint8_t)m[p]]) collide = 1;
            hit[(uint8_t)m[p]] = 1;
            if (p && cos6w(m[p]) < cos6w(m[p - 1])) mono_bad = 1;
        }
        CHECK("G5 mutation (branch-swap) turns suite red", collide || mono_bad);
    }

    printf("\n%s (fail=%d)\n", failc == 0 ? "RESULT: ALL GREEN" : "RESULT: FAILED",
           failc);
    (void)g_mut_note;
    return failc ? 1 : 0;
}
