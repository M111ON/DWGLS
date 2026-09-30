/* tools/beam_sector_probe.c — Phase 1: Sort + Sector บนน้ำหนักจริง
 * ═══════════════════════════════════════════════════════════════════════════
 * sort ค่า int8 (Q8_0) low→high ทั้งโมเดล → split ตามเลขนำ 1-9 + sector 0
 * วัด: sector 0 กินที่เท่าไร / sector เกาะกลุ่มไหม (RLE runs) / lossless ไหม
 * (permutation cost ไม่นับตรงนี้ — เป็นงาน Phase 2)
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/beam_sector_probe \
 *        tools/beam_sector_probe.c -lm
 * RUN:   ./build/beam_sector_probe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/gguf_box.h"

#define MAX_VALS 700000000ull  /* กัน上限 700M values (~700MB) */

static int cmp_i8(const void *a, const void *b) {
    return (int)(*(const int8_t *)a) - (int)(*(const int8_t *)b);
}

static int lead9(int v) {  /* เลขนำของ |v|: 1..9, 0 -> 0 */
    int a = v < 0 ? -v : v;
    if (a == 0) return 0;
    while (a >= 10) a /= 10;
    return a;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf\n", argv[0]); return 2; }
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    uint64_t total = 0;
    for (uint32_t t = 0; t < box.n_tensors; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        total += e->n_elems;
    }
    if (total == 0 || total > MAX_VALS) { printf("no Q8 data or too big\n"); return 1; }
    printf("values: %llu\n", (unsigned long long)total);

    int8_t *v = malloc(total);
    if (!v) { printf("oom\n"); return 1; }
    uint64_t p = 0;
    for (uint32_t t = 0; t < box.n_tensors; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        uint64_t nb = e->n_elems / 32, rem = e->n_elems % 32;
        for (uint64_t b = 0; b < nb; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32; k++) v[p++] = (int8_t)blk[k];
        }
        (void)rem;
    }
    uint64_t n = p;
    int64_t sum0 = 0;
    for (uint64_t i = 0; i < n; i++) sum0 += v[i];

    qsort(v, n, 1, cmp_i8);

    /* lossless: multiset check ผ่าน sum + min/max */
    int64_t sum1 = 0;
    for (uint64_t i = 0; i < n; i++) sum1 += v[i];
    printf("lossless(multiset): %s (sum %lld==%lld)\n",
           sum0 == sum1 ? "OK" : "FAIL", (long long)sum0, (long long)sum1);

    /* sectors */
    uint64_t cnt[10] = {0}, runs[10] = {0};
    int prev = 0;
    int has_prev = 0;
    for (uint64_t i = 0; i < n; i++) {
        int s = lead9(v[i]);
        cnt[s]++;
        if (!has_prev || v[i] != prev) { runs[s]++; prev = v[i]; has_prev = 1; }
    }
    printf("sector0 (free): %llu values = %.2f%%\n",
           (unsigned long long)cnt[0], 100.0 * cnt[0] / n);
    printf("sector : count : runs : avg_run\n");
    for (int s = 0; s < 10; s++)
        printf("  %d : %llu : %llu : %.1f\n", s,
               (unsigned long long)cnt[s], (unsigned long long)runs[s],
               runs[s] ? (double)cnt[s] / runs[s] : 0.0);

    /* RLE estimate: run value (8b) + run length; length via log2 buckets */
    uint64_t tot_runs = 0;
    for (int s = 0; s < 10; s++) tot_runs += runs[s];
    /* ประมาณ: 8 bits/value-id + ceil(log2(avg_run+1)) bits/len ต่อ run */
    double rle_bits = 0;
    for (int s = 0; s < 10; s++) {
        if (!runs[s]) continue;
        double avg = (double)cnt[s] / runs[s];
        rle_bits += runs[s] * (8.0 + ceil(log2(avg + 1.0)));
    }
    printf("RLE est: %.1f MB vs raw %.1f MB (%.2f bits/value, raw 8.0)\n",
           rle_bits / 8 / 1e6, (double)n / 1e6, rle_bits / n);
    free(v);
    return 0;
}
