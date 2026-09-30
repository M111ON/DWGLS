/* tools/beam_sector_cost.c — sector 2-9 ล้วน vs ปน sector 1: Lehmer cost เทียบกัน
 * ═══════════════════════════════════════════════════════════════════════════
 * block 32 ค่า → แยก pure289 (ทุกค่าอยู่ sectors 2-9) vs mixed (มี 0/1 ปน)
 * วัด Lehmer entropy ทั้งสองกลุ่ม + สัดส่วน ตัดสินว่าเว้น sector 1 คุ้มไหม
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_sector_cost.exe tools/beam_sector_cost.c -lm
 * RUN:   ./build/beam_sector_cost.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/gguf_box.h"

#define N 32u

static double entropy_of(const uint64_t *h, int sz, uint64_t total) {
    double e = 0.0;
    for (int i = 0; i < sz; i++) {
        if (!h[i]) continue;
        double p = (double)h[i] / (double)total;
        e -= p * log(p) * 1.4426950408889634;
    }
    return e;
}

static void argsort(const int8_t *v, uint8_t *order) {
    for (int i = 0; i < N; i++) order[i] = (uint8_t)i;
    for (int i = 1; i < (int)N; i++) {
        uint8_t key = order[i];
        int8_t kv = v[key];
        int j = i - 1;
        while (j >= 0 && (v[order[j]] > kv || (v[order[j]] == kv && order[j] > key))) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

static int lead9(int v) {
    int a = v < 0 ? -v : v;
    if (a == 0) return 0;
    while (a >= 10) a /= 10;
    return a;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf\n", argv[0]); return 2; }
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    static uint64_t hp[N][N], hm[N][N];
    memset(hp, 0, sizeof(hp));
    memset(hm, 0, sizeof(hm));
    uint64_t np = 0, nm = 0;

    for (unsigned t = 0; t < box.n_tensors; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / N;
        unsigned long long stride = (nb + 400000 - 1) / 400000;
        for (unsigned long long b = 0; b < nb; b += stride) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            int8_t v[N];
            int pure = 1;
            for (int k = 0; k < (int)N; k++) {
                v[k] = (int8_t)blk[k];
                int s = lead9(v[k]);
                if (s < 2) pure = 0;
            }
            uint8_t order[N];
            argsort(v, order);
            uint64_t (*h)[N] = pure ? hp : hm;
            uint64_t *cnt = pure ? &np : &nm;
            for (int i = 0; i < (int)N; i++) {
                unsigned d = 0;
                for (int j = i + 1; j < (int)N; j++)
                    if (order[j] < order[i]) d++;
                h[i][d]++;
            }
            (*cnt)++;
        }
    }
    double cp = 0, cm = 0, cu = 0;
    for (int i = 0; i < (int)N; i++) {
        int range = N - i;
        if (np) cp += entropy_of(hp[i], range, np);
        if (nm) cm += entropy_of(hm[i], range, nm);
        cu += log2((double)range);
    }
    printf("pure289 blocks: %llu  lehmer=%.1f b (uniform %.1f)\n", np, cp, cu);
    printf("mixed   blocks: %llu  lehmer=%.1f b (uniform %.1f)\n", nm, cm, cu);
    printf("pure share: %.1f%%\n", 100.0 * np / (np + nm));
    return 0;
}
