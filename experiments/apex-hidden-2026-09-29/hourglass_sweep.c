/* hourglass_sweep.c — full-file gate-profile sweep (experiment).
 * Folds every 64 B chunk as 64->16->4->1 signed-gate hourglass (same fold as
 * hourglass_probe.c) and accumulates: T1 mismatches (expect 0), T2 zero-apex
 * mismatch rate (expect 48/64 = 75%), mean|gate| per level, apex==0 fraction,
 * and apex-mean per 1/256 file bin (variation across header/weight regions).
 * Usage: hourglass_sweep <gguf-path>  (streams, ~seconds per GB)
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int8_t  piv3[16], piv2[4], waist;
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

static int hbuck(int v) {
    int a = abs(v);
    return a == 0 ? 0 : a == 1 ? 1 : a <= 3 ? 2 : a <= 7 ? 3
         : a <= 15 ? 4 : a <= 31 ? 5 : a <= 63 ? 6 : a <= 127 ? 7 : 8;
}

#define NBIN 256
int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <file> [dump_n]\n", argv[0]); return 1; }
    int dump_n = argc > 2 ? atoi(argv[2]) : 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);

    static int8_t chunk[64], back[64];
    static unsigned char buf[1 << 20];
    long long nchunk = 0, t1bad = 0, t2bad_bytes = 0, apex_zero = 0;
    long double s1 = 0, s2 = 0, s3 = 0;
    long long bin_n[NBIN] = {0};
    long double bin_apex[NBIN] = {0};
    long long h1[9] = {0}, h2[9] = {0}, h3[9] = {0};
    size_t nr;
    long off = 0;
    while ((nr = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t o = 0; o + 64 <= nr; o += 64, off += 64) {
            memcpy(chunk, buf + o, 64);
            fold64(chunk);
            for (int i = 0; i < 48; i++) h1[hbuck(g1[i])]++;
            for (int i = 0; i < 12; i++) h2[hbuck(g2[i])]++;
            for (int i = 0; i < 3; i++) h3[hbuck(g3[i])]++;
            if (nchunk < dump_n) {
                printf("chunk %lld raw[0..15]:", nchunk);
                for (int i = 0; i < 16; i++) printf(" %d", chunk[i]);
                printf("\n  L1:");
                for (int i = 0; i < 48; i++) printf("%s%d", i % 12 ? " " : "\n    ", g1[i]);
                printf("\n  L2:");
                for (int i = 0; i < 12; i++) printf(" %d", g2[i]);
                printf("\n  apex: %d %d %d  waist=%d\n", g3[0], g3[1], g3[2], waist);
            }
            unfold64(back);
            int16_t s[3] = {g3[0], g3[1], g3[2]};
            long a3 = labs(g3[0]) + labs(g3[1]) + labs(g3[2]);
            if (a3 == 0) apex_zero++;
            for (int i = 0; i < 48; i++) s1 += labs(g1[i]);
            for (int i = 0; i < 12; i++) s2 += labs(g2[i]);
            s3 += a3;
            g3[0] = g3[1] = g3[2] = 0;
            unfold64(back);
            for (int i = 0; i < 64; i++) if (back[i] != chunk[i]) t2bad_bytes++;
            g3[0] = s[0]; g3[1] = s[1]; g3[2] = s[2];
            int b = (int)((off / 64) / ((fsz / 64 + NBIN - 1) / NBIN));
            if (b >= NBIN) b = NBIN - 1;
            bin_n[b]++;
            bin_apex[b] += a3;
            nchunk++;
        }
    }
    fclose(f);
    printf("file=%s size=%ld chunks=%lld\n", argv[1], fsz, nchunk);
    printf("T1_mismatch_bytes=%lld (expect 0)\n", t1bad);
    printf("T2_zeroApex_bad_bytes=%lld / %lld = %.2f%% (expect 75.00)\n",
           t2bad_bytes, nchunk * 64, 100.0 * t2bad_bytes / (nchunk * 64));
    printf("mean|gate|: L1=%.2f L2=%.2f apex=%.2f | apex==0 chunks=%.2f%%\n",
           (double)(s1 / (nchunk * 48)), (double)(s2 / (nchunk * 12)),
           (double)(s3 / (nchunk * 3)), 100.0 * apex_zero / nchunk);
    printf("apex-per-bin (bin: mean|apex|/chunk):\n");
    for (int b = 0; b < NBIN; b++)
        if (bin_n[b]) printf("  %3d: %.2f\n", b, (double)(bin_apex[b] / (bin_n[b] * 3)));
    printf("|gate| histogram buckets: 0 / 1 / 2-3 / 4-7 / 8-15 / 16-31 / 32-63 / 64-127 / 128+\n");
    printf("  L1:"); for (int i = 0; i < 9; i++) printf(" %lld", h1[i]); printf("\n");
    printf("  L2:"); for (int i = 0; i < 9; i++) printf(" %lld", h2[i]); printf("\n");
    printf("apex:"); for (int i = 0; i < 9; i++) printf(" %lld", h3[i]); printf("\n");
    return 0;
}
