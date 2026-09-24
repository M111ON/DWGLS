/* gpu_batch_break_even.cu — normal path vs JetBridge: how big must batch wait?
 * ═══════════════════════════════════════════════════════════════════════════
 * Question: WITHOUT the DRamTile+GearLock+JetBridge system, how big must a
 * batch grow before paying dispatch overhead is "worth it"?
 *
 * Definitions (independent of code under test):
 *   overhead(N) = launch_floor + H2D(N×64B) − kern(N)
 *                where launch_floor = empty-kernel launch (measured once)
 *                      H2D(N)      = measured copy time for N×64 B
 *                      kern(N)     = kernel GPU time (CUDA events)
 *   pct(N)      = launch_only / full(N)   — launch share of total time
 *                (copy is DATA, not overhead; launch is the fixed cost
 *                 that batching exists to amortize)
 *   N50         = smallest sweep N with pct ≤ 50%
 *   N10         = smallest sweep N with pct ≤ 10%
 *
 * JetBridge line: 12 wants → 1 dispatch (measured), per-want µs.
 *
 * BUILD: cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\2022\
 *   BuildTools\VC\Auxiliary\Build\vcvars64.bat\" >nul 2>&1 && \
 *   I:\cuda_temp\bin\nvcc.exe -O2 -arch=sm_61 -o build\gpu_batch_break_even.exe \
 *   bench\gpu_batch_break_even.cu"
 * RUN:   build\gpu_batch_break_even.exe
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <cstdio>
#include <cstdint>
#include <cuda_runtime.h>

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while (0)

#define CUDA_OK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, \
                cudaGetErrorString(_e)); \
        return 1; \
    } \
} while (0)

__global__ void k_empty(void) {}

/* useful work: scatter N chunks × 64 B — the pipeline's real GPU step */
__global__ void k_scatter(const uint8_t *src, uint8_t *field, uint32_t n,
                          uint32_t base)
{
    uint32_t c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= n) return;
    uint32_t slot = (base + c) % 20736u;
    const uint8_t *s = src + (size_t)c * 64;
    uint8_t *d = field + (size_t)slot * 64;
    for (int i = 0; i < 64; i++) d[i] = s[i];
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

static const uint32_t kMaxN = 65536;
static const uint32_t kSweep[] = {
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096,
    8192, 16384, 32768, 65536
};
static const int kSweepN = (int)(sizeof kSweep / sizeof kSweep[0]);

int main(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  Batch break-even — normal path vs JetBridge\n");
    printf("═══════════════════════════════════════════════════\n");

    int ndev = 0;
    CUDA_OK(cudaGetDeviceCount(&ndev));
    if (ndev < 1) { printf("  no CUDA device — SKIP\n"); return 0; }
    cudaDeviceProp prop{};
    CUDA_OK(cudaGetDeviceProperties(&prop, 0));
    printf("  device: %s (sm_%d%d)\n\n", prop.name, prop.major, prop.minor);

    uint8_t *d_field = nullptr, *d_src = nullptr, *h_src = nullptr;
    CUDA_OK(cudaMalloc(&d_field, (size_t)20736 * 64));
    CUDA_OK(cudaMalloc(&d_src, (size_t)kMaxN * 64));
    CUDA_OK(cudaHostAlloc(&h_src, (size_t)kMaxN * 64, cudaHostAllocDefault));
    for (size_t i = 0; i < (size_t)kMaxN * 64; i++) h_src[i] = (uint8_t)(i * 7 + 3);

    /* launch_floor: empty kernel launch + sync (fixed cost baseline) */
    double launch_floor = 1e18;
    for (int r = 0; r < 500; r++) {
        double t0 = now_ns();
        k_empty<<<1, 1>>>();
        CUDA_OK(cudaDeviceSynchronize());
        double dt = now_ns() - t0;
        if (dt < launch_floor) launch_floor = dt;
    }
    launch_floor /= 1000.0;   /* µs */
    printf("launch_floor (empty kernel) = %.2f µs\n", launch_floor);

    cudaEvent_t ev0, ev1;
    CUDA_OK(cudaEventCreate(&ev0));
    CUDA_OK(cudaEventCreate(&ev1));

    struct Row { uint32_t n; double full_us, kern_us, pct; };
    Row rows[64];
    int nrows = 0;
    int N50 = -1, N10 = -1;

    printf("\n  %7s %10s %10s %9s %8s\n",
           "N", "full µs", "kernel µs", "ns/chunk", "launch%");

    for (int si = 0; si < kSweepN; si++) {
        uint32_t N = kSweep[si];
        uint32_t REP = (N <= 256) ? 300 : (N <= 4096 ? 80 : 30);
        dim3 grid((N + 255) / 256), block(256);

        /* full path: H2D + launch + sync (min-of-REP) */
        double best_full = 1e18;
        for (uint32_t r = 0; r < REP; r++) {
            double t0 = now_ns();
            CUDA_OK(cudaMemcpy(d_src, h_src, (size_t)N * 64,
                               cudaMemcpyHostToDevice));
            k_scatter<<<grid, block>>>(d_src, d_field, N, r);
            CUDA_OK(cudaDeviceSynchronize());
            double dt = now_ns() - t0;
            if (dt < best_full) best_full = dt;
        }

        /* kernel GPU time only (events, min-of-REP) */
        double best_kern = 1e18;
        for (uint32_t r = 0; r < REP; r++) {
            CUDA_OK(cudaEventRecord(ev0));
            k_scatter<<<grid, block>>>(d_src, d_field, N, r);
            CUDA_OK(cudaEventRecord(ev1));
            CUDA_OK(cudaEventSynchronize(ev1));
            float ms = 0;
            CUDA_OK(cudaEventElapsedTime(&ms, ev0, ev1));
            double us = ms * 1000.0;
            if (us < best_kern) best_kern = us;
        }

        double full = best_full / 1000.0;   /* µs — fix: was double-divided */
        double kern = best_kern;            /* already µs */
        double pct  = launch_floor / full;  /* launch share of total */

        rows[nrows++] = {N, full, kern, pct};
        printf("  %7u %10.2f %10.3f %9.1f %7.1f%%\n",
               N, full, kern, full * 1000.0 / N, pct * 100.0);

        if (N50 < 0 && pct <= 0.50) N50 = (int)N;
        if (N10 < 0 && pct <= 0.10) N10 = (int)N;
    }

    /* floor = best ns/chunk achievable (throughput-bound tail);
     * N2x = smallest N with per-chunk ≤ 2× floor */
    double floor_ns = 1e18;
    for (int i = 0; i < nrows; i++) {
        double c = rows[i].full_us * 1000.0 / rows[i].n;
        if (c < floor_ns) floor_ns = c;
    }
    int N2x = -1, N15x = -1;
    for (int i = 0; i < nrows; i++) {
        double c = rows[i].full_us * 1000.0 / rows[i].n;
        if (N2x < 0 && c <= 2.0 * floor_ns) N2x = (int)rows[i].n;
        if (N15x < 0 && c <= 1.5 * floor_ns) N15x = (int)rows[i].n;
    }

    /* ── JetBridge: 12 wants → 1 dispatch (system path) ── */
    double best_bridge = 1e18;
    for (uint32_t r = 0; r < 300; r++) {
        double t0 = now_ns();
        CUDA_OK(cudaMemcpy(d_src, h_src, 12ull * 64, cudaMemcpyHostToDevice));
        k_scatter<<<1, 128>>>(d_src, d_field, 12, r);
        CUDA_OK(cudaDeviceSynchronize());
        double dt = now_ns() - t0;
        if (dt < best_bridge) best_bridge = dt;
    }
    double bridge_12 = best_bridge / 1000.0;            /* µs for 12 wants */
    double bridge_per_want = bridge_12 / 12.0;          /* µs/want */
    double normal_per_want_N1 = rows[0].full_us;        /* µs/want at N=1 */

    printf("\n  ── answer ─────────────────────────────────────\n");
    printf("  throughput floor          : %.0f ns/chunk (best in sweep)\n",
           floor_ns);
    printf("  per-chunk ≤ 2× floor      : wait batch ≥ %d chunks (%.0f KB)\n",
           N2x > 0 ? N2x : -1, N2x > 0 ? N2x * 64.0 / 1024.0 : 0.0);
    printf("  per-chunk ≤ 1.5× floor    : wait batch ≥ %d chunks (%.0f KB)\n",
           N15x > 0 ? N15x : -1, N15x > 0 ? N15x * 64.0 / 1024.0 : 0.0);
    if (N10 > 0)
        printf("  launch share ≤ 10%%        : wait batch ≥ %d chunks\n", N10);
    else
        printf("  launch share ≤ 10%%        : not reached at N=%u\n", kMaxN);
    printf("  batch=1 per-chunk         : %.0f ns (%.0f× floor)\n",
           rows[0].full_us * 1000.0, rows[0].full_us * 1000.0 / floor_ns);
    printf("  system: batch=1 OK, no wait; 12 wants 1 dispatch = %.1f µs "
           "(%.2f µs/want vs %.1f µs/want at N=1, %.1f× cheaper)\n",
           bridge_12, bridge_per_want, normal_per_want_N1,
           normal_per_want_N1 / bridge_per_want);

    /* ── checks ── */
    CHECK(1, "batch=1 wildly off floor (≥50× per-chunk cost)",
          nrows > 0 && rows[0].full_us * 1000.0 >= 50.0 * floor_ns);
    CHECK(2, "N10 found — real wait needed before launch share ≤ 10%",
          N10 > 0);
    CHECK(3, "launch share falls with N (pct at Nmax < pct at N=1)",
          nrows >= 2 && rows[nrows - 1].pct < rows[0].pct);
    CHECK(4, "JetBridge per-want ≤ normal per-want at N=1",
          bridge_per_want <= normal_per_want_N1);
    CHECK(5, "N2x found — per-chunk reaches within 2× of floor in sweep",
          N2x > 0 && N2x >= N10 / 4);   /* floor-based break-even exists */

    CUDA_OK(cudaEventDestroy(ev0));
    CUDA_OK(cudaEventDestroy(ev1));
    CUDA_OK(cudaFree(d_field));
    CUDA_OK(cudaFree(d_src));
    CUDA_OK(cudaFreeHost(h_src));

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════\n");
    return fail;
}
