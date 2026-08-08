/* kis_codec_v5_test.c — Full roundtrip test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "gguf_reader.h"
#include "core/kis_codec_v5.h"

static int pass_count = 0, fail_count = 0;
#define T(n,desc,ok) do { \
    if (ok) { pass_count++; printf("T%d: PASS — %s\n", n, desc); } \
    else    { fail_count++; printf("T%d: FAIL — %s\n", n, desc); } \
} while(0)

static int test_roundtrip(const char *name, int8_t *w, uint32_t n) {
    uint32_t buf_size = n * 8 + 1024;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    int8_t *out = (int8_t *)malloc(n);
    if (!buf || !out) { free(buf); free(out); return 0; }

    clock_t t0 = clock();
    uint32_t enc = kis_v5_encode(w, n, buf, buf_size);
    clock_t t1 = clock();
    int dec = kis_v5_decode(buf, enc, out, n);
    clock_t t2 = clock();

    uint64_t mm = 0;
    for (uint32_t i = 0; i < n; i++) if (w[i] != out[i]) mm++;

    printf("  %s: codec=%uB raw=%uB ratio=%.4fx mismatches=%lu enc=%.1fms dec=%.1fms\n",
           name, enc, n, (double)enc/n, (unsigned long)mm,
           (double)(t1-t0)/CLOCKS_PER_SEC*1000,
           (double)(t2-t1)/CLOCKS_PER_SEC*1000);

    free(buf); free(out);
    return (dec == 0 && mm == 0);
}

/* Decode Q8_0 blocks → int8 weights using FP16 ldexpf */
static uint32_t decode_q8_weights(const uint8_t *data, uint32_t n_blocks,
                                   int8_t *out, uint32_t max) {
    uint32_t loaded = 0;
    for (uint32_t b = 0; b < n_blocks && loaded < max; b++) {
        uint16_t su;
        memcpy(&su, data + b*34, 2);
        uint32_t exp = (su >> 10) & 0x1F, mant = su & 0x3FF;
        float scale;
        if (exp == 0) scale = (float)mant / 1024.0f * 5.960464478e-8f;
        else {
            scale = (float)mant / 1024.0f + 1.0f;
            scale = ldexpf(scale, (int)exp - 15);
        }
        if (su & 0x8000) scale = -scale;
        (void)scale; /* weight value = int8, no scale needed for codec test */
        for (int i = 0; i < 32 && loaded < max; i++)
            out[loaded++] = (int8_t)data[b*34 + 2 + i];
    }
    return loaded;
}

/* Find first Q8_0 tensor (type=8) with most data */
static int find_q8_tensor(GgufReader *gf) {
    int best = -1;
    for (uint32_t i = 0; i < gf->n_tensors; i++) {
        if (gf->sizes[i] > 0 && gf->sizes[i] > (uint32_t)(best >= 0 ? gf->sizes[best] : 0)) {
            /* Q8_0 size = blocks * 34 */
            if (gf->sizes[i] % 34 == 0) best = i;
        }
    }
    return best;
}

int main(void) {
    printf("╔══ KIS CODEC v5 — Roundtrip Test ══╗\n\n");

    /* Synthetic */
    printf("═══ Synthetic ═══\n");
    {
        int8_t w[1000]; for (int i = 0; i < 1000; i++) w[i] = 42;
        T(1, "All same (42)", test_roundtrip("same42", w, 1000));
    }
    {
        int8_t w[1000]; for (int i = 0; i < 1000; i++) w[i] = (i%2)?1:-1;
        T(2, "Alternating ±1", test_roundtrip("alt", w, 1000));
    }
    {
        int8_t w[10000]; srand(12345);
        for (int i = 0; i < 10000; i++) w[i] = (int8_t)(rand()%256);
        T(3, "Random 256", test_roundtrip("rand", w, 10000));
    }
    {
        int8_t w[4] = {-128,-1,0,127};
        T(4, "Edge values", test_roundtrip("edge", w, 4));
    }

    /* Real GGUF */
    printf("\n═══ Real GGUF ═══\n");
    const char *models[] = {
        "I:/model/qwen25_q8.gguf",
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        NULL
    };
    for (int m = 0; models[m]; m++) {
        GgufReader gf;
        if (gguf_open(models[m], &gf) != 0) {
            printf("  SKIP: %s\n", models[m]);
            continue;
        }

        int tidx = find_q8_tensor(&gf);
        if (tidx < 0) {
            printf("  SKIP: no Q8_0 in %s\n", models[m]);
            gguf_close(&gf);
            continue;
        }

        uint32_t n = 1000000;
        uint32_t n_blocks = gf.sizes[tidx] / 34;
        if (n_blocks * 32 < n) n = n_blocks * 32;

        printf("\n  %s — %s (%u weights)\n", models[m], gf.names[tidx], n);

        /* Read tensor via bulk mmap */
        uint8_t *raw_data = (uint8_t *)malloc(gf.sizes[tidx]);
        if (!raw_data) { gguf_close(&gf); continue; }
        if (gguf_read_tensor(models[m], &gf, tidx, raw_data, gf.sizes[tidx]) != 0) {
            printf("  read failed\n");
            free(raw_data); gguf_close(&gf); continue;
        }

        int8_t *weights = (int8_t *)malloc(n);
        uint32_t loaded = decode_q8_weights(raw_data, n_blocks, weights, n);
        free(raw_data);
        gguf_close(&gf);

        if (loaded == 0) {
            printf("  no weights decoded\n");
            free(weights);
            continue;
        }

        T(10 + m, models[m], test_roundtrip(models[m], weights, loaded));
        free(weights);
    }

    printf("\n══════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass_count, fail_count);
    printf("══════════════════════════════\n");
    return fail_count;
}
