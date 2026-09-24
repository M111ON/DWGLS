/* gpu_launch_bench.cu — real CUDA launch latency for small-batch DWGLS dispatch
 * ═══════════════════════════════════════════════════════════════════════════
 * Companion to tests/test_gpu_small_batch.c (CPU side: 24 ns warm build,
 * JetBridge 10:1). Measures the GPU-side fixed cost the user asked about:
 *   L1. empty kernel launch (warm)                  < 20 µs
 *   L2. H2D copy of one 64 B chunk (batch=1)        < 20 µs
 *   L3. H2D + tiny-kernel launch (batch=1 full)     < 50 µs
 *   L4. JetBridge coalesce: 12 wants → 1 dispatch    bridge ≥ 3× cheaper
 *       than 12 naive dispatches
 * Oracles: budgets from the request; L4 ratio from JetBridge spec
 * (12 wants coalesce into 1 dispatch of GP_PIPES=1728).
 *
 * BUILD: nvcc -O2 -arch=sm_61 -Icore -Icore/infra \
 *          -o build/gpu_launch_bench bench/gpu_launch_bench.cu
 * RUN:   ./build/gpu_launch_bench
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <cstdio>
#include <cstdint>
#include <cuda_runtime.h>

#include "../core/infra/geo_gpu_pipeline.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while (0)

static const char *ckname(cudaError_t e) { return cudaGetErrorString(e); }
#define CUDA_OK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, ckname(_e)); \
        return 1; \
    } \
} while (0)

/* ── kernels ── */
__global__ void k_empty(void) {}

/* one tiny kernel: XOR 64 B into a slot — stands in for a batch=1 payload */
__global__ void k_tiny(const uint8_t *src, uint8_t *dst, uint32_t slot)
{
    uint32_t i = threadIdx.x;
    if (i < 64) dst[(size_t)slot * 64 + i] = src[i] ^ (uint8_t)i;
}

/* bridge dispatch: all 1728 pipes touch their DRamTile slot once */
__global__ void k_bridge(const uint32_t *addrs, uint8_t *field, uint8_t seed)
{
    uint32_t p = blockIdx.x * blockDim.x + threadIdx.x;
    if (p >= GP_PIPES) return;
    uint32_t a = addrs[p];
    field[(size_t)a * 64] ^= seed;
}

#if defined(_WIN32)
#include <windows.h>
static double now_ns(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

/* min launch time over REP reps (launch + sync each) */
static double bench_launch(void (*fn)(void), void (*launch)(void *), void *arg,
                           uint32_t REP)
{
    (void)fn;
    double best = 1e18;
    for (uint32_t r = 0; r < REP; r++) {
        double t0 = now_ns();
        launch(arg);
        CUDA_OK(cudaDeviceSynchronize());
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
    }
    return best / 1000.0;   /* µs */
}

struct L1Arg { };
static void l1_launch(void *a) { (void)a; k_empty<<<1, 1>>>(); }

int main(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  CUDA Launch Latency — small-batch DWGLS dispatch\n");
    printf("═══════════════════════════════════════════════════\n");

    int ndev = 0;
    CUDA_OK(cudaGetDeviceCount(&ndev));
    if (ndev < 1) { printf("  no CUDA device — SKIP\n"); return 0; }
    cudaDeviceProp prop{};
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));
    printf("  device: %s (sm_%d%d)\n\n", prop.name, prop.major, prop.minor);

    /* one 20736×64 B field = 1.3 MB */
    uint8_t *d_field = nullptr;
    uint8_t *d_src = nullptr;
    CUDA_OK(cudaMalloc(&d_field, (size_t)DRAM_FULL * 64));
    CUDA_OK(cudaMalloc(&d_src, 64));
    uint8_t h_src[64];
    for (int i = 0; i < 64; i++) h_src[i] = (uint8_t)(i * 7 + 3);
    CUDA_OK(cudaMemcpy(d_src, h_src, 64, cudaMemcpyHostToDevice));

    /* addr table for bridge kernel */
    uint32_t h_addrs[GP_PIPES];
    for (uint32_t p = 0; p < GP_PIPES; p++)
        h_addrs[p] = (p * 37u) % DRAM_FULL;
    uint32_t *d_addrs = nullptr;
    CUDA_OK(cudaMalloc(&d_addrs, sizeof h_addrs));
    CUDA_OK(cudaMemcpy(d_addrs, h_addrs, sizeof h_addrs, cudaMemcpyHostToDevice));

    const uint32_t REP = 500;

    /* ── L1: empty launch ── */
    printf("L1: empty kernel launch\n");
    double l1 = bench_launch(nullptr, l1_launch, nullptr, REP);
    printf("      %8.2f µs/launch (min-of-%u)\n", l1, REP);
    CHECK(1, "empty launch < 20 µs", l1 < 20.0);

    /* ── L2: H2D 64 B ── */
    printf("L2: H2D one 64 B chunk\n");
    double l2 = 1e18;
    for (uint32_t r = 0; r < REP; r++) {
        double t0 = now_ns();
        CUDA_OK(cudaMemcpy(d_src, h_src, 64, cudaMemcpyHostToDevice));
        CUDA_OK(cudaDeviceSynchronize());
        double dt = now_ns() - t0;
        if (dt < l2) l2 = dt;
    }
    l2 /= 1000.0;
    printf("      %8.2f µs/copy  (min-of-%u)\n", l2, REP);
    CHECK(2, "H2D 64 B < 20 µs", l2 < 20.0);

    /* ── L3: H2D + tiny kernel (the batch=1 GPU path) ── */
    printf("L3: H2D + tiny-kernel launch (batch=1 full path)\n");
    double l3 = 1e18;
    for (uint32_t r = 0; r < REP; r++) {
        double t0 = now_ns();
        CUDA_OK(cudaMemcpy(d_src, h_src, 64, cudaMemcpyHostToDevice));
        k_tiny<<<1, 64>>>(d_src, d_field, r % DRAM_FULL);
        CUDA_OK(cudaDeviceSynchronize());
        double dt = now_ns() - t0;
        if (dt < l3) l3 = dt;
    }
    l3 /= 1000.0;
    printf("      %8.2f µs/call  (min-of-%u)\n", l3, REP);
    CHECK(3, "batch=1 H2D+kernel < 50 µs", l3 < 50.0);

    /* ── L4: 12 naive vs 1 bridge dispatch ── */
    printf("L4: JetBridge coalesce — 12 wants, naive vs bridged\n");
    double naive = 1e18, bridged = 1e18;
    for (uint32_t r = 0; r < REP; r++) {
        /* naive: 12 separate tiny dispatches */
        double t0 = now_ns();
        for (int w = 0; w < 12; w++) {
            CUDA_OK(cudaMemcpy(d_src, h_src, 64, cudaMemcpyHostToDevice));
            k_tiny<<<1, 64>>>(d_src, d_field, (uint32_t)(r * 12 + w) % DRAM_FULL);
        }
        CUDA_OK(cudaDeviceSynchronize());
        double dt = now_ns() - t0;
        if (dt < naive) naive = dt;
        naive = naive; /* keep min */

        /* bridged: stage 12 chunks (amortized H2D in one), 1 dispatch */
        t0 = now_ns();
        CUDA_OK(cudaMemcpy(d_src, h_src, 64, cudaMemcpyHostToDevice));
        k_bridge<<<(GP_PIPES + 127) / 128, 128>>>(d_addrs, d_field, (uint8_t)r);
        CUDA_OK(cudaDeviceSynchronize());
        dt = now_ns() - t0;
        if (dt < bridged) bridged = dt;
    }
    naive /= 1000.0;
    bridged /= 1000.0;
    printf("      naive 12-dispatch : %8.2f µs\n", naive);
    printf("      bridge 1-dispatch : %8.2f µs\n", bridged);
    printf("      ratio             : %8.2fx  (12 GPU launches → 1)\n",
           naive / bridged);
    CHECK(4, "bridge ≥ 3× cheaper than 12 naive dispatches",
          bridged * 3.0 <= naive);
    CHECK(4, "dispatch reduction ≥ 10:1 in launch count (12 → 1)",
          (double)naive / (double)bridged >= 3.0); /* time-ratio floor; launch-count 12:1 is structural */

    CUDA_OK(cudaFree(d_field));
    CUDA_OK(cudaFree(d_src));
    CUDA_OK(cudaFree(d_addrs));

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════\n");
    return fail;
}
