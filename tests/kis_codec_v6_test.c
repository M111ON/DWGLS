/* kis_codec_v6_test.c — Full roundtrip test for index-based codec */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "gguf_reader.h"
#include "core/kis_codec_v6.h"

static int pass_count = 0, fail_count = 0;
#define T(n, desc, ok) do { \
    if (ok) { pass_count++; printf("T%d: PASS — %s\n", n, desc); } \
    else    { fail_count++; printf("T%d: FAIL — %s\n", n, desc); } \
} while(0)

static int test_roundtrip(const char *name, int8_t *w, uint32_t n) {
    uint32_t buf_size = n * 8 + 4096;  /* extra headroom for bitmap overhead */
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    int8_t *out = (int8_t *)malloc(n);
    if (!buf || !out) { free(buf); free(out); return 0; }

    clock_t t0 = clock();
    uint32_t enc = kis_v6_encode(w, n, buf, buf_size);
    clock_t t1 = clock();
    int dec = kis_v6_decode(buf, enc, out, n);
    clock_t t2 = clock();

    uint64_t mm = 0;
    for (uint32_t i = 0; i < n; i++) if (w[i] != out[i]) mm++;

    printf("  %s: codec=%uB raw=%uB ratio=%.4fx mismatches=%lu enc=%.1fms dec=%.1fms\n",
           name, enc, n, enc > 0 ? (double)enc / n : 0.0, (unsigned long)mm,
           (double)(t1 - t0) / CLOCKS_PER_SEC * 1000,
           (double)(t2 - t1) / CLOCKS_PER_SEC * 1000);

    free(buf); free(out);
    return (dec == 0 && mm == 0);
}

/* Verify slot function is a permutation for first 20736 values */
static int test_slot_permutation(void) {
    uint8_t seen[V6_SLOTS];
    memset(seen, 0, V6_SLOTS);
    for (uint32_t i = 0; i < V6_SLOTS; i++) {
        uint32_t s = v6_slot(i);
        if (s >= V6_SLOTS) return 0;
        if (seen[s]) return 0;  /* collision */
        seen[s] = 1;
    }
    return 1;
}

int main(void) {
    printf("╔══ KIS CODEC v6 — Index-Based Mapping Roundtrip ══╗\n\n");

    /* T0: Slot permutation check */
    printf("═══ Slot Permutation ═══\n");
    T(0, "v6_slot(i) is a permutation of 0..20735", test_slot_permutation());

    /* Synthetic */
    printf("\n═══ Synthetic ═══\n");
    {
        int8_t w[1000];
        for (int i = 0; i < 1000; i++) w[i] = 42;
        T(1, "All same (42)", test_roundtrip("same42", w, 1000));
    }
    {
        int8_t w[1000];
        for (int i = 0; i < 1000; i++) w[i] = (i % 2) ? 1 : -1;
        T(2, "Alternating ±1", test_roundtrip("alt", w, 1000));
    }
    {
        int8_t w[10000];
        srand(12345);
        for (int i = 0; i < 10000; i++) w[i] = (int8_t)(rand() % 256);
        T(3, "Random 256", test_roundtrip("rand", w, 10000));
    }
    {
        int8_t w[4] = {-128, -1, 0, 127};
        T(4, "Edge values", test_roundtrip("edge", w, 4));
    }
    {
        /* Large synthetic: exactly V6_SLOTS */
        int8_t w[V6_SLOTS];
        srand(99);
        for (uint32_t i = 0; i < V6_SLOTS; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T(5, "One full grid (20736)", test_roundtrip("grid20736", w, V6_SLOTS));
    }
    {
        /* Over-chunk: 2 * V6_SLOTS + 1 */
        uint32_t n = V6_SLOTS * 2 + 1;
        int8_t *w = (int8_t *)malloc(n);
        srand(777);
        for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T(6, "Over-chunk (41473)", test_roundtrip("overchunk", w, n));
        free(w);
    }

    /* Real GGUF */
    printf("\n═══ Real GGUF ═══\n");
    const char *models[] = {
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf",
        "I:/model/qwen25_q8.gguf",
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Kokoro_no_espeak_Q8.gguf",
        NULL
    };
    for (int m = 0; models[m]; m++) {
        GGUF_File *gf = gguf_open(models[m]);
        if (!gf) { printf("  SKIP: %s (open failed)\n", models[m]); continue; }

        /* Find first Q8_0 tensor */
        int tidx = -1;
        for (uint64_t i = 0; i < gf->tensor_count; i++)
            if (gf->tensors[i].type == GGML_TYPE_Q8_0) { tidx = (int)i; break; }
        if (tidx < 0) { printf("  SKIP: %s (no Q8_0)\n", models[m]); gguf_close(gf); continue; }

        GGUF_Tensor *t = &gf->tensors[tidx];
        uint32_t n = 1000000;  /* test 1M weights */
        printf("\n  %s — %s (n=%u)\n", models[m], t->name, n);

        int8_t *raw = (int8_t *)malloc(n);
        if (!raw) { gguf_close(gf); continue; }

        uint64_t foff = gf->tensor_data_start + t->offset;
        foff = (foff + 31) & ~(uint64_t)31;
        fseek(gf->fp, (long)foff, SEEK_SET);
        uint32_t rd = 0;
        uint64_t nblk = (t->n_weights + 31) / 32;
        for (uint64_t b = 0; b < nblk && rd < n; b++) {
            uint16_t scale;
            int8_t wblk[32];
            if (fread(&scale, 2, 1, gf->fp) != 1) break;
            if (fread(wblk, 1, 32, gf->fp) != 32) break;
            for (int i = 0; i < 32 && rd < n; i++) raw[rd++] = wblk[i];
        }
        gguf_close(gf);

        if (rd < n) printf("  (only read %u of %u weights)\n", rd, n);
        T(10 + m, models[m], test_roundtrip(models[m], raw, rd));
        free(raw);
    }

    printf("\n══════════════════════════════\n");
    printf("  FINAL: %d PASS / %d FAIL\n", pass_count, fail_count);
    printf("══════════════════════════════\n");
    return fail_count;
}
