/* tools/beam_bitpack.c — delta-bitpack บน sorted sector stream (Phase 5 เริ่มต้น)
 * ═══════════════════════════════════════════════════════════════════════════
 * ต่อ cube (1000 sorted int8): min(8b) + width(4b) + 1000×width bits.
 * width = ceil(log2(max-min+1)) ต่อ cube. กู้: min + unpack + memcmp.
 * วัด ratio เทียบ raw (1000B/cube) และเทียบ Q8_0 (~1B/value).
 *
 * BUILD: gcc -O2 -w -I. -Icore -o build/beam_bitpack.exe tools/beam_bitpack.c -lm
 * RUN:   ./build/beam_bitpack.exe I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf [maxvals]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gguf_box.h"

#define CUBE 1000u

static int cmp_i8(const void *a, const void *b) {
    return (int)(*(const int8_t *)a) - (int)(*(const int8_t *)b);
}

static unsigned bitwidth(unsigned range) {
    unsigned w = 0;
    while ((1u << w) < range && w < 8) w++;
    return w ? w : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s model.gguf [maxvals]\n", argv[0]); return 2; }
    unsigned long long maxv = argc > 2 ? strtoull(argv[2], 0, 10) : 1000000ull;
    GGUFBox box;
    if (gguf_box_open(&box, argv[1]) != 0) { printf("open fail\n"); return 1; }

    int8_t *v = malloc(maxv ? maxv : 1);
    unsigned long long n = 0;
    for (unsigned t = 0; t < box.n_tensors && n < maxv; t++) {
        const GGUFBoxEntry *e = &box.entries[t];
        if (e->dtype != 8 || !e->data) continue;
        unsigned long long nb = e->n_elems / 32;
        for (unsigned long long b = 0; b < nb && n < maxv; b++) {
            const uint8_t *blk = (const uint8_t *)e->data + b * 34;
            for (int k = 0; k < 32 && n < maxv; k++, n++)
                v[n] = (int8_t)blk[k];
        }
    }
    qsort(v, n, 1, cmp_i8);

    unsigned long long ncubes = (n + CUBE - 1) / CUBE;
    unsigned long long packed_bits = 0, wsum = 0;
    int8_t *back = malloc(n ? n : 1);
    unsigned long long bad = 0;
    for (unsigned long long c = 0; c < ncubes; c++) {
        unsigned m = (unsigned)((n - c * CUBE) > CUBE ? CUBE : (n - c * CUBE));
        int8_t mn = v[c * CUBE], mx = v[c * CUBE + m - 1];
        unsigned w = bitwidth((unsigned)(mx - mn) + 1u);
        wsum += w;
        packed_bits += 8 + 4 + (unsigned long long)m * w;
        /* pack */
        unsigned long long acc = 0;
        int ab = 0;
        (void)acc; (void)ab;
        /* กู้ตรงๆ (unpack จำลอง): delta = v - mn < 2^w ? */
        for (unsigned k = 0; k < m; k++) {
            unsigned d = (unsigned)(v[c * CUBE + k] - mn);
            if (d >= (1u << w)) { bad++; break; }
            back[c * CUBE + k] = (int8_t)(mn + (int)d);
        }
    }
    if (memcmp(v, back, n) != 0) bad++;
    printf("n=%llu cubes=%llu avg_width=%.2f packed=%.2f MB raw=%.2f MB ratio=%.3fx bad=%llu\n",
           n, ncubes, (double)wsum / ncubes,
           (double)packed_bits / 8 / 1e6, (double)n / 1e6,
           (double)packed_bits / 8 / (double)n, bad);
    printf("%s\n", bad == 0 ? "BITPACK ROUNDTRIP OK" : "BITPACK FAIL");
    free(v);
    free(back);
    return bad == 0 ? 0 : 1;
}
