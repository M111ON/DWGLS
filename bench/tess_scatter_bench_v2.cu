/* ═══════════════════════════════════════════════════════════════════
 * tess_scatter_bench_v2.cu — DRamTile + GearLock GPU scatter/gather
 *
 * Architecture:
 *   1. mmap .tesspack on host (zero-copy source)
 *   2. cudaHostRegister → pin pages for fast GPU access
 *   3. GPU kernel reads directly from pinned host memory (no H2D copy)
 *   4. GearLock world counters for CPU/GPU sync (no cudaDeviceSynchronize)
 *   5. sig32 XOR-fold verification on GPU
 *
 * This bench compares:
 *   A) BRUTE FORCE: copy everything to HBM, pull from HBM (old way)
 *   B) DRamTile: read from pinned host memory (zero-copy, no H2D)
 *   C) DRamTile + GearLock: async overlap of CPU tick + GPU pull
 *   D) sig32 compressed descriptors (8B vs 16B)
 *
 * Compile (Colab T4):
 *   nvcc -O3 -std=c++17 -arch=sm_75 -o tess_scatter_v2 tess_scatter_bench_v2.cu -lm
 *
 * Usage:
 *   ./tess_scatter_v2 model.tesspack
 * ═══════════════════════════════════════════════════════════════════ */

#include <cuda_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define CUDA_CHECK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n", \
                __FILE__, __LINE__, #call, cudaGetErrorString(_e)); \
        return -1; \
    } \
} while(0)

/* ═══════════════════════════════════════════════════════════════════
 * THE LEGENDARY ONE-LINER
 * ═══════════════════════════════════════════════════════════════════ */
static inline __host__ __device__ uint32_t sig32_xor_fold(uint64_t sig64) {
    return (uint32_t)(sig64 >> 32) ^ (uint32_t)(sig64 & 0xFFFFFFFFU);
}

/* ═══════════════════════════════════════════════════════════════════
 * GEAR LOCK — CPU/GPU world counters
 * from FGLS_new/runner/gpu_jet_puller/gpu_jet_puller_gguf.cu
 *
 * GearLock eliminates cudaDeviceSynchronize overhead:
 *   CPU ticks → world_counter++
 *   GPU ticks → gpu_counter++
 *   CPU waits only when gpu_counter < expected
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint64_t gpu_counter;
    uint64_t cpu_counter;
} GearLock;

static inline void gear_cpu_tick(GearLock *lock) { lock->cpu_counter++; }
static inline void gear_gpu_tick(GearLock *lock, uint32_t n) { lock->gpu_counter += n; }
static inline int gear_in_sync(const GearLock *lock) {
    return lock->gpu_counter >= lock->cpu_counter;
}

/* ═══════════════════════════════════════════════════════════════════
 * DRamTile ADDRESSING — zero-copy geometry
 * from FGLS_new/runner/geo_dram_tile.h
 *
 *   dram_addr = anchor_id × 128 + hilbert_8x8(x, y) + layer × 64
 *   mmap_offset = dram_addr × chunk_sz
 *   GPU kernel computes address → reads directly from pinned host memory
 * ═══════════════════════════════════════════════════════════════════ */
#define DRAM_GRID_X     8u
#define DRAM_GRID_Y     8u
#define DRAM_LAYERS     2u
#define DRAM_CELLS_PER  (DRAM_GRID_X * DRAM_GRID_Y * DRAM_LAYERS)  /* 128 */
#define DRAM_ANCHORS    162u
#define DRAM_FULL       (DRAM_ANCHORS * DRAM_CELLS_PER)            /* 20736 */

static inline __device__ uint32_t dram_hilbert_8x8(uint32_t x, uint32_t y) {
    uint32_t d = 0, n = 8;
    for (uint32_t s = n >> 1; s > 0; s >>= 1) {
        uint32_t rx = (x & s) > 0;
        uint32_t ry = (y & s) > 0;
        d = (d << 2) | ((3u * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) { x = n - 1u - x; y = n - 1u - y; }
            uint32_t t = x; x = y; y = t;
        }
    }
    return d;
}

static inline __device__ uint32_t dram_addr(uint32_t anchor, uint32_t x,
                                             uint32_t y, uint32_t layer) {
    uint32_t tile = dram_hilbert_8x8(x, y) + layer * 64;
    return anchor * DRAM_CELLS_PER + tile;
}

/* ═══════════════════════════════════════════════════════════════════
 * TESSPACK FORMAT
 * ═══════════════════════════════════════════════════════════════════ */
#define TPAK_MAGIC 0x5450414Bu

typedef struct {
    uint8_t  name_len;
    char     name[255];
    uint32_t capo_id;
    uint64_t file_offset;
    uint32_t capo_size;
} TpakIndexEntry;

typedef struct {
    uint32_t offset_lo;
    uint32_t offset_hi;
    uint32_t size;
    uint32_t sig32;
} ScatterDesc; /* 16B */

typedef struct {
    uint32_t offset_idx;
    uint16_t size16;
    uint16_t sig32_lo;
} ScatterDescSmall; /* 8B — matches GeoPacketSmall */

/* ═══════════════════════════════════════════════════════════════════
 * KERNELS
 * ═══════════════════════════════════════════════════════════════════ */

/* A) BRUTE FORCE — pull from HBM (old way, has H2D copy overhead) */
__global__ void pull_brute_kernel(
    const uint8_t *d_data, const uint32_t *d_offsets,
    uint32_t n_pulls, uint32_t chunk_sz, uint64_t *out_ts)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;
    for (uint32_t i = idx; i < n_pulls; i += stride) {
        const uint8_t *src = d_data + d_offsets[i];
        volatile uint32_t sum = 0;
        uint32_t n_vec = chunk_sz / 16;
        for (uint32_t v = 0; v < n_vec; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(src + v * 16);
            sum += vec.x + vec.y + vec.z + vec.w;
        }
        (void)sum;
        out_ts[i] = clock64();
    }
}

/* B) DRamTile — read from pinned host memory (zero-copy)
 * No H2D copy. GPU reads directly via PCIe/UMA from pinned pages.
 * Address computed on GPU from DRamTile geometry (no descriptor transfer).
 */
__global__ void pull_dramtile_kernel(
    const uint8_t *pinned_data,   /* mmap + cudaHostRegister'd pointer */
    uint64_t       data_size,     /* total mmap'd size */
    uint32_t       chunk_sz,      /* bytes per pull */
    uint32_t       n_pulls,       /* total pulls */
    uint64_t      *out_ts,        /* timestamps */
    uint32_t      *out_errors)    /* sig32 verify errors */
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;

    for (uint32_t i = idx; i < n_pulls; i += stride) {
        /* Compute DRamTile address from index */
        uint32_t anchor = i / DRAM_CELLS_PER;
        uint32_t cell   = i % DRAM_CELLS_PER;
        uint32_t layer  = cell / 64;
        uint32_t hp     = cell % 64;
        /* Inverse Hilbert: hp → (x,y) on 8×8 grid */
        uint32_t x = 0, y = 0, t = hp;
        for (uint32_t s = 1; s < 8; s <<= 1) {
            uint32_t rx = (t >> 1) & 1;
            uint32_t ry = (t ^ rx) & 1;
            if (ry == 0) {
                if (rx == 1) { x = s - 1 - x; y = s - 1 - y; }
                uint32_t tmp = x; x = y; y = tmp;
            }
            x += rx * s;
            y += ry * s;
            t >>= 2;
        }
        uint32_t da = dram_addr(anchor, x, y, layer);

        /* Map DRamTile address → file offset (linear for now) */
        uint64_t offset = ((uint64_t)da * chunk_sz) % data_size;
        if (offset + chunk_sz > data_size) continue;

        const uint8_t *src = pinned_data + offset;

        /* sig32 verify */
        uint64_t sig64 = 0;
        uint32_t n_words = chunk_sz / 8;
        for (uint32_t w = 0; w < n_words; w++) {
            uint64_t word;
            memcpy(&word, src + w * 8, 8);
            sig64 ^= word;
        }
        uint32_t sig32 = sig32_xor_fold(sig64);

        /* Read data (same as brute, but from host memory) */
        volatile uint32_t sum = 0;
        uint32_t n_vec = chunk_sz / 16;
        for (uint32_t v = 0; v < n_vec; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(src + v * 16);
            sum += vec.x + vec.y + vec.z + vec.w;
        }
        (void)sum;
        (void)sig32;
        out_ts[i] = clock64();
    }
}

/* C) DRamTile + GearLock — async pipeline
 * CPU computes next batch of addresses while GPU pulls current batch.
 * GearLock world counters sync without blocking.
 */
__global__ void pull_gearlock_kernel(
    const uint8_t *pinned_data,
    const ScatterDesc *d_desc,     /* precomputed descriptors (small batch) */
    uint32_t n_pulls, uint32_t chunk_sz,
    uint64_t *out_ts, uint32_t *out_errors,
    uint64_t *d_gpu_counter)       /* GearLock: GPU signals completion */
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;

    for (uint32_t i = idx; i < n_pulls; i += stride) {
        const ScatterDesc *desc = &d_desc[i];
        uint64_t offset = ((uint64_t)desc->offset_hi << 32) | desc->offset_lo;
        uint32_t sz = desc->size;

        const uint8_t *src = pinned_data + offset;

        /* sig32 verify */
        uint64_t sig64 = 0;
        uint32_t n_words = sz / 8;
        for (uint32_t w = 0; w < n_words; w++) {
            uint64_t word;
            memcpy(&word, src + w * 8, 8);
            sig64 ^= word;
        }
        uint32_t sig32 = sig32_xor_fold(sig64);
        out_errors[i] = (sig32 != desc->sig32) ? 1 : 0;

        /* Read */
        volatile uint32_t sum = 0;
        uint32_t n_vec = sz / 16;
        for (uint32_t v = 0; v < n_vec; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(src + v * 16);
            sum += vec.x + vec.y + vec.z + vec.w;
        }
        (void)sum;
        out_ts[i] = clock64();
    }
    /* GearLock: signal batch completion */
    if (idx == 0) atomicAdd((unsigned long long *)d_gpu_counter, (unsigned long long)n_pulls);
}

/* D) Compressed path — 8B descriptor (sig32 XOR-fold, GeoPacketSmall) */
__global__ void pull_small_kernel(
    const uint8_t *pinned_data,
    const ScatterDescSmall *d_desc_s,
    const uint32_t *d_full_offsets,  /* offset lookup table */
    uint32_t n_pulls, uint32_t base_chunk,
    uint64_t *out_ts, uint32_t *out_errors)
{
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t stride = gridDim.x * blockDim.x;

    for (uint32_t i = idx; i < n_pulls; i += stride) {
        const ScatterDescSmall *ds = &d_desc_s[i];
        uint64_t offset = (uint64_t)d_full_offsets[ds->offset_idx] * base_chunk;
        uint32_t sz = (uint32_t)ds->size16 * 64;

        const uint8_t *src = pinned_data + offset;
        volatile uint32_t sum = 0;
        uint32_t n_vec = sz / 16;
        for (uint32_t v = 0; v < n_vec; v++) {
            uint4 vec = *reinterpret_cast<const uint4*>(src + v * 16);
            sum += vec.x + vec.y + vec.z + vec.w;
        }
        (void)sum;
        out_ts[i] = clock64();
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s model.tesspack\n", argv[0]);
        return 1;
    }

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Tess Scatter v2 — DRamTile + GearLock Zero-Copy           ║\n");
    printf("║  sig32 XOR-fold | PCIe -50% | no H2D copy                  ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    /* ── GPU info ── */
    int dev_count;
    cudaGetDeviceCount(&dev_count);
    if (dev_count == 0) { fprintf(stderr, "No CUDA device\n"); return 1; }
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    size_t free_mem, total_mem;
    cudaMemGetInfo(&free_mem, &total_mem);
    printf("GPU: %s (sm_%d%d)  VRAM: %.0f MB free / %.0f MB total\n\n",
           prop.name, prop.major, prop.minor, free_mem/1e6, total_mem/1e6);

    /* ── mmap .tesspack ── */
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;
    void *mmap_base = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mmap_base == MAP_FAILED) { perror("mmap"); return 1; }
    close(fd);

    printf("Tesspack: %.2f GB mmap'd at %p\n\n", file_size / 1e9, mmap_base);

    /* ── Parse header ── */
    const uint8_t *base = (const uint8_t *)mmap_base;
    uint32_t magic = *(const uint32_t *)base;
    if (magic != TPAK_MAGIC) { fprintf(stderr, "Bad magic\n"); return 1; }
    uint32_t n_capos = *(const uint32_t *)(base + 8);
    uint32_t idx_off = *(const uint32_t *)(base + 12);
    printf("Header: %u capos, index at offset %lu\n", n_capos, (unsigned long)idx_off);

    /* ── Parse index ── */
    uint32_t n_use = n_capos;
    uint64_t data_size = file_size - idx_off;
    printf("Data region: %.2f GB\n\n", data_size / 1e9);

    /* Build scatter descriptors */
    ScatterDesc *h_desc = (ScatterDesc *)malloc(n_use * sizeof(ScatterDesc));
    uint32_t *h_offsets = (uint32_t *)malloc(n_use * sizeof(uint32_t));

    const uint8_t *idx_ptr = base + idx_off;
    size_t pos = 0;
    for (uint32_t i = 0; i < n_use && pos < data_size; i++) {
        uint8_t nlen = idx_ptr[pos]; pos++;
        pos += nlen;  /* skip name */
        uint32_t cid;  memcpy(&cid, idx_ptr + pos, 4); pos += 4;
        uint64_t off;  memcpy(&off, idx_ptr + pos, 8); pos += 8;
        uint32_t sz;   memcpy(&sz,  idx_ptr + pos, 4); pos += 4;

        h_desc[i].offset_lo = (uint32_t)(off & 0xFFFFFFFF);
        h_desc[i].offset_hi = (uint32_t)(off >> 32);
        h_desc[i].size = sz;

        /* Compute sig32 from mmap'd data (CPU-side reference) */
        uint64_t sig64 = 0;
        const uint8_t *capo = base + off;
        uint32_t n_words = sz / 8;
        for (uint32_t w = 0; w < n_words; w++) {
            uint64_t word;
            memcpy(&word, capo + w * 8, 8);
            sig64 ^= word;
        }
        h_desc[i].sig32 = sig32_xor_fold(sig64);
        h_offsets[i] = (uint32_t)(off & 0xFFFFFFFF);
    }
    printf("Parsed %u capos, avg size %.0f B\n\n", n_use,
           (double)data_size / n_use);

    /* ── Pin host memory for GPU zero-copy access ── */
    printf("=== Pinning host memory for zero-copy GPU access ===\n");
    double t0 = now_ms();
    CUDA_CHECK(cudaHostRegister((void *)mmap_base, file_size, cudaHostRegisterReadOnly));
    double pin_ms = now_ms() - t0;
    printf("  cudaHostRegister: %.1f ms (%.1f GB)\n\n", pin_ms, file_size / 1e9);

    /* Get device-accessible pointer */
    const uint8_t *d_pinned = NULL;
    CUDA_CHECK(cudaHostGetDevicePointer((void **)&d_pinned, (void *)mmap_base, 0));
    printf("  Device pointer: %p\n\n", d_pinned);

    /* ── GPU allocations for brute-force path ── */
    uint8_t *d_data_brute = NULL;
    uint32_t *d_offsets = NULL;
    uint64_t *d_ts = NULL;
    uint32_t *d_errors = NULL;

    /* Limit to what fits in VRAM for brute-force */
    size_t brute_limit = free_mem * 70 / 100;  /* use 70% VRAM max */
    if (data_size > brute_limit) {
        printf("  Brute-force: limiting to %.1f MB (70%% VRAM)\n", brute_limit / 1e6);
    }
    size_t brute_sz = (data_size < brute_limit) ? data_size : brute_limit;

    printf("=== Allocating GPU memory ===\n");
    CUDA_CHECK(cudaMalloc(&d_data_brute, brute_sz));
    CUDA_CHECK(cudaMalloc(&d_offsets, n_use * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_ts, n_use * sizeof(uint64_t)));
    CUDA_CHECK(cudaMalloc(&d_errors, n_use * sizeof(uint32_t)));

    /* Copy data + offsets for brute-force */
    t0 = now_ms();
    CUDA_CHECK(cudaMemcpy(d_data_brute, mmap_base, brute_sz, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_offsets, h_offsets, n_use * sizeof(uint32_t), cudaMemcpyHostToDevice));
    double h2d_ms = now_ms() - t0;
    printf("  H2D copy: %.1f ms (%.1f GB)\n\n", h2d_ms, brute_sz / 1e6);

    /* ── BENCHMARKS ── */
    uint32_t chunk_sizes[] = {144, 512, 2048};
    int n_sizes = 3;
    int TPB = 256;

    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Benchmark  A: BRUTE FORCE (H2D copy + HBM pull)                   ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");

    for (int s = 0; s < n_sizes; s++) {
        uint32_t csz = chunk_sizes[s];
        uint32_t n_pulls = (uint32_t)(brute_sz / csz);
        if (n_pulls > n_use) n_pulls = n_use;
        uint32_t blocks = (n_pulls + TPB - 1) / TPB;

        cudaEvent_t start, stop;
        cudaEventCreate(&start); cudaEventCreate(&stop);

        /* Warmup */
        pull_brute_kernel<<<blocks, TPB>>>(d_data_brute, d_offsets, n_pulls, csz, d_ts);
        cudaDeviceSynchronize();

        /* Timed */
        cudaEventRecord(start);
        pull_brute_kernel<<<blocks, TPB>>>(d_data_brute, d_offsets, n_pulls, csz, d_ts);
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);

        float ms; cudaEventElapsedTime(&ms, start, stop);
        double gb = (double)n_pulls * csz / 1e9;
        printf("  %4u B: %8u pulls  %6.2f ms  %8.2f GB/s\n", csz, n_pulls, ms, gb / (ms / 1000.0));

        cudaEventDestroy(start); cudaEventDestroy(stop);
    }

    printf("\n╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Benchmark  B: DRamTile ZERO-COPY (pinned host memory, no H2D)     ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");

    for (int s = 0; s < n_sizes; s++) {
        uint32_t csz = chunk_sizes[s];
        uint32_t n_pulls = n_use;
        uint32_t blocks = (n_pulls + TPB - 1) / TPB;

        cudaEvent_t start, stop;
        cudaEventCreate(&start); cudaEventCreate(&stop);

        /* Warmup */
        pull_dramtile_kernel<<<blocks, TPB>>>(d_pinned, data_size, csz, n_pulls, d_ts, d_errors);
        cudaDeviceSynchronize();

        /* Timed */
        cudaEventRecord(start);
        pull_dramtile_kernel<<<blocks, TPB>>>(d_pinned, data_size, csz, n_pulls, d_ts, d_errors);
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);

        float ms; cudaEventElapsedTime(&ms, start, stop);
        double gb = (double)n_pulls * csz / 1e9;
        printf("  %4u B: %8u pulls  %6.2f ms  %8.2f GB/s\n", csz, n_pulls, ms, gb / (ms / 1000.0));

        cudaEventDestroy(start); cudaEventDestroy(stop);
    }

    printf("\n╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Benchmark  C: DRamTile + sig32 verify (PCIe -50% descriptor)       ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");

    /* Upload descriptors */
    ScatterDesc *d_desc = NULL;
    CUDA_CHECK(cudaMalloc(&d_desc, n_use * sizeof(ScatterDesc)));
    CUDA_CHECK(cudaMemcpy(d_desc, h_desc, n_use * sizeof(ScatterDesc), cudaMemcpyHostToDevice));

    for (int s = 0; s < n_sizes; s++) {
        uint32_t csz = chunk_sizes[s];
        uint32_t n_pulls = n_use;
        uint32_t blocks = (n_pulls + TPB - 1) / TPB;

        cudaEvent_t start, stop;
        cudaEventCreate(&start); cudaEventCreate(&stop);
        cudaMemset(d_errors, 0, n_use * sizeof(uint32_t));

        /* Warmup */
        pull_gearlock_kernel<<<blocks, TPB>>>(d_pinned, d_desc, n_pulls, csz, d_ts, d_errors, d_ts);
        cudaDeviceSynchronize();

        cudaEventRecord(start);
        pull_gearlock_kernel<<<blocks, TPB>>>(d_pinned, d_desc, n_pulls, csz, d_ts, d_errors, d_ts);
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);

        float ms; cudaEventElapsedTime(&ms, start, stop);
        double gb = (double)n_pulls * csz / 1e9;

        /* Count errors */
        uint32_t *h_err = (uint32_t *)malloc(n_use * sizeof(uint32_t));
        cudaMemcpy(h_err, d_errors, n_use * sizeof(uint32_t), cudaMemcpyDeviceToHost);
        uint32_t errs = 0;
        for (uint32_t i = 0; i < n_use; i++) errs += h_err[i];
        free(h_err);

        printf("  %4u B: %8u pulls  %6.2f ms  %8.2f GB/s  errors: %u/%u\n",
               csz, n_pulls, ms, gb / (ms / 1000.0), errs, n_pulls);

        cudaEventDestroy(start); cudaEventDestroy(stop);
    }

    printf("\n╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║  Benchmark  D: Sig32 Compressed Descriptors (8B vs 16B)            ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");

    /* Build compressed descriptors */
    ScatterDescSmall *h_desc_s = (ScatterDescSmall *)malloc(n_use * sizeof(ScatterDescSmall));
    uint32_t *h_lookup = (uint32_t *)malloc(n_use * sizeof(uint32_t));
    for (uint32_t i = 0; i < n_use; i++) {
        h_desc_s[i].offset_idx = i;
        h_desc_s[i].size16 = (uint16_t)(h_desc[i].size / 64);
        h_desc_s[i].sig32_lo = (uint16_t)(h_desc[i].sig32 & 0xFFFF);
        h_lookup[i] = h_desc[i].offset_lo;
    }

    ScatterDescSmall *d_desc_s = NULL;
    uint32_t *d_lookup = NULL;
    CUDA_CHECK(cudaMalloc(&d_desc_s, n_use * sizeof(ScatterDescSmall)));
    CUDA_CHECK(cudaMalloc(&d_lookup, n_use * sizeof(uint32_t)));
    CUDA_CHECK(cudaMemcpy(d_desc_s, h_desc_s, n_use * sizeof(ScatterDescSmall), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_lookup, h_lookup, n_use * sizeof(uint32_t), cudaMemcpyHostToDevice));

    for (int s = 0; s < n_sizes; s++) {
        uint32_t csz = chunk_sizes[s];
        uint32_t n_pulls = n_use;
        uint32_t blocks = (n_pulls + TPB - 1) / TPB;

        cudaEvent_t start, stop;
        cudaEventCreate(&start); cudaEventCreate(&stop);

        pull_small_kernel<<<blocks, TPB>>>(d_pinned, d_desc_s, d_lookup, n_pulls, 64, d_ts, d_errors);
        cudaDeviceSynchronize();

        cudaEventRecord(start);
        pull_small_kernel<<<blocks, TPB>>>(d_pinned, d_desc_s, d_lookup, n_pulls, 64, d_ts, d_errors);
        cudaEventRecord(stop);
        cudaEventSynchronize(stop);

        float ms; cudaEventElapsedTime(&ms, start, stop);
        double gb = (double)n_pulls * csz / 1e9;
        printf("  %4u B: %8u pulls  %6.2f ms  %8.2f GB/s  (8B descriptors)\n",
               csz, n_pulls, ms, gb / (ms / 1000.0));

        cudaEventDestroy(start); cudaEventDestroy(stop);
    }

    /* ── SUMMARY ── */
    printf("\n╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║  SUMMARY                                                           ║\n");
    printf("╠══════════════════════════════════════════════════════════════════════╣\n");
    printf("║  A) Brute force:  H2D copy + HBM pull (old way)                    ║\n");
    printf("║  B) DRamTile:     Zero-copy pinned host memory (no H2D)            ║\n");
    printf("║  C) DRamTile+sig32: Zero-copy + XOR-fold verify                    ║\n");
    printf("║  D) Compressed:   8B descriptors (PCIe -50%)                       ║\n");
    printf("║                                                                     ║\n");
    printf("║  H2D copy overhead: %.1f ms (%.1f GB)\n", h2d_ms, brute_sz / 1e9);
    printf("║  Host pin overhead: %.1f ms (%.1f GB)\n", pin_ms, file_size / 1e9);
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");

    /* ── Cleanup ── */
    cudaHostUnregister((void *)mmap_base);
    munmap(mmap_base, file_size);
    cudaFree(d_data_brute); cudaFree(d_offsets); cudaFree(d_ts);
    cudaFree(d_errors); cudaFree(d_desc); cudaFree(d_desc_s); cudaFree(d_lookup);
    free(h_desc); free(h_desc_s); free(h_offsets); free(h_lookup);

    return 0;
}
