/*
 * engine_bench.c — Geometric Engine vs Array Access Benchmark
 * ═══════════════════════════════════════════════════════════════════
 *
 * คำถาม: geometric decode (XOR) เร็วกว่า memory read ไหม?
 *
 * Test cases:
 *   1. array[i]          — baseline sequential read
 *   2. array[rand[i]]    — random access (realistic)
 *   3. XOR(x_i, y_i)     — geometric decode (no memory read for value)
 *   4. wave_encode + dec — full geometric roundtrip
 *
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Timer ───────────────────────────────────────────────────── */
static double now_sec(void)
{
    clock_t c = clock();
    return (double)c / (double)CLOCKS_PER_SEC;
}

/* ── Config ──────────────────────────────────────────────────── */
#define N_OPS 100000000   /* 100M operations */
#define WARMUP 3

/* ── Geometric decode (no memory read for value) ───────────────
 *
 *   Input: index i, stride s
 *   x_i = (i * s) % 360
 *   y_i = x_i ^ weight_i  (but we don't have weight_i!)
 *
 *   Actually: we precompute x_i,y_i from weight_i at "build time"
 *   and store ONLY y_i (x_i is derived from i).
 *
 *   For a fair comparison with array (1 byte read):
 *   - We store 1 byte per weight: y_i (the XOR pair)
 *   - x_i = (i * stride) % 360
 *   - weight = XOR(x_i, y_i)
 *
 *   vs array benchmark: 1 byte read per weight
 *   vs geom benchmark:  1 byte read + 1 XOR + 1 modulo
 *
 *   If we want ZERO storage:
 *   - x_i = (i * stride) % 360
 *   - y_i = (i * stride + bias) % 360
 *   - weight = XOR(x_i, y_i)
 *   - But this gives FIXED weights, not arbitrary ones.
 */

/* ── Variant A: Pure compute (no storage) ──────────────────────
 *    weight = XOR(f1(i), f2(i)) where f1,f2 are analytic
 *    ONLY works for structured models.
 *    Here we use: x_i = (i * 37) % 360, y_i = (i * 7 + 13) % 360
 */
static inline int32_t geom_compute_nostore(uint32_t i)
{
    uint16_t x = (uint16_t)((i * 37u) % 360u);
    uint16_t y = (uint16_t)((i * 7u + 13u) % 360u);
    int32_t w = (int32_t)(x ^ y);
    /* Bias to Q8 range (-128..127): compress 9-bit XOR to 8-bit */
    w = ((w ^ (w >> 1)) & 0xFF) - 128;
    return w;
}

/* ── Variant B: 1-byte stored pair + compute ───────────────────
 *    Store y_i only. x_i = (i * stride) % 360.
 *    weight = XOR(x_i, y_i)
 *    Storage: 1 byte/weight (same as Q8 array)
 *    Access: 1 memory read + 1 XOR + 1 modulo
 */
static inline int32_t geom_decode_stored(uint32_t i, const uint8_t *stored_y,
                                          uint16_t stride)
{
    uint16_t x = (uint16_t)((i * stride) % 360u);
    uint16_t y = (uint16_t)stored_y[i];
    return (int32_t)((uint8_t)(x ^ y)) - 128;
}


/* ══════════════════════════════════════════════════════════════════
   BENCHMARKS
   ══════════════════════════════════════════════════════════════════ */

/* Allocate aligned arrays */
static int32_t *arr_s8;
static uint8_t *arr_u8;
static uint8_t *stored_y;
static uint32_t *rand_idx;

static int bench_init(uint32_t n)
{
    arr_s8 = (int32_t*)malloc(n * sizeof(int32_t));
    arr_u8 = (uint8_t*)malloc(n * sizeof(uint8_t));
    stored_y = (uint8_t*)malloc(n * sizeof(uint8_t));
    rand_idx = (uint32_t*)malloc(n * sizeof(uint32_t));

    if (!arr_s8 || !arr_u8 || !stored_y || !rand_idx) return -1;

    /* Fill arrays */
    srand(42);
    for (uint32_t i = 0; i < n; i++) {
        int32_t w = (int32_t)(rand() % 256) - 128;  /* Q8 random */
        arr_s8[i] = w;
        arr_u8[i] = (uint8_t)(w + 128);
        /* Geometric: x_i = (i * 37) % 360, y_i = x_i ^ (w+128) */
        uint16_t x = (uint16_t)((i * 37u) % 360u);
        stored_y[i] = (uint8_t)(x ^ (uint16_t)(w + 128));
        rand_idx[i] = (uint32_t)((uint64_t)rand() * n / RAND_MAX);
    }
    return 0;
}

static void bench_free(void)
{
    free(arr_s8); free(arr_u8); free(stored_y); free(rand_idx);
}

/* ── B1: Sequential array read (int32) ──────────────────────── */
static double bench_seq_array_s8(uint32_t n)
{
    volatile int32_t sink = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        sink += arr_s8[i];
    }
    double t1 = now_sec();
    if (sink == 0xDEAD) printf(""); /* prevent optimize-out */
    return t1 - t0;
}

/* ── B2: Sequential array read (uint8) ───────────────────────── */
static double bench_seq_array_u8(uint32_t n)
{
    volatile int32_t sink = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        sink += (int32_t)arr_u8[i];
    }
    double t1 = now_sec();
    if (sink == 0xDEAD) printf("");
    return t1 - t0;
}

/* ── B3: Random access array read (uint8) ────────────────────── */
static double bench_rand_array_u8(uint32_t n)
{
    volatile int32_t sink = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        sink += (int32_t)arr_u8[rand_idx[i]];
    }
    double t1 = now_sec();
    if (sink == 0xDEAD) printf("");
    return t1 - t0;
}

/* ── B4: Geometric sequential (stored y + compute x) ──────────── */
static double bench_seq_geom(uint32_t n)
{
    volatile int32_t sink = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        uint16_t x = (uint16_t)((i * 37u) % 360u);
        uint16_t y = (uint16_t)stored_y[i];
        sink += (int32_t)((uint8_t)(x ^ y)) - 128;
    }
    double t1 = now_sec();
    if (sink == 0xDEAD) printf("");
    return t1 - t0;
}

/* ── B5: Geometric random (stored y + compute x) ─────────────── */
static double bench_rand_geom(uint32_t n)
{
    volatile int32_t sink = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = rand_idx[i];
        uint16_t x = (uint16_t)((idx * 37u) % 360u);
        uint16_t y = (uint16_t)stored_y[idx];
        sink += (int32_t)((uint8_t)(x ^ y)) - 128;
    }
    double t1 = now_sec();
    if (sink == 0xDEAD) printf("");
    return t1 - t0;
}

/* ── B6: Pure compute (no storage) — for comparison ──────────── */
static double bench_pure_compute(uint32_t n)
{
    volatile int32_t sink = 0;
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        sink += geom_compute_nostore(i);
    }
    double t1 = now_sec();
    if (sink == 0xDEAD) printf("");
    return t1 - t0;
}

/* ── B7: Wave encode + decode (full geometric roundtrip) ─────── */
static double bench_wave_roundtrip(uint32_t n)
{
    volatile int32_t sink = 0;
    /* Pre-gen data */
    int32_t *wv = (int32_t*)malloc(n * sizeof(int32_t));
    uint8_t *ph = (uint8_t*)malloc(n * sizeof(uint8_t));
    for (uint32_t i = 0; i < n; i++) {
        wv[i] = (int32_t)((int)(i % 256) - 128);
        ph[i] = (uint8_t)(i & 0xFF);
    }

    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        /* Encode: weight + phase → (x,y,layer) */
        uint8_t d = (uint8_t)((wv[i] < 0) ? -wv[i] : wv[i]);
        uint16_t x = (uint16_t)ph[i];
        uint16_t y = (uint16_t)(x ^ d);
        /* Decode: (x,y) → weight */
        sink += (int32_t)((uint8_t)(x ^ y)) - 128;
    }
    double t1 = now_sec();
    free(wv); free(ph);
    if (sink == 0xDEAD) printf("");
    return t1 - t0;
}


/* ══════════════════════════════════════════════════════════════════
   MAIN
   ══════════════════════════════════════════════════════════════════ */

int main(void)
{
    uint32_t n = N_OPS;

    printf("═══ Geometric Engine vs Array Access ═══\n");
    printf("  Operations: %u\n", n);
    printf("  Data type:  Q8 (-128..+127)\n");
    printf("  Geometry:   360×360 dual square\n\n");

    if (bench_init(n) != 0) {
        printf("FAIL: alloc\n");
        return 1;
    }

    /* Warmup */
    for (int w = 0; w < WARMUP; w++) {
        bench_seq_array_u8(1000000);
        bench_seq_geom(1000000);
    }

    printf("  %-40s %12s %12s\n", "Test", "Time (s)", "Ops/sec");
    printf("  " "----------------------------------------" " " "------------" " " "------------" "\n");

    /* SEQUENTIAL ACCESS */
    double t;

    t = bench_seq_array_s8(n);
    printf("  %-40s %12.4f %12.0f\n", "array[i] (int32, sequential)", t, (double)n/t);

    t = bench_seq_array_u8(n);
    printf("  %-40s %12.4f %12.0f\n", "array[i] (uint8, sequential)", t, (double)n/t);

    t = bench_seq_geom(n);
    printf("  %-40s %12.4f %12.0f\n", "geom decode (1B y_i store, seq)", t, (double)n/t);

    t = bench_wave_roundtrip(n);
    printf("  %-40s %12.4f %12.0f\n", "wave encode+decode (no store)", t, (double)n/t);

    t = bench_pure_compute(n);
    printf("  %-40s %12.4f %12.0f\n", "pure compute XOR(index) (0 store)", t, (double)n/t);

    printf("\n");

    /* RANDOM ACCESS */
    t = bench_rand_array_u8(n);
    printf("  %-40s %12.4f %12.0f\n", "array[rand] (uint8, random)", t, (double)n/t);

    t = bench_rand_geom(n);
    printf("  %-40s %12.4f %12.0f\n", "geom decode (rand, 1B store)", t, (double)n/t);

    printf("\n");

    /* Summary */
    printf("═══ Size comparison ═══\n");
    printf("  %-30s %12s\n", "Format", "Bytes/weight");
    printf("  %-30s %12d\n", "Q8 array (int32)", 4);
    printf("  %-30s %12d\n", "Q8 array (uint8)", 1);
    printf("  %-30s %12d\n", "Geom decode (1B y_i, compute x)", 1);
    printf("  %-30s %12s\n", "Pure compute (0 storage)", "0 (fixed)");

    bench_free();
    printf("\n✓ Benchmark complete\n");
    return 0;
}
