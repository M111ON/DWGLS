/* tools/beam_cost_probe.c — "ทำ cost ให้ less" วัดด้วยของจริง
 * ═══════════════════════════════════════════════════════════════════════════
 * beam_codec ล้มเพราะต้องเก็บ permutation (sort กู้ order กลับไม่ได้ฟรี)
 * คำถาม: cost ของ permutation จริงๆ เท่าไหร่ เมื่อบีบด้วย entropy?
 *
 *   กำไรจาก sort : H(sorted-delta) < H(raw)  (sorted มี structure)
 *   cost ของ perm : Lehmer digits — ถ้า data มีโครงสร้าง → digits ไม่ uniform
 *                   → cost < log₂(32!) = 114.3 bits (เพดาน uniform)
 *   net = 32×H(raw) − (31×H(sorted-delta) + H(first) + H(perm))
 *
 * ถ้า net > 0 → beam/sort approach พอเป็นไปได้ (ทำ cost ให้ less จริง)
 * ถ้า net ≈ 0  → "cost ไม่คุ้ม" ยืนยันด้วยตัวเลข
 *
 * BUILD: gcc -O2 -Wall -I. -Icore -Icore/infra -o build/beam_cost_probe \
 *        tools/beam_cost_probe.c -lm
 * RUN:   ./build/beam_cost_probe [model.gguf ...]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../core/gguf_box.h"

#define MAX_SAMPLE  400000u
#define N           32u

static double entropy_of(const uint64_t *h, int sz, uint64_t total) {
    double e = 0.0;
    for (int i = 0; i < sz; i++) {
        if (!h[i]) continue;
        double p = (double)h[i] / (double)total;
        e -= p * log(p) * 1.4426950408889634;
    }
    return e;
}

/* argsort (stable) ของ int8[32] — คืน index เรียงตามค่า */
static void argsort(const int8_t *v, uint8_t *order) {
    for (int i = 0; i < N; i++) order[i] = (uint8_t)i;
    /* insertion sort (stable) — n=32 เล็ก */
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

/* Lehmer digits ของ permutation order[] (ขนาด n) — digit i ∈ [0, n−1−i] */
static void lehmer_digits(const uint8_t *order, uint16_t *digits) {
    for (int i = 0; i < (int)N; i++) {
        uint16_t d = 0;
        for (int j = i + 1; j < (int)N; j++)
            if (order[j] < order[i]) d++;
        digits[i] = d;   /* ∈ [0, N−1−i] */
    }
}

static void probe_model(const char *path) {
    GGUFBox box;
    printf("\n═ %s ═\n", path);
    if (gguf_box_open(&box, path) != 0) { printf("  (skip)\n"); return; }

    uint64_t h_delta[512] = {0};          /* sorted deltas: 0..511 (diff int8s) */
    uint64_t h_first[256] = {0};
    uint64_t h_lehmer[N][N] = {{0}};      /* digit i: range N−i */
    uint64_t nblocks = 0;

    for (uint32_t t = 0; t < box.n_tensors; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        uint64_t nb = e->n_elems / N;
        if (nb == 0) continue;
        uint64_t stride = (nb + MAX_SAMPLE - 1) / MAX_SAMPLE;
        uint64_t sb = nb / stride;
        for (uint64_t b = 0; b < sb; b++) {
            const uint8_t *blk = e->data + (uint64_t)(b * stride) * 34;
            int8_t v[N];
            for (int k = 0; k < (int)N; k++) v[k] = (int8_t)blk[k];

            uint8_t order[N];
            argsort(v, order);

            /* sorted values → delta stream */
            int8_t sv[N];
            for (int k = 0; k < (int)N; k++) sv[k] = v[order[k]];
            h_first[(uint8_t)sv[0]]++;
            for (int k = 1; k < (int)N; k++)
                h_delta[(int)sv[k] - (int)sv[k - 1] + 255]++;

            uint16_t dig[N];
            lehmer_digits(order, dig);
            for (int k = 0; k < (int)N; k++) h_lehmer[k][dig[k]]++;
            nblocks++;
        }
    }
    if (nblocks == 0) { printf("  (no Q8 blocks)\n"); return; }

    double h_raw = 7.65;                  /* วัดไว้แล้วทั้ง 4 โมเดล (7.62-7.66) */
    double h_ds  = entropy_of(h_delta, 512, nblocks * (N - 1));
    double h_f   = entropy_of(h_first, 256, nblocks);
    double h_perm = 0.0;
    double h_perm_uniform = 0.0;
    for (int i = 0; i < (int)N; i++) {
        int range = N - i;
        h_perm += entropy_of(h_lehmer[i], range, nblocks);
        if (range > 0) h_perm_uniform += log2((double)range);
    }

    double bits_sorted = h_f + (N - 1) * h_ds;
    double bits_raw    = N * h_raw;
    double saving      = bits_raw - bits_sorted;      /* กำไรจาก sort */
    double net         = saving - h_perm;              /* หัก cost permutation */

    printf("  blocks: %llu\n", (unsigned long long)nblocks);
    printf("  H(sorted-delta)=%.3f b | H(first)=%.3f b | sorted total %.1f b vs raw %.1f b\n",
           h_ds, h_f, bits_sorted, bits_raw);
    printf("  permutation: compressed %.1f b  vs  uniform bound %.1f b (log2 32!)\n",
           h_perm, h_perm_uniform);
    printf("  saving from sort %.1f b − perm cost %.1f b = NET %.1f b/block (%s)\n",
           saving, h_perm, net, net > 0 ? "POSITIVE — พอเป็นไปได้" : "≤ 0 — cost ไม่คุ้ม ยืนยัน");
    printf("  per-value: raw %.2f → sorted+perm %.2f bits\n",
           h_raw, (bits_sorted + h_perm) / N);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Beam cost probe — permutation cost จริง vs กำไรจาก sort\n");
    printf("═════════════════════════════════════════════════════════\n");
    const char *paths[4] = {
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Qwen3-0.6B-Q8_0.gguf",
        "I:/model/LFM2.5-2.6B-Q8_0.gguf",
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf"
    };
    uint32_t n = (argc > 1) ? (uint32_t)argc - 1 : 4;
    for (uint32_t i = 0; i < n; i++)
        probe_model((argc > 1) ? argv[i + 1] : paths[i]);
    return 0;
}
