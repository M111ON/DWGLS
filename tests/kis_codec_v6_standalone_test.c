/* kis_codec_v6_standalone_test.c — Roundtrip without gguf_reader.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "core/kis_codec_v6.h"

static int pass_count = 0, fail_count = 0;
#define T(n, desc, ok) do { \
    if (ok) { pass_count++; printf("  T%d: PASS — %s\n", n, desc); } \
    else    { fail_count++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

static int test_roundtrip(const char *name, const int8_t *w, uint32_t n) {
    uint32_t buf_size = n * 2 + 4096;
    uint8_t *buf = (uint8_t *)malloc(buf_size);
    int8_t *out = (int8_t *)malloc(n);
    if (!buf || !out) { free(buf); free(out); return 0; }

    clock_t t0 = clock();
    uint32_t enc = v6_encode(w, n, buf, buf_size);
    clock_t t1 = clock();
    int dec = v6_decode(buf, enc, out, n);
    clock_t t2 = clock();

    uint64_t mm = 0;
    for (uint32_t i = 0; i < n; i++) if (w[i] != out[i]) mm++;

    double ratio = enc > 0 ? (double)enc / n : 0.0;
    double enc_ms = (double)(t1 - t0) / CLOCKS_PER_SEC * 1000;
    double dec_ms = (double)(t2 - t1) / CLOCKS_PER_SEC * 1000;
    printf("    %s: %u/%uB ratio=%.4fx mm=%lu enc=%.1fms dec=%.1fms\n",
           name, enc, n, ratio, (unsigned long)mm, enc_ms, dec_ms);

    free(buf); free(out);
    return (dec == 0 && mm == 0);
}

/* Verify v6_slot is a bijection on [0, 20736) */
static int test_slot_bijection(void) {
    uint8_t *seen = (uint8_t *)calloc(V6_SLOTS, 1);
    if (!seen) return 0;
    for (uint32_t i = 0; i < V6_SLOTS; i++) {
        uint32_t s = v6_slot(i);
        if (s >= V6_SLOTS || seen[s]) { free(seen); return 0; }
        seen[s] = 1;
    }
    free(seen);
    return 1;
}

/* Production API test */
static int test_production_api(void) {
    int8_t w[500];
    for (int i = 0; i < 500; i++) w[i] = (int8_t)(i * 73 % 256 - 128);

    uint32_t enc = v6_encode_buf(w, 500, NULL, 0);
    if (enc == 0) return 0;

    uint8_t *buf = (uint8_t *)malloc(enc);
    int8_t *out = (int8_t *)malloc(500);
    if (!buf || !out) { free(buf); free(out); return 0; }

    uint32_t written = v6_encode_buf(w, 500, buf, enc);
    if (written == 0) { free(buf); free(out); return 0; }

    int rc = v6_decode_buf(buf, written, out, 500);
    if (rc != 0) { free(buf); free(out); return 0; }

    int match = (memcmp(w, out, 500) == 0);
    float ratio = v6_ratio(buf, written, 500);
    printf("    api: enc=%uB ratio=%.4fx verify=%s\n",
           written, ratio, v6_verify(buf, written, out, 500) ? "ok" : "FAIL");

    free(buf); free(out);
    return match;
}

int main(void) {
    printf("╔══ KIS CODEC v6 — Standalone Roundtrip ══╗\n\n");

    /* T0: Slot bijection */
    printf("═══ Slot Bijection ═══\n");
    T(0, "v6_slot is bijection on [0,20736)", test_slot_bijection());

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
        for (int i = 0; i < 10000; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T(3, "Random Q8 range", test_roundtrip("rand10k", w, 10000));
    }
    {
        int8_t w[4] = {-128, -1, 0, 127};
        T(4, "Edge values", test_roundtrip("edge", w, 4));
    }
    {
        int8_t w[V6_SLOTS];
        srand(99);
        for (uint32_t i = 0; i < V6_SLOTS; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T(5, "One full grid (20736)", test_roundtrip("grid", w, V6_SLOTS));
    }
    {
        uint32_t n = V6_SLOTS * 2 + 1;
        int8_t *w = (int8_t *)malloc(n);
        srand(777);
        for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T(6, "Over-chunk (41473)", test_roundtrip("over", w, n));
        free(w);
    }
    {
        /* Large: 1M weights */
        uint32_t n = 1000000;
        int8_t *w = (int8_t *)malloc(n);
        srand(42);
        for (uint32_t i = 0; i < n; i++) w[i] = (int8_t)(rand() % 256 - 128);
        T(7, "1M random weights", test_roundtrip("1m", w, n));
        free(w);
    }

    /* Production API */
    printf("\n═══ Production API ═══\n");
    T(10, "v6_encode_buf/v6_decode_buf/v6_verify", test_production_api());

    printf("\n══════════════════════════════\n");
    printf("  FINAL: %d PASS / %d FAIL\n", pass_count, fail_count);
    printf("══════════════════════════════\n");
    return fail_count;
}
