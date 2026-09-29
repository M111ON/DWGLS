/* fold_n_probe.c — does group size matter? (experiment)
 * Single-level fold with group size n: pivot = v[0], gates = v[k]-v[0].
 * Exact for every n by construction (transpose keeps n values for n cells),
 * so the question is distributional: mean|gate| + histogram peak vs n.
 * Wall hypothesis would need a discontinuity at 7; noise theory predicts
 * flat across n on random/real (E|diff| is group-size-independent) and
 * rising-with-n on smooth (pivots spread). Same 1MB real window for all n.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static double run_n(const int8_t *d, int ncells, int n, long *hist) {
    long long s = 0, c = 0;
    for (int i = 0; i + n <= ncells; i += n) {
        for (int k = 1; k < n; k++) {
            int g = abs((int)d[i + k] - (int)d[i]);
            s += g; c++;
            int b = g == 0 ? 0 : g == 1 ? 1 : g <= 3 ? 2 : g <= 7 ? 3
                  : g <= 15 ? 4 : g <= 31 ? 5 : g <= 63 ? 6 : g <= 127 ? 7 : 8;
            hist[b]++;
        }
    }
    return c ? (double)s / c : 0.0;
}

int main(void) {
    static int8_t real[1 << 20], rnd[1 << 20], smooth[1 << 20];
    FILE *f = fopen("I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf", "rb");
    if (!f) { printf("cannot open model\n"); return 1; }
    fseek(f, 1048576L, SEEK_SET);
    if (fread(real, 1, sizeof real, f) != sizeof real) { printf("short read\n"); return 1; }
    fclose(f);
    srand(7);
    for (int i = 0; i < (1 << 20); i++) rnd[i] = (int8_t)(rand() % 256 - 128);
    for (int i = 0; i < (1 << 20); i++) smooth[i] = (int8_t)((i % 256) - 128);
    printf("SPEC: noise theory -> random/real FLAT vs n, smooth RISES; wall needs a break at 7\n");
    printf("%4s %8s %8s %8s | real-histogram(0/1/2-3/4-7/8-15/16-31/32-63/64-127/128+)\n", "n", "smooth", "random", "real");
    for (int n = 2; n <= 12; n++) {
        long hr[9] = {0};
        double ms = run_n(smooth, sizeof smooth, n, (long[9]){0});
        double mr = run_n(rnd, sizeof rnd, n, (long[9]){0});
        double md = run_n(real, sizeof real, n, hr);
        printf("%4d %8.2f %8.2f %8.2f |", n, ms, mr, md);
        for (int b = 0; b < 9; b++) printf(" %lld", hr[b]);
        printf("\n");
    }
    return 0;
}
