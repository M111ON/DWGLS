/* tools/beam_ninelane.c — 9-lane architecture (user design) end-to-end
 * ═══════════════════════════════════════════════════════════════════════════
 * แยก 9 lanes ตาม leading digit ตั้งแต่ต้น (zeros = lane พิเศษรวม sector-0)
 * cost = interleave map (lane label/position, entropy) + ค่าใน lane
 * (sorted-delta ต่อ lane) + lane counts (9×64b)
 * ตัดสินโครง 9-lane ตรงๆ รวมทุก cost
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_ninelane.exe tools/beam_ninelane.c -lm
 * RUN:   ./build/beam_ninelane.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/gguf_box.h"

static int lead9(int v) {
    int a = v < 0 ? -v : v;
    if (a == 0) return 0;
    while (a >= 10) a /= 10;
    return a;
}

static int cmp_i8(const void *a, const void *b) {
    return (int)(*(const int8_t *)a) - (int)(*(const int8_t *)b);
}

static double H(const unsigned long long *h, int sz, unsigned long long tot) {
    double e = 0;
    for (int i = 0; i < sz; i++) {
        if (!h[i]) continue;
        double p = (double)h[i] / (double)tot;
        e -= p * log(p) * 1.4426950408889634;
    }
    return e;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf\n", argv[0]); return 2; }
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    /* pass 1: lane counts + lane-label histogram (interleave map) */
    unsigned long long lane_cnt[10] = {0};
    unsigned long long n = 0;
    for (unsigned t = 0; t < box.n_tensors; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        unsigned long long stride = (nb + 3000000 - 1) / 3000000;
        for (unsigned long long b = 0; b < nb; b += stride) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32; k++) {
                lane_cnt[lead9((int8_t)blk[k])]++;
                n++;
            }
        }
    }
    /* interleave map = ลำดับ lane labels (10 สัญลักษณ์) */
    double h_lane = H(lane_cnt, 10, n);

    /* pass 2: sorted-delta ต่อ lane (sample ต่อ lane) */
    printf("lane: count : H(delta) : H(first)\n");
    double val_bits = 0;
    for (int s = 0; s < 10; s++) {
        if (!lane_cnt[s]) continue;
        /* เก็บ sample ของ lane นี้ (cap 2M) */
        unsigned long long cap = lane_cnt[s] > 2000000 ? 2000000 : lane_cnt[s];
        int8_t *sv = malloc(cap);
        unsigned long long got = 0;
        for (unsigned t = 0; t < box.n_tensors && got < cap; t++) {
            const GGUFBoxEntry *e = &box.entries[t];
            if (e->dtype != 8 || !e->data) continue;
            unsigned long long nb = e->n_elems / 32;
            unsigned long long stride = (nb + 500000 - 1) / 500000;
            for (unsigned long long b = 0; b < nb && got < cap; b += stride) {
                const uint8_t *blk = (const uint8_t *)e->data + b * 34;
                for (int k = 0; k < 32 && got < cap; k++) {
                    int8_t vv = (int8_t)blk[k];
                    if (lead9(vv) == s) sv[got++] = vv;
                }
            }
        }
        /* sort sample */
        qsort(sv, got, 1, cmp_i8);
        unsigned long long hd[512] = {0}, hf[256] = {0};
        if (got) hf[(uint8_t)sv[0]]++;
        for (unsigned long long i = 1; i < got; i++)
            hd[(int)sv[i] - (int)sv[i - 1] + 255]++;
        double h_d = got > 1 ? H(hd, 512, got - 1) : 0;
        double h_f = got ? H(hf, 256, got) : 0;
        /* ต่อ value ใน lane: (H(first) + (m-1)*H(delta))/m, m = lane size */
        double m = (double)lane_cnt[s];
        double per_val = (h_f + (m - 1.0) * h_d) / m;
        printf("  %d: %llu : %.3f : %.3f (per-val %.3f)\n", s, lane_cnt[s], h_d, h_f, per_val);
        val_bits += lane_cnt[s] * per_val;
        free(sv);
    }
    double per_val_all = val_bits / n;
    double total = h_lane + per_val_all;
    printf("interleave map: %.3f b/val + values: %.3f b/val = %.3f b/val (raw 8.0) ratio=%.3fx\n",
           h_lane, per_val_all, total, total / 8.0);
    return 0;
}
