/* ═══════════════════════════════════════════════════════════════════════════
 * kis_codec_v4_test.c — Full codec test: Codebook + Permutation roundtrip
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Tests:
 *   T1-T4: Synthetic edge cases (all same, alternating, random, single)
 *   T5-T8: Real GGUF models — full tensor roundtrip
 *   T9-T10: Speed benchmark
 *
 * Compile:
 *   gcc -O2 -Wall -Wextra -I. -Icore -Icore/infra -o test-kis_codec_v4_test tests/kis_codec_v4_test.c -lm
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include "core/kis_codec_v4.h"
#include "gguf_reader.h"

static int pass_count = 0, fail_count = 0;
#define T(n,desc,ok) do { \
    if (ok) { pass_count++; printf("T%d: PASS — %s\n", n, desc); } \
    else    { fail_count++; printf("T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ═══════ SYNTHETIC TESTS ═══════════════════════════════════════════════════ */

static void test_all_same(void) {
    uint32_t n = 100000;
    int8_t *w = (int8_t *)malloc(n);
    memset(w, 42, n); /* all value 42 */
    uint32_t mismatches = kis_v4_roundtrip_test(w, n);
    uint32_t buf_size = n + 2048;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    uint32_t enc = kis_v4_encode(w, n, buf, buf_size);
    T(1, "All same value (42) — roundtrip", mismatches == 0);
    printf("    codec: %u bytes, raw: %u bytes, ratio: %.1fx\n",
           enc, n, (double)n / enc);
    free(w); free(buf);
}

static void test_alternating(void) {
    uint32_t n = 100000;
    int8_t *w = (int8_t *)malloc(n);
    for (uint32_t i = 0; i < n; i++) w[i] = (i % 2) ? 1 : -1;
    uint32_t mismatches = kis_v4_roundtrip_test(w, n);
    uint32_t buf_size = n + 2048;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    uint32_t enc = kis_v4_encode(w, n, buf, buf_size);
    T(2, "Alternating +1/-1 — roundtrip", mismatches == 0);
    printf("    codec: %u bytes, raw: %u bytes, ratio: %.1fx\n",
           enc, n, (double)n / enc);
    free(w); free(buf);
}

static void test_random_256(void) {
    uint32_t n = 100000;
    int8_t *w = (int8_t *)malloc(n);
    srand(12345);
    for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() & 0xFF);
    uint32_t mismatches = kis_v4_roundtrip_test(w, n);
    uint32_t buf_size = n * 2; /* permutation can be up to ~2x for random data */
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    uint32_t enc = kis_v4_encode(w, n, buf, buf_size);
    T(3, "Random 256 values — roundtrip", mismatches == 0);
    printf("    codec: %u bytes, raw: %u bytes, ratio: %.1fx\n",
           enc, n, (double)n / enc);
    free(w); free(buf);
}

static void test_single(void) {
    int8_t w = -128;
    /* Allocate enough for single weight */
    uint8_t buf[1024];
    int8_t decoded = 0;
    uint32_t enc = kis_v4_encode(&w, 1, buf, sizeof(buf));
    int rc = kis_v4_decode(buf, enc, &decoded, 1);
    uint32_t mismatches = (rc == 0 && decoded == w) ? 0 : 1;
    T(4, "Single weight — roundtrip", mismatches == 0);
}

/* ═══════ REAL GGUF TESTS ═══════════════════════════════════════════════════ */

/* Q8_0: 2B FP16 scale + 32 × int8 = 34B per block.
 * We use FP16 ldexpf decode (verified correct), NOT fixed-point. */
static uint32_t decode_q8_block(const uint8_t *block, int8_t *out32) {
    uint16_t su;
    memcpy(&su, block, 2);
    /* FP16 → float using ldexpf (CORRECT decode) */
    uint32_t exp = (su >> 10) & 0x1F, mant = su & 0x3FF;
    float scale;
    if (exp == 0) scale = (float)mant / 1024.0f * 5.960464478e-8f;
    else {
        scale = (float)mant / 1024.0f + 1.0f;
        scale = ldexpf(scale, (int)exp - 15);
    }
    if (su & 0x8000) scale = -scale;
    for (int i = 0; i < 32; i++) {
        out32[i] = (int8_t)block[2 + i];
    }
    return 32;
}

static void test_real_gguf(const char *path, uint32_t max_weights) {
    printf("\n═══ %s (max=%u) ═══\n", path, max_weights);

    GgufReader gf;
    if (gguf_open(path, &gf) != 0) {
        printf("  Cannot open\n");
        return;
    }
    printf("  tensors=%u\n", gf.n_tensors);

    /* Find first Q8_0 tensor (type=8) with most data */
    int target_idx = -1;
    for (uint32_t i = 0; i < gf.n_tensors; i++) {
        /* sizes[] in bulk reader = byte size of tensor data */
        if (gf.sizes[i] > 0) {
            if (target_idx < 0 || gf.sizes[i] > gf.sizes[target_idx])
                target_idx = i;
        }
    }
    if (target_idx < 0) { printf("  No tensor found\n"); gguf_close(&gf); return; }

    printf("  Tensor[%d]: %s size=%u bytes\n",
           target_idx, gf.names[target_idx], gf.sizes[target_idx]);

    /* Read tensor data via bulk mmap (zero syscall) */
    uint32_t sz = gf.sizes[target_idx];
    uint8_t *buf = (uint8_t*)malloc(sz);
    if (!buf) { gguf_close(&gf); return; }

    if (gguf_read_tensor(path, &gf, target_idx, buf, sz) != 0) {
        printf("  read_tensor failed\n");
        free(buf); gguf_close(&gf); return;
    }

    /* Decode Q8_0 blocks → int8 weights */
    uint32_t n_blocks = sz / 34;
    uint32_t n_weights = n_blocks * 32;
    if (max_weights > 0 && n_weights > max_weights) n_weights = max_weights;
    int8_t *weights = (int8_t *)malloc(n_weights);
    uint32_t loaded = 0;
    for (uint32_t b = 0; b < n_blocks && loaded < n_weights; b++) {
        int8_t block_out[32];
        decode_q8_block(buf + b * 34, block_out);
        uint32_t chunk = (32 < n_weights - loaded) ? 32 : (n_weights - loaded);
        memcpy(weights + loaded, block_out, chunk);
        loaded += chunk;
    }
    printf("  Loaded %u int8 weights from Q8_0 blocks\n", loaded);
    T(5, "loaded >= 64", loaded >= 64);

    /* Encode */
    clock_t t0 = clock();
    uint32_t buf_size = loaded * 2;
    uint8_t *enc_buf = (uint8_t *)malloc(buf_size);
    uint32_t enc = kis_v4_encode(weights, loaded, enc_buf, buf_size);
    clock_t t1 = clock();

    /* Decode */
    int8_t *decoded = (int8_t *)malloc(loaded);
    int rc = kis_v4_decode(enc_buf, enc, decoded, loaded);
    clock_t t2 = clock();

    /* Compare */
    uint32_t mismatches = 0;
    if (rc == 0) {
        for (uint32_t i = 0; i < loaded; i++) {
            if (decoded[i] != weights[i]) mismatches++;
        }
    } else {
        mismatches = loaded;
        printf("  Decode error: %d\n", rc);
    }

    double enc_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000;
    double dec_ms = (double)(t2 - t1) / CLOCKS_PER_SEC * 1000;

    printf("  Codec: %u bytes (%.2f KB)\n", enc, enc / 1024.0);
    printf("  Raw:   %u bytes (%.2f MB)\n", loaded, loaded / 1048576.0);
    printf("  Ratio: %.2fx\n", (double)loaded / enc);
    printf("  Mismatches: %u / %u\n", mismatches, loaded);
    printf("  Encode: %.1f ms, Decode: %.1f ms\n", enc_ms, dec_ms);
    T(6, "Real GGUF roundtrip — lossless", mismatches == 0);

    free(weights); free(enc_buf); free(decoded); free(buf);
    gguf_close(&gf);
}

/* ═══════ SPEED BENCHMARK ═══════════════════════════════════════════════════ */

static void bench_speed(void) {
    printf("\n═══ Speed Benchmark ═══\n");
    srand(99999);

    uint32_t sizes[] = {100000, 1000000, 5000000};
    for (int s = 0; s < 3; s++) {
        uint32_t n = sizes[s];
        int8_t *w = (int8_t *)malloc(n);
        for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() & 0xFF);

        uint32_t buf_size = n * 2;
        uint8_t *buf = (uint8_t *)malloc(buf_size);
        int8_t  *decoded = (int8_t *)malloc(n);

        clock_t t0 = clock();
        uint32_t enc = kis_v4_encode(w, n, buf, buf_size);
        clock_t t1 = clock();
        kis_v4_decode(buf, enc, decoded, n);
        clock_t t2 = clock();

        double enc_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000;
        double dec_ms = (double)(t2 - t1) / CLOCKS_PER_SEC * 1000;
        double mb = n / 1048576.0;

        printf("  N=%7uK: enc=%6.1fms (%.0f MB/s) dec=%6.1fms (%.0f MB/s) codec=%uB ratio=%.1fx\n",
               n/1000, enc_ms, mb/(enc_ms/1000), dec_ms, mb/(dec_ms/1000),
               enc, (double)n/enc);

        free(w); free(buf); free(decoded);
    }
}

/* ═══════ MAIN ═════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    printf("╔══ KIS CODEC v4 — Full Roundtrip Test ══╗\n\n");

    /* Synthetic tests */
    test_all_same();
    test_alternating();
    test_random_256();
    test_single();

    /* Real GGUF */
    if (argc > 1) {
        test_real_gguf(argv[1], 0); /* full tensor */
    } else {
        test_real_gguf("I:/model/SmolLM2-360M-Instruct.Q8_0.gguf", 1000000);
        test_real_gguf("I:/model/qwen25_q8.gguf", 1000000);
    }

    /* Speed */
    bench_speed();

    printf("\n══════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass_count, fail_count);
    printf("══════════════════════════════════════\n");
    return fail_count ? 1 : 0;
}
