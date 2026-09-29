/* hourglass_probe.c — cellx4 -> cell1 |gate| cell1 -> cellx4 (experiment).
 *
 * Owner spec: normal equations subtract-and-destroy (cut = lossy). This one
 * transposes instead: the 3 non-pivot cells move across as SIGNED residues
 * (left side may go negative, the hidden -4 apex) and multiply back x4/level:
 *   1 -> 4 -> 16 -> 64  (unfold; base 8x8, visible top 2x2=4, apex = top gate)
 * Fold (per group of 4): pivot = v[0], gate = {v[1]-v[0], v[2]-v[0], v[3]-v[0]}
 *   (int16, negatives allowed — the "move across" instead of "subtract away").
 * Unfold: v[0] = pivot, v[k] = pivot + gate[k-1]. Exact by construction.
 * Levels on 64 cells: 64 -> 16 pivots -> 4 pivots -> 1 pivot (+48+12+3 gates).
 *
 * Tests:
 *   T1 roundtrip exact (memcmp==0) on smooth / random / real model bytes.
 *   T2 zero the apex gate (top 3 residues) -> unfold MUST break (mismatch > 0).
 *        (proves the hidden -4 carries load; without it = cut = lossy)
 *   T3 report mean|gate| per level per dataset (who carries what:
 *        small gates = fold wins; large gates = apex quietly holding noise).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define NCELL 64
/* level sizes: 64 -> 16 -> 4 -> 1 ; gates per level: 48, 12, 3 */
static int8_t  piv3[16], piv2[4];
static int8_t  waist;
static int16_t g1[48], g2[12], g3[3];

static void fold4(const int8_t *v, int8_t *p, int16_t *g) {
    p[0] = v[0];
    g[0] = (int16_t)v[1] - (int16_t)v[0];
    g[1] = (int16_t)v[2] - (int16_t)v[0];
    g[2] = (int16_t)v[3] - (int16_t)v[0];
}
static void unfold4(int8_t p, const int16_t *g, int8_t *v) {
    v[0] = p;
    v[1] = (int8_t)(p + g[0]);
    v[2] = (int8_t)(p + g[1]);
    v[3] = (int8_t)(p + g[2]);
}
static void fold64(const int8_t *in) {
    for (int i = 0; i < 16; i++) fold4(in + 4 * i, &piv3[i], g1 + 3 * i);
    for (int i = 0; i < 4; i++) fold4(piv3 + 4 * i, &piv2[i], g2 + 3 * i);
    fold4(piv2, &waist, g3);
}
static void unfold64(int8_t *out) {
    static int8_t q2[4], q3[16];
    unfold4(waist, g3, q2);
    for (int i = 0; i < 4; i++) unfold4(q2[i], g2 + 3 * i, q3 + 4 * i);
    for (int i = 0; i < 16; i++) unfold4(q3[i], g1 + 3 * i, out + 4 * i);
}
static double meang(const int16_t *g, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) s += abs(g[i]);
    return (double)s / n;
}
static void run_one(const char *name, const int8_t *data, int have) {
    if (!have) { printf("%-7s SKIP\n", name); return; }
    static int8_t cells[NCELL], back[NCELL];
    memcpy(cells, data, NCELL);
    fold64(cells);
    unfold64(back);
    int bad1 = 0;
    for (int i = 0; i < NCELL; i++) if (back[i] != cells[i]) bad1++;
    /* T2: kill the hidden apex, unfold again */
    int16_t save[3] = {g3[0], g3[1], g3[2]};
    g3[0] = g3[1] = g3[2] = 0;
    unfold64(back);
    int bad2 = 0;
    for (int i = 0; i < NCELL; i++) if (back[i] != cells[i]) bad2++;
    g3[0] = save[0]; g3[1] = save[1]; g3[2] = save[2];
    printf("%-7s T1_bad=%-3d T2_zeroApex_bad=%-3d |gate|: L1=%.1f L2=%.1f apex=%.1f %s\n",
           name, bad1, bad2, meang(g1, 48), meang(g2, 12), meang(g3, 3),
           (bad1 == 0 && bad2 > 0) ? "HOURGLASS-OK" : "OFF-SPEC");
}

int main(void) {
    static int8_t smooth[NCELL], rnd[NCELL], real[NCELL];
    for (int i = 0; i < NCELL; i++) smooth[i] = (int8_t)(i - 32);
    srand(7);
    for (int i = 0; i < NCELL; i++) rnd[i] = (int8_t)(rand() % 256 - 128);
    int have_real = 0;
    FILE *f = fopen("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", "rb");
    if (f) {
        fseek(f, 1048576L, SEEK_SET);
        if (fread(real, 1, NCELL, f) == NCELL) have_real = 1;
        fclose(f);
    }
    printf("SPEC: T1_bad==0 && T2_bad>0  (fold=transpose keeps, zero-apex=cut breaks)\n");
    run_one("smooth", smooth, 1);
    run_one("random", rnd, 1);
    run_one("real", real, have_real);
    return 0;
}
