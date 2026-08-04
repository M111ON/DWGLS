/* kis_codec_v5_test.c — Full roundtrip test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
        "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf",
        "I:/model/qwen25_q8.gguf",
        "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf",
        "I:/model/Kokoro_no_espeak_Q8.gguf",
        NULL
    };
    for (int m = 0; models[m]; m++) {
        GGUF_File *gf = gguf_open(models[m]);
        if (!gf) { printf("  SKIP: %s\n", models[m]); continue; }
        int tidx = -1;
        for (uint64_t i = 0; i < gf->tensor_count; i++)
            if (gf->tensors[i].type == GGML_TYPE_Q8_0) { tidx = (int)i; break; }
        if (tidx < 0) { printf("  SKIP: no Q8_0\n"); gguf_close(gf); continue; }

        GGUF_Tensor *t = &gf->tensors[tidx];
        uint32_t n = 1000000;
        printf("\n  %s — %s (%u weights)\n", models[m], t->name, n);

        int8_t *raw = (int8_t *)malloc(n);
        uint64_t foff = gf->tensor_data_start + t->offset;
        foff = (foff + 31) & ~(uint64_t)31;
        fseek(gf->fp, (long)foff, SEEK_SET);
        uint32_t rd = 0;
        uint64_t nblk = (t->n_weights + 31) / 32;
        for (uint64_t b = 0; b < nblk && rd < n; b++) {
            uint16_t scale; int8_t w[32];
            if (fread(&scale,2,1,gf->fp) != 1) break;
            if (fread(w,1,32,gf->fp) != 32) break;
            for (int i = 0; i < 32 && rd < n; i++) raw[rd++] = w[i];
        }
        gguf_close(gf);

        T(10 + m, models[m], test_roundtrip(models[m], raw, rd));
        free(raw);
    }

    printf("\n══════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass_count, fail_count);
    printf("══════════════════════════════\n");
    return fail_count;
}
