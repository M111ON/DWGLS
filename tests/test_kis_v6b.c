/* test_kis_v6b.c — Roundtrip + multi-type + streaming verification */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "core/kis_codec_v6b.h"

static int pass = 0, fail = 0;
#define T(desc, ok) do { if (ok) { pass++; printf("  PASS: %s\n", desc); } \
                       else { fail++; printf("  FAIL: %s\n", desc); } } while(0)

static int test_bijection(void) {
    return v6b_check_bijection();
}

static int test_roundtrip_q8(const int8_t *w, uint32_t n, const char *name) {
    v6b_stream_t st;
    if (v6b_init(&st, V6B_Q8) != 0) return 0;
    if (v6b_collect(&st, w, n) != 0) { v6b_free(&st); return 0; }
    uint32_t hdr = v6b_header(&st, NULL, 0);
    if (hdr == 0) { v6b_free(&st); return 0; }
    uint32_t nchunks = (n + V6B_SLOTS - 1) / V6B_SLOTS + 2;
    uint32_t need = hdr + nchunks * (4u + V6B_SLOTS * 6u + 64u);
    uint8_t *buf = (uint8_t *)malloc(need);
    if (!buf) { v6b_free(&st); return 0; }
    uint32_t hw = v6b_header(&st, buf, need);
    uint32_t ew = 0, total_emit = 0;
    while ((ew = v6b_emit(&st, buf + hw + total_emit, need - hw - total_emit)) > 0)
        total_emit += ew;
    uint32_t total = hw + total_emit;
    int ok = v6b_verify(buf, total, w, n);
    printf("    Q8 %s n=%u: %u B (ratio=%.2fx) %s\n",
           name, n, total, (double)total / n, ok ? "ok" : "FAIL");
    free(buf);
    v6b_free(&st);
    return ok;
}

static int test_roundtrip_fp16(const uint16_t *w, uint32_t n, const char *name) {
    v6b_stream_t st;
    if (v6b_init(&st, V6B_FP16) != 0) return 0;
    if (v6b_collect(&st, w, n) != 0) { v6b_free(&st); return 0; }
    uint32_t hdr = v6b_header(&st, NULL, 0);
    if (hdr == 0) { v6b_free(&st); return 0; }
    uint32_t nchunks = (n + V6B_SLOTS - 1) / V6B_SLOTS + 2;
    uint32_t need = hdr + nchunks * (4u + V6B_SLOTS * 6u + 64u);
    uint8_t *buf = (uint8_t *)malloc(need);
    if (!buf) { v6b_free(&st); return 0; }
    uint32_t hw = v6b_header(&st, buf, need);
    uint32_t ew = 0, total_emit = 0;
    while ((ew = v6b_emit(&st, buf + hw + total_emit, need - hw - total_emit)) > 0)
        total_emit += ew;
    uint32_t total = hw + total_emit;
    int ok = v6b_verify(buf, total, w, n);
    printf("    FP16 %s n=%u: %u B (ratio=%.2fx) %s\n",
           name, n, total, (double)total / (n * 2), ok ? "ok" : "FAIL");
    free(buf);
    v6b_free(&st);
    return ok;
}

int main(void) {
    printf("=== KIS Codec v6b - Multi-type + Streaming ===\n\n");
    fflush(stdout);

    printf("[Bijection]\n"); fflush(stdout);
    T("v6b_slot is bijection on [0,20736)", test_bijection());

    printf("\n[Q8 roundtrip]\n"); fflush(stdout);
    {
        int8_t w[1000];
        for (int i = 0; i < 1000; i++) w[i] = 42;
        T("Q8 all-same 1000", test_roundtrip_q8(w, 1000, "same42"));
    }
    {
        int8_t w[1000];
        for (int i = 0; i < 1000; i++) w[i] = (i % 2) ? 1 : -1;
        T("Q8 alt 1000", test_roundtrip_q8(w, 1000, "alt"));
    }
    {
        int8_t w[10000];
        srand(42);
        for (int i = 0; i < 10000; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T("Q8 random 10K", test_roundtrip_q8(w, 10000, "rand10k"));
    }
    {
        int8_t w[V6B_SLOTS];
        srand(7);
        for (uint32_t i = 0; i < V6B_SLOTS; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T("Q8 full grid 20736", test_roundtrip_q8(w, V6B_SLOTS, "grid"));
    }
    {
        uint32_t n = V6B_SLOTS * 2 + 100;
        int8_t *w = (int8_t *)malloc(n);
        srand(13);
        for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T("Q8 overchunk", test_roundtrip_q8(w, n, "overchunk"));
        free(w);
    }

    printf("\n[FP16 roundtrip]\n"); fflush(stdout);
    {
        uint16_t w[1000];
        srand(1);
        for (int i = 0; i < 1000; i++) w[i] = (uint16_t)(rand() & 0xFFFF);
        T("FP16 random 1000", test_roundtrip_fp16(w, 1000, "rand1k"));
    }
    {
        uint16_t w[10000];
        srand(2);
        for (int i = 0; i < 10000; i++) w[i] = (uint16_t)(rand() & 0xFFFF);
        T("FP16 random 10K", test_roundtrip_fp16(w, 10000, "rand10k"));
    }
    {
        uint16_t w[V6B_SLOTS];
        srand(3);
        for (uint32_t i = 0; i < V6B_SLOTS; i++) w[i] = (uint16_t)(rand() & 0xFFFF);
        T("FP16 full grid 20736", test_roundtrip_fp16(w, V6B_SLOTS, "grid"));
    }
    {
        uint16_t w[5000];
        srand(4);
        for (int i = 0; i < 5000; i++) w[i] = (uint16_t)(rand() % 32);
        T("FP16 structured 5K", test_roundtrip_fp16(w, 5000, "structured"));
    }

    printf("\n[Streaming - chunked feed]\n"); fflush(stdout);
    {
        uint32_t n = 50000, chunk = 1000;
        int8_t *w = (int8_t *)malloc(n);
        srand(99);
        for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() % 256 - 128);

        v6b_stream_t st;
        v6b_init(&st, V6B_Q8);
        for (uint32_t i = 0; i < n; i += chunk)
            v6b_collect(&st, w + i, (i + chunk <= n) ? chunk : (n - i));
        uint32_t hdr = v6b_header(&st, NULL, 0);
        uint32_t nchunks = (n + V6B_SLOTS - 1) / V6B_SLOTS + 2;
        uint32_t need = hdr + nchunks * (4u + V6B_SLOTS * 6u + 64u);
        uint8_t *buf = (uint8_t *)malloc(need);
        v6b_header(&st, buf, need);
        uint32_t ew = 0, total_emit = 0;
        while ((ew = v6b_emit(&st, buf + hdr + total_emit, need - hdr - total_emit)) > 0)
            total_emit += ew;
        uint32_t total = hdr + total_emit;

        v6b_dec_t dec;
        v6b_dec_init(&dec, buf, total);
        int8_t *out = (int8_t *)malloc(n);
        int ok = 1;
        uint32_t got = 0, pos = 0;
        while ((got = v6b_dec_chunk(&dec, (uint8_t *)(out + pos), n - pos)) > 0)
            pos += got;
        ok = (pos == n) && (memcmp(out, w, n) == 0);
        printf("    streaming 50K in 1K chunks: %u B, %s\n", total, ok ? "ok" : "FAIL");
        T("streamed 50K matches original", ok);
        free(out); free(buf); free(w);
        v6b_dec_free(&dec); v6b_free(&st);
    }

    printf("\n==============================\n");
    printf("  FINAL: %d PASS / %d FAIL\n", pass, fail);
    printf("==============================\n");
    fflush(stdout);
    return fail;
}
