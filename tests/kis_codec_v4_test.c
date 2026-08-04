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
 *   gcc -O2 -Wall -Wextra -I. -o runner/explore/kis_codec_v4_test.exe \
 *       runner/explore/kis_codec_v4_test.c -lm
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "core/kis_codec_v4.h"
#include "runner/explore/gguf_reader.h"

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

static void test_real_gguf(const char *path, uint32_t max_weights) {
    printf("\n═══ %s (max=%u) ═══\n", path, max_weights);

    GGUF_File gf;
    memset(&gf, 0, sizeof(gf));
    gf.fp = fopen(path, "rb");
    if (!gf.fp) { printf("  Cannot open\n"); return; }

    uint32_t magic; fread(&magic, 4, 1, gf.fp);
    if (magic != GGUF_MAGIC) { fclose(gf.fp); printf("  Bad magic\n"); return; }
    fread(&gf.version, 4, 1, gf.fp);
    fread(&gf.tensor_count, 8, 1, gf.fp);
    fread(&gf.kv_count, 8, 1, gf.fp);

    for (uint64_t i = 0; i < gf.kv_count; i++) {
        GGUFFieldStr k; read_gguf_str_fp(gf.fp, &k);
        uint32_t vt; fread(&vt, 4, 1, gf.fp);
        skip_gguf_value(gf.fp, vt);
        free(k.data);
    }

    gf.tensors = (GGUF_Tensor *)malloc(sizeof(GGUF_Tensor) * gf.tensor_count);
    for (uint64_t i = 0; i < gf.tensor_count; i++) {
        GGUF_Tensor *t = &gf.tensors[i];
        GGUFFieldStr nm; read_gguf_str_fp(gf.fp, &nm);
        strncpy(t->name, nm.data, 255); free(nm.data);
        fread(&t->n_dims, 4, 1, gf.fp);
        for (uint32_t d = 0; d < t->n_dims; d++) fread(&t->dims[d], 8, 1, gf.fp);
        fread(&t->type, 4, 1, gf.fp);
        uint64_t off; fread(&off, 8, 1, gf.fp); t->offset = off;
        uint64_t bs, wpb; ggml_type_block_size(t->type, &bs, &wpb);
        uint64_t nb = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) nb *= t->dims[d];
        nb /= wpb;
        t->size_bytes = nb * bs;
        t->n_weights = nb * wpb;
    }

    long pos = ftell(gf.fp);
    long aligned = (pos + 31) & ~31L;
    fseek(gf.fp, aligned, SEEK_SET);
    gf.tensor_data_start = aligned;

    /* Find first Q8_0 tensor */
    int found = 0;
    for (uint64_t ti = 0; ti < gf.tensor_count; ti++) {
        GGUF_Tensor *t = &gf.tensors[ti];
        if (t->type != GGML_TYPE_Q8_0) continue;
        found = 1;

        uint32_t n = (max_weights > 0 && t->n_weights > max_weights)
                      ? max_weights : (uint32_t)t->n_weights;
        printf("  Tensor: %s (%u weights)\n", t->name, n);

        /* Read weights */
        fseek(gf.fp, gf.tensor_data_start + t->offset, SEEK_SET);
        int8_t *weights = (int8_t *)malloc(n);
        uint64_t nblocks = n / 32;
        uint32_t read_count = 0;
        for (uint64_t b = 0; b < nblocks; b++) {
            uint16_t scale; int8_t w[32];
            if (fread(&scale, 2, 1, gf.fp) != 1) break;
            if (fread(w, 1, 32, gf.fp) != 32) break;
            uint32_t chunk = (32 < n - read_count) ? 32 : (n - read_count);
            memcpy(weights + read_count, w, chunk);
            read_count += chunk;
            if (read_count >= n) break;
        }

        /* Encode */
        clock_t t0 = clock();
        uint32_t buf_size = n * 2; /* worst case for permutation */
        uint8_t *buf = (uint8_t *)malloc(buf_size);
        uint32_t enc = kis_v4_encode(weights, n, buf, buf_size);
        clock_t t1 = clock();

        /* Decode */
        int8_t *decoded = (int8_t *)malloc(n);
        int rc = kis_v4_decode(buf, enc, decoded, n);
        clock_t t2 = clock();

        /* Compare */
        uint32_t mismatches = 0;
        if (rc == 0) {
            for (uint32_t i = 0; i < n; i++) {
                if (decoded[i] != weights[i]) mismatches++;
            }
        } else {
            mismatches = n;
            printf("  Decode error: %d\n", rc);
        }

        double enc_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000;
        double dec_ms = (double)(t2 - t1) / CLOCKS_PER_SEC * 1000;

        printf("  Codec: %u bytes (%.2f KB)\n", enc, enc / 1024.0);
        printf("  Raw:   %u bytes (%.2f MB)\n", n, n / 1048576.0);
        printf("  Ratio: %.2fx\n", (double)n / enc);
        printf("  Mismatches: %u / %u\n", mismatches, n);
        printf("  Encode: %.1f ms, Decode: %.1f ms\n", enc_ms, dec_ms);

        /* Only first tensor for now */
        free(weights); free(buf); free(decoded);
        break;
    }

    if (!found) printf("  No Q8_0 tensor found\n");
    free(gf.tensors);
    fclose(gf.fp);
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
