/* tools/huff_delta_measure.c — T1.1d: Huffman จริงบน residual เทียบ entropy bound
 * ═══════════════════════════════════════════════════════════════════════
 * user: "เอา entropy-coded residual มาใส่เป็น byte plane ตัวจริง: เขียน
 *        Huffman/range coder ขนาดเล็กบน residual (WAV/MP4/Q8) วัดขนาด
 *        byte จริงเทียบ entropy bound 0.77-0.85 B/cell"
 *
 * ต่อ block 16 KB (128×128): center B=2 predict → residual (เหมือน T1.1c)
 *   per-block codebook : แต่ละ block มี Huffman ของตัวเอง (+256 B lens)
 *   global codebook    : 1 Huffman ทั้งไฟล์ (+256 B lens)
 * วัด: entropy bound (B/cell) · Huffman จริง (B/cell) · ratio actual/bound
 *      · with-base (self-contained: +4096 B/block — honest full cost)
 * lossless: encode → decode → memcmp ทุก block
 *
 * BUILD: gcc -O2 -I. -Icore -Icore/infra -o build/huff_delta_measure tools/huff_delta_measure.c -lm
 * RUN:   build/huff_delta_measure --file <path> | --syn <kind> <n> | --gguf <model> <idx>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gguf_reader.h"
#include "../core/huff_codec.h"

#define MAXN  (8u << 20)
#define BLOCK 16384u
#define BINS  256u

typedef struct { uint64_t h[BINS]; uint64_t total; } Hist;

static double hist_entropy(const Hist *H) {
    if (!H->total) return 0.0;
    double e = 0.0;
    for (int i = 0; i < BINS; i++) {
        if (!H->h[i]) continue;
        double p = (double)H->h[i] / (double)H->total;
        e -= p * log(p) * 1.4426950408889634;
    }
    return e;
}

/* center B=2 residual ของ block (128×128) */
static void block_residual(const uint8_t *fine, uint8_t *res, Hist *Hres) {
    uint32_t rows = 128, cols = 128;
    uint8_t base[64 * 64];
    memset(Hres, 0, sizeof(*Hres));
    for (uint32_t i = 0; i < 64; i++)
        for (uint32_t j = 0; j < 64; j++)
            base[i * 64 + j] = fine[(size_t)(i * 2 + 1) * cols + (j * 2 + 1)];
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t c = 0; c < cols; c++) {
            uint8_t pred = base[(size_t)(r / 2) * 64 + (c / 2)];
            uint8_t rr = (uint8_t)((fine[(size_t)r * cols + c] - pred) & 0xFFu);
            res[(size_t)r * cols + c] = rr;
            Hres->h[rr]++; Hres->total++;
        }
}

static void run(const char *name, uint8_t *x, uint32_t n) {
    uint32_t pad = (BLOCK - (n % BLOCK)) % BLOCK;
    for (uint32_t i = 0; i < pad; i++) x[n + i] = 0;
    uint32_t nblocks = (n + BLOCK - 1) / BLOCK;

    uint8_t *res = (uint8_t *)malloc(BLOCK);
    uint8_t *coded = (uint8_t *)malloc(BLOCK * 2 + 512);
    uint8_t *decoded = (uint8_t *)malloc(BLOCK);

    double ent_sum = 0.0;
    uint64_t per_block_bytes = 0;        /* Σ(256 lens + coded) */
    uint64_t global_coded = 0;
    uint64_t global_freq[256] = {0};
    int all_ok = 1;

    for (uint32_t b = 0; b < nblocks; b++) {
        Hist Hres;
        block_residual(x + (size_t)b * BLOCK, res, &Hres);
        ent_sum += hist_entropy(&Hres);

        uint64_t freq[256] = {0};
        for (int s = 0; s < BINS; s++) freq[s] = Hres.h[s];

        HuffModel m;
        huff_build(&m, freq);
        uint32_t cb = huff_encode(&m, res, BLOCK, coded, BLOCK * 2 + 512);
        if (cb == 0) { all_ok = 0; }
        per_block_bytes += 256u + cb;

        /* verify roundtrip */
        HuffModel mr;
        huff_rebuild(&mr, m.lens);
        if (huff_decode(&mr, coded, cb, decoded, BLOCK) != 0) all_ok = 0;
        if (memcmp(decoded, res, BLOCK) != 0) all_ok = 0;

        for (int s = 0; s < BINS; s++) global_freq[s] += freq[s];
        global_coded += cb;    /* global codebook coded size ≠ per-block — recompute below */
    }

    /* global codebook: rebuild from combined freq (all blocks as one stream) */
    uint8_t *all_res = (uint8_t *)malloc((size_t)nblocks * BLOCK);
    for (uint32_t b = 0; b < nblocks; b++) {
        Hist Hres;
        block_residual(x + (size_t)b * BLOCK, all_res + (size_t)b * BLOCK, &Hres);
    }
    HuffModel gm;
    huff_build(&gm, global_freq);
    uint8_t *gcode = (uint8_t *)malloc((size_t)nblocks * BLOCK * 2 + 512);
    uint32_t gcb = huff_encode(&gm, all_res, nblocks * BLOCK, gcode,
                               (uint32_t)((size_t)nblocks * BLOCK * 2 + 512));
    uint64_t global_bytes = 256u + gcb;
    free(gcode);

    double cells = (double)nblocks * BLOCK;
    double bound_bpc = ent_sum / (double)nblocks / 8.0;     /* B/cell */
    double pb_bpc = (double)per_block_bytes / cells;
    double g_bpc = (double)global_bytes / cells;
    double base_bpc = 4096.0 / (double)BLOCK;               /* 0.25 */
    double with_base = pb_bpc + base_bpc;

    printf("%-34s %9u B  blocks=%u  lossless=%s\n", name, n, nblocks,
           all_ok ? "OK ✓" : "FAIL ✗");
    printf("  entropy bound : %.3f B/cell   Huffman per-block: %.3f B/cell (%2.0f%% ของ bound)\n",
           bound_bpc, pb_bpc, 100.0 * pb_bpc / bound_bpc);
    printf("  Huffman global: %.3f B/cell   full-delta(raw): 1.000 B/cell\n", g_bpc);
    printf("  per-block  : %lu B  | global : %lu B  | full : %lu B\n",
           (unsigned long)per_block_bytes, (unsigned long)global_bytes,
           (unsigned long)(nblocks * BLOCK));
    printf("  vs full-delta: per-block %.2f× | global %.2f× | with-base(0.25) %.2f×\n",
           pb_bpc, g_bpc, with_base);
    printf("  ──\n");

    free(res); free(coded); free(decoded); free(all_res);
}

static void synth_fill(uint8_t *x, uint32_t n, const char *kind) {
    uint32_t c = 512, r = n / c; if (r < 1) r = 1;
    if (strcmp(kind, "noise") == 0) {
        uint64_t s = 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < n; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; x[i] = (uint8_t)s; }
    } else if (strcmp(kind, "smooth") == 0) {
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(((i / c) * 255u / r + (i % c) * 255u / c) / 2);
    } else if (strcmp(kind, "sine2d") == 0) {
        for (uint32_t i = 0; i < n; i++)
            x[i] = (uint8_t)(128 + 120 * sin((double)(i % c) * 0.1) * cos((double)(i / c) * 0.05));
    } else {
        for (uint32_t i = 0; i < n; i++) x[i] = (uint8_t)(i * 255u / n);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage:\n  %s --syn <kind> <n>\n  %s --file <path>\n  %s --gguf <model> <idx>\n",
               argv[0], argv[0], argv[0]);
        return 1;
    }
    uint8_t *x = (uint8_t *)malloc(MAXN);
    if (!x) return 1;

    if (strcmp(argv[1], "--syn") == 0 && argc >= 4) {
        uint32_t n = (uint32_t)strtoul(argv[3], NULL, 10);
        if (n > MAXN) n = MAXN;
        synth_fill(x, n, argv[2]);
        run(argv[2], x, n);
        return 0;
    }
    if (strcmp(argv[1], "--file") == 0 && argc >= 3) {
        FILE *fp = fopen(argv[2], "rb");
        if (!fp) { printf("cannot open %s\n", argv[2]); return 1; }
        uint32_t n = (uint32_t)fread(x, 1, MAXN, fp);
        fclose(fp);
        run(argv[2], x, n);
        return 0;
    }
    if (strcmp(argv[1], "--gguf") == 0 && argc >= 4) {
        const char *path = argv[2];
        uint32_t idx = (uint32_t)strtoul(argv[3], NULL, 10);
        GgufReader r;
        if (gguf_open(path, &r) != 0) { printf("cannot open %s\n", path); return 1; }
        if (idx >= r.n_tensors) { printf("idx %u out of range (%u)\n", idx, r.n_tensors); return 1; }
        uint64_t sz = r.sizes[idx];
        if (sz > MAXN) sz = MAXN;
        uint8_t *t = (uint8_t *)malloc((size_t)sz);
        if (!t) return 1;
        if (gguf_read_tensor(path, &r, idx, t, (uint32_t)sz) != 0) { printf("read fail\n"); return 1; }
        char nm[96];
        snprintf(nm, sizeof(nm), "%s[%u] %s", path, idx, r.names[idx]);
        run(nm, t, (uint32_t)sz);
        free(t);
        return 0;
    }
    printf("unknown mode\n");
    return 1;
}
