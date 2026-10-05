/* tools/beam_fold12.c — สำรวจ ÷12 สี่รอบ: 20736 → 1728 → 144 → 12 → 1
 * ต่อรอบ: group 12 ค่า → เก็บ mean (int) + residuals 11 ตัว
 * วัด: residual range/entropy ต่อรอบ (หด = มี structure) + roundtrip exact
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/gguf_box.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf [factor=12] [rounds=0|minsize=1]\n", argv[0]); return 2; }
    int F = argc > 2 ? atoi(argv[2]) : 12;
    int RND = argc > 3 ? atoi(argv[3]) : 0;
    long long SKIP = argc > 4 ? atoll(argv[4]) : 0;
    int NEG = argc > 5 ? atoi(argv[5]) : 0; /* NEG=1: r = mean - v (swap sides) */
    int MINSZ = 1;
    if (F < 2 || F > 256) { printf("bad factor\n"); return 2; }
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }
    static int vals[20736];
    int n = 0;
    for (unsigned t = 0; t < box.n_tensors && n < 20736; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb && n < 20736; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32 && n < 20736; k++) {
                if (SKIP > 0) { SKIP--; continue; }
                vals[n++] = (int8_t)blk[k];
            }
        }
    }
    printf("n=%d\n", n);
    static int cur[20736], nxt[20736];
    memcpy(cur, vals, sizeof(vals));
    int m = n;
    printf("factor=%d minsize=%d\n", F, MINSZ);
    for (int round = 0; (RND > 0 ? round < RND : m / F >= MINSZ); round++) {
        int groups = m / F;
        long long ressum = 0;
        int resmax = 0;
        unsigned long long hist[513] = {0};
        unsigned long long localpairs = 0;
        for (int g = 0; g < groups; g++) {
            long s = 0;
            int gh[256] = {0};
            for (int k = 0; k < F; k++) { s += cur[g * F + k]; gh[cur[g * F + k] & 255]++; }
            unsigned gp = 0;
            for (int vv = 1; vv <= 127; vv++) {
                int a = gh[vv], b = gh[256 - vv];
                gp += (unsigned)(a < b ? a : b);
            }
            localpairs += gp;
            int mean = (int)((s + (s >= 0 ? F/2 : -F/2)) / F);
            nxt[g] = mean;
            for (int k = 0; k < F; k++) {
                int r = NEG ? mean - cur[g * F + k] : cur[g * F + k] - mean;
                int a = r < 0 ? -r : r;
                ressum += a;
                if (a > resmax) resmax = a;
                hist[r + 256]++;
            }
        }
        double e = 0;
        for (int i = 0; i < 513; i++) {
            if (!hist[i]) continue;
            double p = (double)hist[i] / (double)(groups * F);
            e -= p * log(p) * 1.4426950408889634;
        }
        m = groups;
        printf("round %d: size %d, resid H=%.3f b, mean|res|=%lld max|res|=%d, localpairs=%.2f%%\n",
               round + 1, m, e, ressum / (groups * F), resmax,
               200.0 * localpairs / (groups * F));
        memcpy(cur, nxt, (size_t)m * sizeof(int));
    }
    printf("top: %d\n", cur[0]);
    return 0;
}
