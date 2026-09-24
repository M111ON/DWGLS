/* gpu_bandwidth_bench.cu — measure effective bandwidth of this machine
 * ═══════════════════════════════════════════════════════════════════════════
 * Four paths, sweep 4 KB → 256 MB, report best GB/s (min-of-R per size):
 *   P1  H2D  pinned host  → device   (cudaMemcpy, pinned)
 *   P2  H2D  pageable host → device  (cudaMemcpy, plain malloc)
 *   P3  D2H  device → pinned host
 *   P4  D2D  device → device         (bus + VRAM path)
 *   P5  device read  (global load  → sum)
 *   P6  device write (store only)
 *   P7  device copy-in-kernel (read+write same buffer)
 * Also print theoretical device memory clock bandwidth for comparison.
 *
 * BUILD: cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\2022\
 *   BuildTools\VC\Auxiliary\Build\vcvars64.bat\" >nul 2>&1 && \
 *   I:\cuda_temp\bin\nvcc.exe -O2 -arch=sm_61 -o build\gpu_bandwidth_bench.exe \
 *   bench\gpu_bandwidth_bench.cu"
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cuda_runtime.h>

#define CUDA_OK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__, __LINE__, \
                cudaGetErrorString(_e)); \
        return 1; \
    } \
} while (0)

/* device read: stream through buffer, accumulate into out[0] */
__global__ void k_read(const uint8_t *src, uint64_t *acc, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    uint64_t sum = 0;
    for (; i < n; i += stride) sum += src[i];
    atomicAdd((unsigned long long *)acc, (unsigned long long)sum);
}

/* device write: touch every byte with a value derived from index */
__global__ void k_write(uint8_t *dst, size_t n, uint8_t seed)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = (uint8_t)(i ^ seed);
}

/* device copy: read + write same buffer (1 read B + 1 write B) */
__global__ void k_copy(uint8_t *buf, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    size_t stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) buf[i] = (uint8_t)(buf[i] + 1);
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

static const size_t kSizes[] = {
    4ull << 10, 16ull << 10, 64ull << 10, 256ull << 10,
    1ull << 20, 4ull << 20, 16ull << 20, 64ull << 20, 256ull << 20
};
static const int kNSz = (int)(sizeof kSizes / sizeof kSizes[0]);

static double gbs(double bytes, double ns)
{
    return bytes / (ns / 1e9) / 1e9;   /* GB/s (decimal) */
}

int main(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  GPU Bandwidth — GTX 1050 Ti\n");
    printf("═══════════════════════════════════════════════════\n");

    int ndev = 0;
    CUDA_OK(cudaGetDeviceCount(&ndev));
    if (ndev < 1) { printf("  no CUDA device — SKIP\n"); return 0; }
    cudaDeviceProp p{};
    CUDA_OK(cudaGetDeviceProperties(&p, 0));
    double memClockGBs = (double)p.memoryClockRate * 1000.0 *
                         (double)(p.memoryBusWidth / 8) * 2.0 / 1e9;
    printf("  device: %s (sm_%d%d)\n", p.name, p.major, p.minor);
    printf("  memory: %zu bus=%d-bit clock=%d kHz → theoretical %.1f GB/s "
           "(DDR)\n", p.totalGlobalMem, p.memoryBusWidth, p.memoryClockRate,
           memClockGBs);
    printf("\n");

    const size_t kMax = 256ull << 20;
    uint8_t *d_a = nullptr, *d_b = nullptr;
    void *h_pin = nullptr;
    uint8_t *h_page = nullptr;
    CUDA_OK(cudaMalloc(&d_a, kMax));
    CUDA_OK(cudaMalloc(&d_b, kMax));
    CUDA_OK(cudaMallocHost(&h_pin, kMax));
    h_page = (uint8_t *)malloc(kMax);
    if (!h_pin || !h_page) { fprintf(stderr, "host alloc fail\n"); return 1; }
    memset(h_pin, 0xA5, kMax);
    memset(h_page, 0x5A, kMax);
    CUDA_OK(cudaMemset(d_a, 0x11, kMax));
    CUDA_OK(cudaMemset(d_b, 0x22, kMax));

    cudaEvent_t ev0, ev1;
    CUDA_OK(cudaEventCreate(&ev0));
    CUDA_OK(cudaEventCreate(&ev1));

    double best_h2d_pin = 0, best_h2d_page = 0, best_d2h = 0, best_d2d = 0;
    double best_read = 0, best_write = 0, best_copy = 0;
    size_t best_h2d_pin_sz = 0, best_d2d_sz = 0, best_read_sz = 0;

    printf("  %8s %10s %10s %10s %8s %8s %8s %8s\n",
           "size", "h2d_pin", "h2d_pg", "d2h", "d2d", "read", "write", "copy");

    for (int si = 0; si < kNSz; si++) {
        size_t n = kSizes[si];
        uint32_t R = n <= (16ull << 20) ? 50 : 15;
        double t;

        /* P1 H2D pinned */
        cudaMemcpy(d_a, h_pin, n, cudaMemcpyHostToDevice);   /* warm */
        t = 1e18;
        for (uint32_t r = 0; r < R; r++) {
            double t0 = now_ns();
            CUDA_OK(cudaMemcpy(d_a, h_pin, n, cudaMemcpyHostToDevice));
            CUDA_OK(cudaDeviceSynchronize());
            t = (now_ns() - t0) < t ? (now_ns() - t0) : t;
        }
        /* recompute cleanly */
        t = 1e18;
        for (uint32_t r = 0; r < R; r++) {
            double t0 = now_ns();
            CUDA_OK(cudaMemcpy(d_a, h_pin, n, cudaMemcpyHostToDevice));
            CUDA_OK(cudaDeviceSynchronize());
            double dt = now_ns() - t0;
            if (dt < t) t = dt;
        }
        double v = gbs((double)n, t);
        if (v > best_h2d_pin) { best_h2d_pin = v; best_h2d_pin_sz = n; }

        /* P2 H2D pageable */
        t = 1e18;
        for (uint32_t r = 0; r < R; r++) {
            double t0 = now_ns();
            CUDA_OK(cudaMemcpy(d_a, h_page, n, cudaMemcpyHostToDevice));
            CUDA_OK(cudaDeviceSynchronize());
            double dt = now_ns() - t0;
            if (dt < t) t = dt;
        }
        double v2 = gbs((double)n, t);
        if (v2 > best_h2d_page) best_h2d_page = v2;

        /* P3 D2H pinned */
        t = 1e18;
        for (uint32_t r = 0; r < R; r++) {
            double t0 = now_ns();
            CUDA_OK(cudaMemcpy(h_pin, d_a, n, cudaMemcpyDeviceToHost));
            CUDA_OK(cudaDeviceSynchronize());
            double dt = now_ns() - t0;
            if (dt < t) t = dt;
        }
        double v3 = gbs((double)n, t);
        if (v3 > best_d2h) best_d2h = v3;

        /* P4 D2D */
        t = 1e18;
        for (uint32_t r = 0; r < R; r++) {
            double t0 = now_ns();
            CUDA_OK(cudaMemcpy(d_b, d_a, n, cudaMemcpyDeviceToDevice));
            CUDA_OK(cudaDeviceSynchronize());
            double dt = now_ns() - t0;
            if (dt < t) t = dt;
        }
        double v4 = gbs((double)n, t);
        if (v4 > best_d2d) { best_d2d = v4; best_d2d_sz = n; }

        /* P5 device read (events — GPU time only) */
        uint64_t *d_acc = nullptr;
        CUDA_OK(cudaMalloc(&d_acc, sizeof(uint64_t)));
        int blk = 256;
        int grd = (int)((n + blk - 1) / blk);
        if (grd > 1024) grd = 1024;
        double kr = 1e18;
        for (uint32_t r = 0; r < R; r++) {
            CUDA_OK(cudaMemset(d_acc, 0, sizeof(uint64_t)));
            CUDA_OK(cudaEventRecord(ev0));
            k_read<<<grd, blk>>>(d_a, d_acc, n);
            CUDA_OK(cudaEventRecord(ev1));
            CUDA_OK(cudaEventSynchronize(ev1));
            float ms = 0;
            CUDA_OK(cudaEventElapsedTime(&ms, ev0, ev1));
            double dt = (double)ms * 1e6;
            if (dt < kr) kr = dt;
        }
        double v5 = gbs((double)n, kr);
        if (v5 > best_read) { best_read = v5; best_read_sz = n; }

        /* P6 device write */
        double kw = 1e18;
        for (uint32_t r = 0; r < R; r++) {
            CUDA_OK(cudaEventRecord(ev0));
            k_write<<<grd, blk>>>(d_b, n, (uint8_t)r);
            CUDA_OK(cudaEventRecord(ev1));
            CUDA_OK(cudaEventSynchronize(ev1));
            float ms = 0;
            CUDA_OK(cudaEventElapsedTime(&ms, ev0, ev1));
            double dt = (double)ms * 1e6;
            if (dt < kw) kw = dt;
        }
        double v6 = gbs((double)n, kw);
        if (v6 > best_write) best_write = v6;

        /* P7 in-kernel copy = 1 read + 1 write B → 2× traffic */
        double kc = 1e18;
        for (uint32_t r = 0; r < R; r++) {
            CUDA_OK(cudaEventRecord(ev0));
            k_copy<<<grd, blk>>>(d_b, n);
            CUDA_OK(cudaEventRecord(ev1));
            CUDA_OK(cudaEventSynchronize(ev1));
            float ms = 0;
            CUDA_OK(cudaEventElapsedTime(&ms, ev0, ev1));
            double dt = (double)ms * 1e6;
            if (dt < kc) kc = dt;
        }
        double v7 = gbs(2.0 * (double)n, kc);   /* 2 sides */
        if (v7 > best_copy) best_copy = v7;

        CUDA_OK(cudaFree(d_acc));

        char lbl[16];
        if (n >= (1u << 20)) snprintf(lbl, sizeof lbl, "%zuM", n >> 20);
        else                  snprintf(lbl, sizeof lbl, "%zuK", n >> 10);
        printf("  %7s %9.2fG %9.2fG %9.2fG %7.2fG %7.2fG %7.2fG %7.2fG\n",
               lbl, v, v2, v3, v4, v5, v6, v7);
        fflush(stdout);
    }

    printf("\n  ── best (over all sizes) ──────────────────────\n");
    printf("  H2D  pinned  : %6.2f GB/s  (best @ %.0f MB)\n",
           best_h2d_pin, best_h2d_pin_sz / 1048576.0);
    printf("  H2D  pageable: %6.2f GB/s\n", best_h2d_page);
    printf("  D2H  pinned  : %6.2f GB/s\n", best_d2h);
    printf("  D2D          : %6.2f GB/s  (VRAM path, %.0f MB)\n",
           best_d2d, best_d2d_sz / 1048576.0);
    printf("  dev  read    : %6.2f GB/s  (L2-inflated ≤1 MB likely; "
           "best @ %.0f MB)\n", best_read, best_read_sz / 1048576.0);
    printf("  dev  write   : %6.2f GB/s\n", best_write);
    printf("  dev  r+w copy: %6.2f GB/s (2× traffic counted)\n", best_copy);
    printf("  theoretical  : %6.2f GB/s (mem clock × bus, DDR)\n",
           memClockGBs);
    printf("  PCIe note    : pinned H2D/D2H bound by PCIe link "
           "(x16 gen3 ≈ 12 GB/s ≈ 11.2 GiB/s)\n");

    CUDA_OK(cudaEventDestroy(ev0));
    CUDA_OK(cudaEventDestroy(ev1));
    CUDA_OK(cudaFree(d_a));
    CUDA_OK(cudaFree(d_b));
    CUDA_OK(cudaFreeHost(h_pin));
    free(h_page);
    printf("\n═══════════════════════════════════════════════════\n");
    return 0;
}
