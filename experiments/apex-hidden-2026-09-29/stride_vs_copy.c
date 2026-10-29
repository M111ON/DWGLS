/* stride_vs_copy.c — on THIS machine: is trading a memory write for a multiply a win?
 *
 * move path  : memcpy(dst, src, n)          -> read n + write n
 * stride path: acc += (src[i] * inv) & 255  -> read n + n multiplies, no write
 *
 * If stride < memcpy, converting a move into a stride pays for itself here.
 * The multiply is also measured alone (hot L1 buffer) to show its share.
 *
 * build: gcc -O2 -o stride_vs_copy.exe stride_vs_copy.c
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double now(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
#include <time.h>
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
#endif

#define MB (1024u * 1024u)

static double bench_copy(const uint8_t *src, uint8_t *dst, size_t n, int reps) {
    double t0 = now();
    for (int r = 0; r < reps; r++) memcpy(dst, src, n);
    return (now() - t0) / reps;
}

static double bench_read(const uint8_t *src, size_t n, int reps, uint32_t *out) {
    uint32_t sink = 0;
    double t0 = now();
    for (int r = 0; r < reps; r++) {
        uint32_t acc = (uint32_t)r;                 /* rep-dependent: cannot be hoisted */
        for (size_t i = 0; i < n; i++) acc += src[i] ^ (uint8_t)r;
        sink += acc;
    }
    *out = sink;
    return (now() - t0) / reps;
}

static double bench_stride(const uint8_t *src, size_t n, int reps, uint32_t k, uint32_t *out) {
    uint32_t sink = 0;
    double t0 = now();
    for (int r = 0; r < reps; r++) {
        uint32_t acc = (uint32_t)r;
        uint32_t kk = k + (uint32_t)r;               /* rep-dependent: cannot be hoisted */
        for (size_t i = 0; i < n; i++) acc += ((uint32_t)src[i] * kk) & 255u;
        sink += acc;
    }
    *out = sink;
    return (now() - t0) / reps;
}

static double bench_mul_only(size_t n, int reps, uint32_t k, uint32_t *out) {
    static uint8_t tiny[4096];
    for (size_t i = 0; i < sizeof tiny; i++) tiny[i] = (uint8_t)(i * 37u);
    uint32_t sink = 0;
    double t0 = now();
    for (int r = 0; r < reps; r++) {
        uint32_t acc = (uint32_t)r;
        uint32_t kk = k + (uint32_t)r;
        for (size_t i = 0; i < n; i++) acc += ((uint32_t)tiny[i & 4095] * kk) & 255u;
        sink += acc;
    }
    *out = sink;
    return (now() - t0) / reps;
}

int main(void) {
    const size_t sizes[] = { 4 * MB, 16 * MB, 64 * MB };
    const int reps[] = { 40, 12, 4 };
    const uint32_t K = 173u;      /* 37^-1 mod 256 : the stride decode multiplier */
    uint32_t sink = 0;

    printf("%-8s %12s %12s %12s %12s | %s\n",
           "size", "memcpy", "read", "stride", "mul-only", "verdict");
    for (int s = 0; s < 3; s++) {
        size_t n = sizes[s];
        uint8_t *a = malloc(n), *b = malloc(n);
        if (!a || !b) { printf("alloc failed\n"); return 1; }
        for (size_t i = 0; i < n; i++) a[i] = (uint8_t)(i * 131u + 7u);

        double tc = bench_copy(a, b, n, reps[s]);
        double tr = bench_read(a, n, reps[s], &sink);
        double ts = bench_stride(a, n, reps[s], K, &sink);
        double tm = bench_mul_only(n, reps[s], K, &sink);

        double gb_c = (2.0 * n) / tc / 1e9;     /* memcpy moves 2n */
        double gb_s = (double)n / ts / 1e9;     /* stride reads n */
        printf("%-8zu %10.2fms %10.2fms %10.2fms %10.2fms | copy %.2f GB/s vs stride %.2f GB/s  %s\n",
               n / MB, tc * 1e3, tr * 1e3, ts * 1e3, tm * 1e3,
               gb_c, gb_s, ts < tc ? "STRIDE WINS" : "COPY WINS");
        free(a); free(b);
    }
    printf("\n(%u to keep the compiler honest)\n", sink);
    return 0;
}
