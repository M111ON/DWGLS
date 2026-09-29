/* fold_n_compress.c — can a gate catch the order? (experiment)
 * Same n-fold as fold_n_probe, but gates are zigzag+varint-encoded and the
 * total is measured against raw input bytes. Pivot stays 1 raw byte.
 * Claim under test: real Q8_0 order (short-range, coherence ~10) compresses;
 * random must NOT (control: gate only pays when order exists).
 * Prints ratio vs n (2..12) on smooth / random / real-1MB.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int vlen(int v) {
    unsigned u = (unsigned)((v << 1) ^ (v >> 31)); /* zigzag */
    return u < 128 ? 1 : u < 16384 ? 2 : 3;
}

static int g_step = 1; /* element stride: 2 = deinterleave 16-bit streams */
static long *g_h = 0;  /* optional 9-bucket |gate| histogram sink */

static void hbucket(long *h, int g) {
    int a = abs(g);
    h[a == 0 ? 0 : a == 1 ? 1 : a <= 3 ? 2 : a <= 7 ? 3
      : a <= 15 ? 4 : a <= 31 ? 5 : a <= 63 ? 6 : a <= 127 ? 7 : 8]++;
}

static double ratio_n_par(const int8_t *d, int ncells, int n, int par) {
    long long enc = 0, raw = 0;
    int span = n * g_step;
    for (int i = par; i + span <= ncells; i += span) {
        enc += 1; /* pivot */
        for (int k = 1; k < n; k++) {
            int g = (int)d[i + k * g_step] - (int)d[i];
            enc += vlen(g);
            if (g_h) hbucket(g_h, g);
        }
        raw += n;
    }
    return raw ? (double)enc / raw : 0.0;
}

static double ratio_n(const int8_t *d, int ncells, int n) {
    return ratio_n_par(d, ncells, n, 0);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    long offset = argc > 2 ? atol(argv[2]) : 1048576L;
    if (argc > 3) g_step = atoi(argv[3]);
    static int8_t real[1 << 20], rnd[1 << 20], smooth[1 << 20];
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, offset, SEEK_SET);
    size_t got = fread(real, 1, sizeof real, f);
    fclose(f);
    if (got < 1024) { printf("short read %zu\n", got); return 1; }
    int nc = (int)(got & ~(size_t)63); /* trim to 64-multiple */
    printf("data=%s off=%ld bytes=%d\n", path, offset, nc);
    srand(7);
    for (int i = 0; i < (1 << 20); i++) rnd[i] = (int8_t)(rand() % 256 - 128);
    for (int i = 0; i < (1 << 20); i++) smooth[i] = (int8_t)((i % 256) - 128);
    printf("SPEC: ratio<1.0 wins; random must stay >=1 (control)\n");
    printf("%4s %8s %8s %8s | par0 par1 (stride streams)\n", "n", "smooth", "random", "real");
    for (int n = 2; n <= 12; n++)
        printf("%4d %8.3f %8.3f %8.3f | %.3f %.3f\n", n,
               ratio_n(smooth, sizeof smooth, n),
               ratio_n(rnd, sizeof rnd, n),
               ratio_n(real, nc, n),
               ratio_n_par(real, nc, n, 0), ratio_n_par(real, nc, n, 1));
    if (g_step == 2) {
        /* odd-stream (high bytes) gate histogram at n=4 -> entropy bound */
        static long hh[9] = {0};
        g_h = hh;
        ratio_n_par(real, nc, 4, 1);
        g_h = 0;
        printf("odd-stream n=4 gate-hist:");
        for (int b = 0; b < 9; b++) printf(" %ld", hh[b]);
        printf("\n");
    }
    return 0;
}
