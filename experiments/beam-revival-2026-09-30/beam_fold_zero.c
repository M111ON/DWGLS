/* tools/beam_fold_zero.c — Option B: fold x:-x → นับ 0 ที่ structure สร้าง
 * ═══════════════════════════════════════════════════════════════════════════
 * histogram 256 levels → foldable = Σ_{v>0} 2*min(cnt[v],cnt[-v]) + cnt[0]
 * fold (v,-v) → (0, carry=v): เก็บ count + carries เทียบ raw
 * ตัดสิน: zeros เยอะ = island work / น้อย = hourglass ไม่ help
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_fold_zero.exe tools/beam_fold_zero.c -lm
 * RUN:   ./build/beam_fold_zero.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf\n", argv[0]); return 2; }
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    unsigned long long hist[256];
    memset(hist, 0, sizeof(hist));
    unsigned long long n = 0;
    for (unsigned t = 0; t < box.n_tensors; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        unsigned long long stride = (nb + 2000000 - 1) / 2000000;
        for (unsigned long long b = 0; b < nb; b += stride) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32; k++) { hist[blk[k]]++; n++; }
        }
    }
    /* foldable: v>0 จับคู่ -v (byte 256-v), 0 อยู่แล้ว, ±128 พิเศษ */
    unsigned long long pairs = 0;
    for (int v = 1; v <= 127; v++) {
        unsigned long long a = hist[v], b = hist[256 - v];
        pairs += (a < b ? a : b);
    }
    unsigned long long zeros = 2 * pairs + hist[0];
    /* -128 ไม่มีคู่ (+128 ไม่มีใน int8) → fold ไม่ได้ */
    printf("n=%llu foldable_pairs=%llu struct_zeros=%llu (%.2f%%) (-128 orphan=%llu)\n",
           n, pairs, zeros, 100.0 * zeros / n, hist[128]);
    /* cost: count(64b) + carries 8b/pair เทียบ raw 8b/value */
    double cost_bits = 64.0 + (double)pairs * 8.0;
    double raw_bits = (double)n * 8.0;
    /* เหลือ: unpaired ต้องเก็บ raw */
    unsigned long long unpaired = n - 2 * pairs - hist[0];
    cost_bits += (double)unpaired * 8.0;
    /* zeros เก็บเป็น count เดียว (sector-0 bunch) ≈ ฟรี */
    printf("fold cost=%.1f MB vs raw=%.1f MB ratio=%.3fx\n",
           cost_bits / 8 / 1e6, raw_bits / 8 / 1e6, cost_bits / raw_bits);
    return 0;
}
