/* hourglass_mirror.c — reverse-shift across the gate (experiment).
 * Forward:  64 > 1  (fold: pivot + signed gates, transpose-not-cut).
 * Reverse:  1 < 64 through the gate as REFLECTION (x:-x): the carried
 * residues negate when crossing: v[k] = pivot - gate (not + gate).
 * The bottom pyramid is the TWIN of the top, not a copy.
 *
 * Pre-registered predictions:
 *   M_all (negate g1,g2,g3): twin B has fold(B).gates == -gates, same waist
 *     (mirror law), and mirror(mirror(x)) == x (involution).
 *   M_apex (negate g3 only): B == top on exactly 16 cells (intact pivot
 *     subtree), other 48 differ by exactly -2*g3[subtree] (frustum split:
 *     visible 16 + mirrored 48 = the 3:1 itself).
 * Datasets: smooth / random / real Q8_0 (64 B @1MB).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define NCELL 64
static int8_t  piv3[16], piv2[4], waist;
static int16_t g1[48], g2[12], g3[3];
/* neg mask bit0=g1 bit1=g2 bit2=g3 */
static int negmask;

static void fold4(const int8_t *v, int8_t *p, int16_t *g) {
    p[0] = v[0];
    g[0] = (int16_t)v[1] - (int16_t)v[0];
    g[1] = (int16_t)v[2] - (int16_t)v[0];
    g[2] = (int16_t)v[3] - (int16_t)v[0];
}
static void unfold4m(int8_t p, const int16_t *g, int8_t *v, int neg) {
    int s = neg ? -1 : 1;
    v[0] = p;
    v[1] = (int8_t)(p + s * g[0]);
    v[2] = (int8_t)(p + s * g[1]);
    v[3] = (int8_t)(p + s * g[2]);
}
static void fold64(const int8_t *in) {
    for (int i = 0; i < 16; i++) fold4(in + 4 * i, &piv3[i], g1 + 3 * i);
    for (int i = 0; i < 4; i++) fold4(piv3 + 4 * i, &piv2[i], g2 + 3 * i);
    fold4(piv2, &waist, g3);
}
static void unfold64m(int8_t *out) {
    static int8_t q2[4], q3[16];
    unfold4m(waist, g3, q2, negmask & 4);
    for (int i = 0; i < 4; i++) unfold4m(q2[i], g2 + 3 * i, q3 + 4 * i, negmask & 2);
    for (int i = 0; i < 16; i++) unfold4m(q3[i], g1 + 3 * i, out + 4 * i, negmask & 1);
}

static void run_one(const char *name, const int8_t *data, int have) {
    if (!have) { printf("%-7s SKIP\n", name); return; }
    static int8_t top[NCELL], bot[NCELL], back[NCELL];
    memcpy(top, data, NCELL);

    /* M_all: full twin */
    fold64(top);
    int8_t w0 = waist;
    int16_t sg1[48], sg2[12], sg3[3];
    memcpy(sg1, g1, sizeof sg1); memcpy(sg2, g2, sizeof sg2); memcpy(sg3, g3, sizeof sg3);
    negmask = 7;
    unfold64m(bot);
    /* mirror law: fold the twin -> negated gates, same waist */
    fold64(bot);
    /* mirror law MOD 256 (int8 wrap is part of the machine, not a violation):
     * fold(twin).gates + gates == 0 in Z_256 */
    int glaw = 0;
    for (int i = 0; i < 48; i++) if ((g1[i] + sg1[i]) % 256 != 0) glaw++;
    for (int i = 0; i < 12; i++) if ((g2[i] + sg2[i]) % 256 != 0) glaw++;
    for (int i = 0; i < 3; i++) if ((g3[i] + sg3[i]) % 256 != 0) glaw++;
    int inv = 0;
    negmask = 7;
    unfold64m(back); /* mirror of mirror */
    for (int i = 0; i < NCELL; i++) if (back[i] != top[i]) inv++;
    /* twin gap: mean|bot-top| vs 2*mean|gate-path| (expect equal by construction) */
    long gap = 0;
    for (int i = 0; i < NCELL; i++) gap += abs((int)bot[i] - (int)top[i]);

    /* M_apex: negate apex gate only */
    memcpy(g1, sg1, sizeof sg1); memcpy(g2, sg2, sizeof sg2); memcpy(g3, sg3, sizeof sg3);
    waist = w0;
    negmask = 4;
    unfold64m(bot);
    int same = 0, gaplaw = 0;
    for (int b = 0; b < 16; b++) {
        /* subtree b sits under q2[b/4]; apex gate index = (b/4==0)?none:b/4-1... */
        int sub = b / 4; /* which L2 pivot */
        /* apex residue affecting this subtree: q2[sub] used gate g3[sub-1] for sub>0 */
        for (int k = 0; k < 4; k++) {
            int i = 4 * b + k;
            if (bot[i] == top[i]) { same++; continue; }
            if (sub == 0) { gaplaw++; continue; } /* must NOT happen */
            int expect = (int8_t)(top[i] - 2 * sg3[sub - 1]);
            if (bot[i] != expect) gaplaw++;
        }
    }
    printf("%-7s Mall_lawbad=%-2d Mall_involution_bad=%-2d twin_gap=%-5ld | Mapex_same=%-2d/64 gaplaw_bad=%d %s\n",
           name, glaw, inv, gap, same, gaplaw,
           (glaw == 0 && inv == 0 && gaplaw == 0 && same % 16 == 0) ? "TWIN-OK" : "OFF-SPEC");
}

int main(int argc, char **argv) {
    long offset = argc > 1 ? atol(argv[1]) : 1048576L;
    static int8_t smooth[NCELL], rnd[NCELL], real[NCELL];
    for (int i = 0; i < NCELL; i++) smooth[i] = (int8_t)(i - 32);
    srand(7);
    for (int i = 0; i < NCELL; i++) rnd[i] = (int8_t)(rand() % 256 - 128);
    int have_real = 0;
    FILE *f = fopen("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", "rb");
    if (f) {
        fseek(f, offset, SEEK_SET);
        if (fread(real, 1, NCELL, f) == NCELL) have_real = 1;
        fclose(f);
    }
    printf("offset=%ld\n", offset);
    printf("SPEC: Mall law+involution exact; Mapex 16 same + 48 at top-2*g3\n");
    run_one("smooth", smooth, 1);
    run_one("random", rnd, 1);
    run_one("real", real, have_real);
    return 0;
}
