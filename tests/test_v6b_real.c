/* test_v6b_real.c — v6b codec on real GGUF tensors
 * Reads tensors from a GGUF file, encodes with v6b streaming API,
 * decodes, verifies lossless byte-by-byte.
 *
 * BUILD: gcc -O2 -Wall -I core -I core/infra -o build/test_v6b_real.exe tests/test_v6b_real.c -lm
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gguf_reader.h"
#include "kis_codec_v6b.h"

static double elapsed_ms(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

static const char *dtype_name(uint8_t dt) {
    switch (dt) {
        case 0: return "F32";
        case 1: return "F16";
        case 2: return "Q4_0";
        case 3: return "Q4_1";
        case 6: return "Q5_0";
        case 7: return "Q5_1";
        case 8: return "Q8_0";
        case 10: return "Q2_K";
        case 11: return "Q3_K";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        case 16: return "Q8_K";
        default: return "???";
    }
}

/* test one tensor: v6b streaming encode → decode → verify */
static int test_tensor(const char *name, uint8_t dtype,
                       const uint8_t *data, uint32_t size) {
    /* v6b works on raw bytes (Q8 treated as int8 stream) */
    uint32_t n_elems = size; /* 1 byte per element for raw bytes */
    const int8_t *weights = (const int8_t *)data;

    /* worst case buffer */
    uint32_t buf_sz = 4 + n_elems * (5u + 1u) + 4096;
    if (buf_sz > 256 * 1024 * 1024) {
        printf("    SKIP %s: too large (%u MB)\n", name, buf_sz / (1024*1024));
        return -1;  /* -1 = skip, not fail */
    }
    uint8_t *buf = (uint8_t *)malloc(buf_sz);
    int8_t *dec = (int8_t *)malloc(n_elems);
    if (!buf || !dec) { free(buf); free(dec); return 0; }

    struct timespec t0, t1;

    /* encode */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    v6b_stream_t st;
    v6b_init(&st, V6B_Q8);
    v6b_collect(&st, weights, n_elems);
    uint32_t hw = v6b_header(&st, buf, buf_sz);
    uint32_t ew = 0, total_emit = 0;
    while ((ew = v6b_emit(&st, buf + hw + total_emit, buf_sz - hw - total_emit)) > 0)
        total_emit += ew;
    uint32_t total = hw + total_emit;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double enc_ms = elapsed_ms(t0, t1);

    /* decode */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t got = v6b_decode_all(buf, total, (uint8_t *)dec, n_elems);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dec_ms = elapsed_ms(t0, t1);

    /* verify */
    int match = (got == n_elems) && (memcmp(weights, dec, n_elems) == 0);

    double ratio = (double)total / n_elems;
    printf("    %s  %s  %u B → %u B (%.2fx)  enc %.1f ms  dec %.1f ms  %s\n",
           match ? "PASS" : "FAIL",
           name, n_elems, total, ratio, enc_ms, dec_ms,
           match ? "" : "MISMATCH");

    free(buf); free(dec);
    v6b_free(&st);
    return match;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "I:\\model\\SmolLM2-360M-Instruct.Q8_0.gguf";

    printf("v6b Real GGUF Tensor Test\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("File: %s\n\n", path);

    GgufReader r;
    if (gguf_open(path, &r) != 0) {
        printf("Failed to open %s\n", path);
        return 1;
    }

    printf("Tensors: %u\n\n", r.n_tensors);

    int pass = 0, fail = 0, skip = 0;

    /* test first 10 tensors (or all if fewer) */
    uint32_t n_test = r.n_tensors < 10 ? r.n_tensors : 10;

    /* get max tensor size for buffer */
    uint32_t max_sz = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        if (r.sizes[i] > max_sz) max_sz = r.sizes[i];
    }

    for (uint32_t i = 0; i < n_test; i++) {
        uint8_t *data = (uint8_t *)malloc(r.sizes[i]);
        if (!data) { skip++; continue; }

        if (gguf_read_tensor(path, &r, i, data, r.sizes[i]) != 0) {
            printf("  SKIP  %s: read failed\n", r.names[i]);
            skip++;
            free(data);
            continue;
        }

        printf("  [%u] %s  dtype=%s  size=%u B\n",
               i, r.names[i], dtype_name(r.dtypes[i]), r.sizes[i]);

        int ok = test_tensor(r.names[i], r.dtypes[i], data, r.sizes[i]);
        if (ok > 0) pass++; else if (ok == 0) fail++; else skip++;

        free(data);
    }

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("Results: %d PASS, %d FAIL, %d SKIP (of %u tested)\n",
           pass, fail, skip, n_test);

    gguf_close(&r);
    return fail ? 1 : 0;
}
