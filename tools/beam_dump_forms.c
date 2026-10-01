/* tools/beam_dump_forms.c — dump 4 forms (raw/sorted/resid1/cancelled) 1M values */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"
static int cmp_i8(const void *a, const void *b) {
    return (int)(*(const int8_t *)a) - (int)(*(const int8_t *)b);
}
int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: %s model.gguf outdir\n", argv[0]); return 2; }
    unsigned long long N = 1000000ull;
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) return 1;
    int8_t *v = malloc(N);
    unsigned long long n = 0;
    for (unsigned t = 0; t < box.n_tensors && n < N; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb && n < N; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32 && n < N; k++, n++) v[n] = (int8_t)blk[k];
        }
    }
    char p[512];
    FILE *f;
    sprintf(p, "%s/raw.bin", argv[2]);
    f = fopen(p, "wb");
    fwrite(v, 1, n, f);
    fclose(f);
    /* sorted */
    int8_t *s = malloc(n);
    memcpy(s, v, n);
    qsort(s, n, 1, cmp_i8);
    sprintf(p, "%s/sorted.bin", argv[2]);
    f = fopen(p, "wb");
    fwrite(s, 1, n, f);
    fclose(f);
    free(s);
    /* resid1: groups of 12, residuals vs mean (int8 wrap) */
    int8_t *r = malloc(n);
    for (unsigned long long g = 0; g < n / 12; g++) {
        long sum = 0;
        for (int k = 0; k < 12; k++) sum += v[g * 12 + k];
        int8_t mean = (int8_t)(sum / 12);
        for (int k = 0; k < 12; k++) r[g * 12 + k] = (int8_t)(v[g * 12 + k] - mean);
    }
    sprintf(p, "%s/resid1.bin", argv[2]);
    f = fopen(p, "wb");
    fwrite(r, 1, (n / 12) * 12, f);
    fclose(f);
    free(r);
    /* cancelled: greedy local pairs in 12-blocks -> 0, rest raw */
    int8_t *c = malloc(n);
    memcpy(c, v, n);
    for (unsigned long long g = 0; g < n / 12; g++) {
        int used[12] = {0};
        for (int i = 0; i < 12; i++) {
            if (used[i] || c[g * 12 + i] == 0) continue;
            for (int j = i + 1; j < 12; j++)
                if (!used[j] && c[g * 12 + j] == (int8_t)(-c[g * 12 + i])) {
                    used[i] = used[j] = 1;
                    c[g * 12 + i] = c[g * 12 + j] = 0;
                    break;
                }
        }
    }
    sprintf(p, "%s/cancelled.bin", argv[2]);
    f = fopen(p, "wb");
    fwrite(c, 1, n, f);
    fclose(f);
    free(c);
    free(v);
    printf("dumped %llu values x4 forms\n", n);
    return 0;
}
