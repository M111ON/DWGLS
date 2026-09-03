/* test_kis_bench.c — KIS codec v4/v6 benchmark on 6ico field (20736) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kis_codec_v4.h"
#include "kis_codec_v6.h"
#include "kis_codec_v6b.h"

#define FIELD 20736
#define CUBE  144
#define TESS  1152
#define ITERS 100

static void fill_i8(int8_t *buf, uint32_t n, uint32_t seed) {
    uint32_t s = seed;
    for (uint32_t i = 0; i < n; i++) {
        s = s * 1103515245 + 12345;
        buf[i] = (int8_t)(s & 0xFF);
    }
}

static double elapsed_ms(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

typedef struct {
    const char *name;
    uint32_t size;
    double encode_ms;
    double decode_ms;
    uint32_t enc_bytes;
    double ratio;
} BenchResult;

static BenchResult bench_codec(const char *name, uint32_t n, int seed) {
    BenchResult r = {0};
    r.name = name;
    r.size = n;

    int8_t orig[FIELD] = {0};
    int8_t dec[FIELD] = {0};
    uint8_t buf[FIELD * 3] = {0};

    fill_i8(orig, n, seed);

    struct timespec t0, t1;

    /* warm up */
    uint32_t enc = 0;
    if (strcmp(name, "v4") == 0) {
        enc = kis_v4_encode(orig, n, buf, sizeof(buf));
        kis_v4_decode(buf, enc, dec, n);
    } else {
        enc = v6_encode(orig, n, buf, sizeof(buf));
        v6_decode(buf, enc, dec, n);
    }

    /* encode benchmark */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERS; i++) {
        if (strcmp(name, "v4") == 0)
            enc = kis_v4_encode(orig, n, buf, sizeof(buf));
        else
            enc = v6_encode(orig, n, buf, sizeof(buf));
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r.encode_ms = elapsed_ms(t0, t1) / ITERS;
    r.enc_bytes = enc;
    r.ratio = (double)enc / n;

    /* decode benchmark */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERS; i++) {
        if (strcmp(name, "v4") == 0)
            kis_v4_decode(buf, enc, dec, n);
        else
            v6_decode(buf, enc, dec, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r.decode_ms = elapsed_ms(t0, t1) / ITERS;

    return r;
}

static BenchResult bench_v6b(uint32_t n, int seed) {
    BenchResult r = {0};
    r.name = "v6b";
    r.size = n;

    int8_t orig[FIELD] = {0};
    int8_t dec[FIELD] = {0};
    uint32_t buf_sz = 4 + n * (5u + 1u) + 4096;
    uint8_t *buf = (uint8_t *)malloc(buf_sz);

    fill_i8(orig, n, seed);

    struct timespec t0, t1;

    /* warm up */
    v6b_stream_t st;
    v6b_init(&st, V6B_Q8);
    v6b_collect(&st, orig, n);
    uint32_t hw = v6b_header(&st, buf, buf_sz);
    uint32_t ew = 0, total_emit = 0;
    while ((ew = v6b_emit(&st, buf + hw + total_emit, buf_sz - hw - total_emit)) > 0)
        total_emit += ew;
    uint32_t total = hw + total_emit;
    v6b_decode_all(buf, total, (uint8_t *)dec, n);
    v6b_free(&st);

    /* encode benchmark (full streaming: init+collect+header+emit) */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERS; i++) {
        v6b_stream_t s;
        v6b_init(&s, V6B_Q8);
        v6b_collect(&s, orig, n);
        uint32_t h = v6b_header(&s, buf, buf_sz);
        uint32_t e = 0, te = 0;
        while ((e = v6b_emit(&s, buf + h + te, buf_sz - h - te)) > 0) te += e;
        total = h + te;
        v6b_free(&s);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r.encode_ms = elapsed_ms(t0, t1) / ITERS;
    r.enc_bytes = total;
    r.ratio = (double)total / n;

    /* decode benchmark */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < ITERS; i++) {
        v6b_decode_all(buf, total, (uint8_t *)dec, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    r.decode_ms = elapsed_ms(t0, t1) / ITERS;

    free(buf);
    return r;
}

static void print_row(BenchResult *r) {
    printf("  %-8s %6u  %8.3f ms  %8.3f ms  %8u B  %5.2fx  %8.0f MB/s  %8.0f MB/s\n",
           r->name, r->size,
           r->encode_ms, r->decode_ms,
           r->enc_bytes, r->ratio,
           (double)r->size / r->encode_ms / 1000.0,
           (double)r->size / r->decode_ms / 1000.0);
}

int main(void) {
    printf("KIS Codec Benchmark (v4 vs v6 vs v6b, 6ico field, %d iters)\n", ITERS);
    printf("═══════════════════════════════════════════════════════════════════════\n");
    printf("  %-8s %6s  %11s  %11s  %10s  %5s  %11s  %11s\n",
           "Codec", "Size", "Encode", "Decode", "Enc bytes", "Ratio", "Enc BW", "Dec BW");
    printf("  %-8s %6s  %11s  %11s  %10s  %5s  %11s  %11s\n",
           "──────", "─────", "───────────", "───────────", "──────────", "─────", "───────────", "───────────");

    /* bench at different sizes */
    uint32_t sizes[] = {CUBE, TESS, FIELD};
    const char *labels[] = {"cube(144)", "tess(1152)", "field(20736)"};

    for (int s = 0; s < 3; s++) {
        uint32_t n = sizes[s];
        BenchResult v4 = bench_codec("v4", n, 42 + s);
        BenchResult v6 = bench_codec("v6", n, 42 + s);
        BenchResult vb = bench_v6b(n, 42 + s);

        printf("\n  [%s]\n", labels[s]);
        print_row(&v4);
        print_row(&v6);
        print_row(&vb);
    }

    printf("\n═══════════════════════════════════════════════════════════════════════\n");

    /* throughput summary for full field */
    printf("\n  Full field (20736 int8) summary:\n");
    BenchResult v4f = bench_codec("v4", FIELD, 99);
    BenchResult v6f = bench_codec("v6", FIELD, 99);
    BenchResult vbf = bench_v6b(FIELD, 99);
    printf("    v4:  encode %.1f ms → decode %.1f ms (total %.1f ms)\n",
           v4f.encode_ms, v4f.decode_ms, v4f.encode_ms + v4f.decode_ms);
    printf("    v6:  encode %.1f ms → decode %.1f ms (total %.1f ms)\n",
           v6f.encode_ms, v6f.decode_ms, v6f.encode_ms + v6f.decode_ms);
    printf("    v6b: encode %.1f ms → decode %.1f ms (total %.1f ms)\n",
           vbf.encode_ms, vbf.decode_ms, vbf.encode_ms + vbf.decode_ms);
    printf("    v4 enc size: %u B (%.2fx), v6 enc size: %u B (%.2fx), v6b enc size: %u B (%.2fx)\n",
           v4f.enc_bytes, v4f.ratio, v6f.enc_bytes, v6f.ratio, vbf.enc_bytes, vbf.ratio);

    return 0;
}
