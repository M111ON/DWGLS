/* ═══════════════════════════════════════════════════════════════════
 * tess_scatter_decode.cu — Standalone GPU scatter decode kernel
 *
 * Decodes .tesspack capos on GPU via stride-37 scatter gather.
 * No llama.cpp dependency. Pure CUDA + mmap + cudaHostRegister.
 *
 * Architecture:
 *   1. mmap .tesspack on host (zero-copy source)
 *   2. Parse pack header + index on CPU
 *   3. For each capo: mmap → read TESS_Header → get cell_size
 *   4. cudaHostRegister pin all pages
 *   5. GPU kernel: weight_idx → slot = (idx*37)%20736 → read cell
 *   6. sig32 XOR-fold verify on GPU
 *   7. Output: decoded weights per capo
 *
 * Compile (Colab T4):
 *   nvcc -O3 -std=c++17 -arch=sm_75 -o tess_scatter_decode tess_scatter_decode.cu -lm
 *
 * Usage:
 *   ./tess_scatter_decode model.tesspack [--verify]
 *   ./tess_scatter_decode model.tesspack --tensor <name> [--verify]
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
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════════ */
#define TESS_TOTAL_SLOTS   20736u
#define TESS_STRIDE_37     37u
#define TESS_STRIDE_37_INV 16813u  /* 37^(-1) mod 20736 */
#define TESS_HEADER_SIZE   64u
#define TESS_FORMULA_SIZE  64u
#define TESS_MAGIC         0x54455353u  /* "TESS" */
#define TPAK_MAGIC         0x5450414Bu  /* "TPAK" */

/* Cell sizes by GGML type */
static inline uint32_t gguf_cell_size(uint32_t gguf_type) {
    switch (gguf_type) {
        case 0:  return 4;    /* F32 */
        case 1:  return 2;    /* F16 */
        case 2:  return 18;   /* Q4_0: 2B scale + 16B int4 */
        case 8:  return 34;   /* Q8_0: 2B scale + 32B int8 */
        case 12: return 144;  /* Q4_K */
        case 13: return 176;  /* Q5_K */
        case 14: return 210;  /* Q6_K */
        case 15: return 292;  /* Q8_K */
        case 41: return 6;    /* Q1_0 */
        default: return 4;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * PACK HEADER + INDEX (CPU-side parsing)
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    uint32_t gguf_type;
    uint32_t cell_size;
    uint32_t total_slots;   /* always 20736 */
    uint32_t tensor_count;
    uint64_t capo_start;    /* byte offset from file start to capo beginning (header) */
    uint64_t cube_offset;   /* byte offset from file start to CubeData */
    uint32_t capo_size;     /* total capo byte size */
    uint32_t n_elems;       /* elements to decode (tensor_count or 20736) */
    char     name[256];
    uint32_t capo_id;
} CapoInfo;

/* sig32 XOR-fold */
static inline __host__ __device__ uint32_t sig32_xor_fold(uint64_t sig64) {
    return (uint32_t)(sig64 >> 32) ^ (uint32_t)(sig64 & 0xFFFFFFFFU);
}

/* ═══════════════════════════════════════════════════════════════════
 * GPU KERNEL: scatter decode
 *
 * For each output element weight_idx:
 *   slot = (weight_idx * 37) % 20736
 *   read cell_size bytes from cube_data + slot * cell_size
 *   write to output[weight_idx * cell_size]
 *
 * grid: 1D, covers all elements across all selected capos
 * ═══════════════════════════════════════════════════════════════════ */
__global__ void scatter_decode_kernel(
    const uint8_t *__restrict__ file_base,  /* mmap base (device ptr via cudaHostRegister) */
    const uint32_t *__restrict__ capo_offsets,  /* byte offsets of each capo's CubeData */
    const uint32_t *__restrict__ capo_starts,   /* output start index for each capo */
    const uint32_t *__restrict__ capo_n_elems,  /* elements per capo */
    const uint32_t *__restrict__ capo_cell_sz,  /* cell_size per capo */
    uint32_t n_capos,
    uint8_t *__restrict__ output)
{
    uint32_t global_idx = blockIdx.x * blockDim.x + threadIdx.x;

    /* find which capo this thread belongs to via binary search on capo_starts */
    if (global_idx >= capo_starts[n_capos - 1] + capo_n_elems[n_capos - 1])
        return;

    /* binary search for capo */
    uint32_t lo = 0, hi = n_capos;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (capo_starts[mid] <= global_idx) lo = mid + 1;
        else hi = mid;
    }
    uint32_t c = lo - 1;

    uint32_t local_idx = global_idx - capo_starts[c];
    uint32_t cell_sz = capo_cell_sz[c];
    uint32_t n_elems = capo_n_elems[c];

    if (local_idx >= n_elems) return;

    /* scatter decode: weight_idx → slot → read */
    uint32_t slot = (local_idx * TESS_STRIDE_37) % TESS_TOTAL_SLOTS;
    const uint8_t *src = file_base + capo_offsets[c] + (uint64_t)slot * cell_sz;
    uint8_t *dst = output + (uint64_t)global_idx * cell_sz;
    for (uint32_t b = 0; b < cell_sz; b++) {
        dst[b] = src[b];
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * GPU KERNEL: raw-data sig32 integrity verify
 *
 * Computes sig32 of each capo's RAW (pre-decode) data on GPU
 * and compares against expected sig32 (computed from CPU mmap).
 * This verifies the mmap→GPU pointer path is correct.
 * ═══════════════════════════════════════════════════════════════════ */
__global__ void sig32_raw_verify_kernel(
    const uint8_t *__restrict__ file_base,
    const uint32_t *__restrict__ capo_offsets,
    const uint32_t *__restrict__ capo_sizes,
    const uint32_t *__restrict__ expected_sig32,
    uint32_t n_capos,
    uint32_t *__restrict__ mismatch_count)
{
    uint32_t c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= n_capos) return;

    uint32_t sz = capo_sizes[c];
    const uint8_t *raw = file_base + capo_offsets[c];

    uint64_t sig64 = 0;
    uint32_t n_words = sz / 8;
    for (uint32_t w = 0; w < n_words; w++) {
        uint64_t word;
        memcpy(&word, raw + w * 8, 8);
        sig64 ^= word;
    }
    for (uint32_t b = n_words * 8; b < sz; b++) {
        sig64 ^= (uint64_t)raw[b] << ((b & 7) * 8);
    }

    uint32_t computed = sig32_xor_fold(sig64);
    if (computed != expected_sig32[c]) {
        atomicAdd(mismatch_count, 1);
        printf("  RAW MISMATCH capo[%u]: computed=0x%08X expected=0x%08X\n",
               c, computed, expected_sig32[c]);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * GPU KERNEL: decoded-output sig32 verify
 *
 * Computes sig32 of GPU-decoded output and compares against
 * expected sig32 computed from CPU reference decode.
 * This verifies the decode kernel is bitwise correct.
 * ═══════════════════════════════════════════════════════════════════ */
__global__ void sig32_decode_verify_kernel(
    const uint8_t *__restrict__ output,
    const uint32_t *__restrict__ capo_starts,
    const uint32_t *__restrict__ capo_n_elems,
    const uint32_t *__restrict__ capo_cell_sz,
    const uint32_t *__restrict__ expected_sig32,
    uint32_t n_capos,
    uint32_t *__restrict__ mismatch_count)
{
    uint32_t c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= n_capos) return;

    uint32_t start = capo_starts[c];
    uint32_t n_elems = capo_n_elems[c];
    uint32_t cell_sz = capo_cell_sz[c];
    uint32_t total_bytes = n_elems * cell_sz;

    uint64_t sig64 = 0;
    const uint8_t *base = output + (uint64_t)start * cell_sz;
    uint32_t n_words = total_bytes / 8;
    for (uint32_t w = 0; w < n_words; w++) {
        uint64_t word;
        memcpy(&word, base + w * 8, 8);
        sig64 ^= word;
    }
    for (uint32_t b = n_words * 8; b < total_bytes; b++) {
        sig64 ^= (uint64_t)base[b] << ((b & 7) * 8);
    }

    uint32_t computed = sig32_xor_fold(sig64);
    if (computed != expected_sig32[c]) {
        atomicAdd(mismatch_count, 1);
        printf("  DECODE MISMATCH capo[%u]: computed=0x%08X expected=0x%08X\n",
               c, computed, expected_sig32[c]);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * CPU REFERENCE DECODE (for verification)
 * ═══════════════════════════════════════════════════════════════════ */
static void cpu_scatter_decode(const uint8_t *cube_data, uint32_t cell_size,
                               uint32_t n_elems, uint8_t *output) {
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = (i * TESS_STRIDE_37) % TESS_TOTAL_SLOTS;
        memcpy(output + (uint64_t)i * cell_size,
               cube_data + (uint64_t)slot * cell_size,
               cell_size);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * TIMING
 * ═══════════════════════════════════════════════════════════════════ */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s model.tesspack [--verify] [--tensor <name>]\n", argv[0]);
        return 1;
    }

    const char *pack_path = argv[1];
    int do_verify = 0;
    const char *filter_tensor = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0) do_verify = 1;
        else if (strcmp(argv[i], "--tensor") == 0 && i + 1 < argc) filter_tensor = argv[++i];
    }

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Tess Scatter Decode — Standalone GPU Kernel                ║\n");
    printf("║  stride-37 | zero-copy | no llama.cpp dependency           ║\n");
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
    double t0 = now_ms();
    int fd = open(pack_path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;
    void *mmap_base = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mmap_base == MAP_FAILED) { perror("mmap"); return 1; }
    close(fd);
    double mmap_ms = now_ms() - t0;
    printf("Tesspack: %.2f GB mmap'd (%.1f ms)\n", file_size / 1e9, mmap_ms);

    /* ── Parse pack header ── */
    const uint8_t *base = (const uint8_t *)mmap_base;
    uint32_t magic = *(const uint32_t *)base;
    if (magic != TPAK_MAGIC) {
        fprintf(stderr, "Bad magic: 0x%08X (expected 0x%08X)\n", magic, TPAK_MAGIC);
        return 1;
    }
    uint32_t version = *(const uint32_t *)(base + 4);
    uint32_t n_capos = *(const uint32_t *)(base + 8);
    uint32_t idx_off = *(const uint32_t *)(base + 12);
    printf("Pack header: v%u, %u capos, index at offset %u\n\n", version, n_capos, idx_off);

    /* ── Parse index — collect capo info ── */
    CapoInfo *capos = (CapoInfo *)calloc(n_capos, sizeof(CapoInfo));
    uint32_t *expected_sig32 = (uint32_t *)malloc(n_capos * sizeof(uint32_t));
    uint32_t n_valid = 0;

    const uint8_t *idx_ptr = base + idx_off;
    size_t pos = 0;
    for (uint32_t i = 0; i < n_capos; i++) {
        if (pos + 1 > file_size - idx_off) break;
        uint8_t nlen = idx_ptr[pos]; pos++;
        if (nlen == 0) break;
        if (pos + nlen + 16 > file_size - idx_off) break;

        char name[256];
        memcpy(name, idx_ptr + pos, nlen);
        name[nlen] = 0;
        pos += nlen;

        uint32_t cid;
        uint64_t off;
        uint32_t sz;
        memcpy(&cid, idx_ptr + pos, 4); pos += 4;
        memcpy(&off, idx_ptr + pos, 8); pos += 8;
        memcpy(&sz,  idx_ptr + pos, 4); pos += 4;

        /* filter by tensor name if requested */
        if (filter_tensor && strcmp(name, filter_tensor) != 0) continue;

        /* read TESS header from capo */
        if (off + TESS_HEADER_SIZE + TESS_FORMULA_SIZE > file_size) continue;
        const uint32_t *thdr = (const uint32_t *)(base + off);
        if (thdr[0] != TESS_MAGIC) continue;

        CapoInfo *c = &capos[n_valid];
        strncpy(c->name, name, 255);
        c->name[255] = 0;
        c->capo_id = cid;
        c->capo_start = off;      /* capo start = header start */
        c->gguf_type = thdr[8];   /* gguf_type at offset 32 */
        c->cell_size = thdr[3];   /* cell_size at offset 12 */
        c->total_slots = thdr[2]; /* total_slots at offset 8 */
        c->tensor_count = thdr[9]; /* tensor_count at offset 36 */
        c->cube_offset = off + TESS_HEADER_SIZE + TESS_FORMULA_SIZE;
        c->capo_size = sz;
        c->n_elems = c->tensor_count ? c->tensor_count : c->total_slots;

        /* fallback cell_size from gguf_type if header value looks wrong */
        if (c->cell_size == 0 || c->cell_size > 20736) {
            c->cell_size = gguf_cell_size(c->gguf_type);
        }

        /* compute expected sig32 from mmap'd capo data */
        uint64_t sig64 = 0;
        const uint8_t *capo_data = base + off;
        uint32_t n_words = sz / 8;
        for (uint32_t w = 0; w < n_words; w++) {
            uint64_t word;
            memcpy(&word, capo_data + w * 8, 8);
            sig64 ^= word;
        }
        expected_sig32[n_valid] = sig32_xor_fold(sig64);

        n_valid++;
    }

    if (n_valid == 0) {
        fprintf(stderr, "No matching capos found\n");
        return 1;
    }

    printf("Selected %u capos:\n", n_valid);
    uint64_t total_elems = 0;
    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < n_valid; i++) {
        total_elems += capos[i].n_elems;
        total_bytes += (uint64_t)capos[i].n_elems * capos[i].cell_size;
        if (n_valid <= 20) {
            printf("  [%u] %s capo=%u type=%u cell=%uB elems=%u\n",
                   i, capos[i].name, capos[i].capo_id,
                   capos[i].gguf_type, capos[i].cell_size, capos[i].n_elems);
        }
    }
    printf("  Total: %lu elements, %.2f MB decoded output\n\n",
           (unsigned long)total_elems, total_bytes / 1e6);

    /* ── Build GPU arrays ── */
    uint32_t *h_offsets = (uint32_t *)malloc(n_valid * sizeof(uint32_t));  /* CubeData offsets */
    uint32_t *h_starts = (uint32_t *)malloc((n_valid + 1) * sizeof(uint32_t)); /* output starts */
    uint32_t *h_n_elems = (uint32_t *)malloc(n_valid * sizeof(uint32_t));
    uint32_t *h_cell_sz = (uint32_t *)malloc(n_valid * sizeof(uint32_t));
    uint32_t *h_capo_starts = (uint32_t *)malloc(n_valid * sizeof(uint32_t)); /* capo start offsets (raw) */

    uint32_t accum = 0;
    for (uint32_t i = 0; i < n_valid; i++) {
        h_offsets[i] = (uint32_t)capos[i].cube_offset;
        h_starts[i] = accum;
        h_n_elems[i] = capos[i].n_elems;
        h_cell_sz[i] = capos[i].cell_size;
        h_capo_starts[i] = (uint32_t)capos[i].capo_start;
        accum += capos[i].n_elems;
    }
    h_starts[n_valid] = accum; /* sentinel */

    /* ── Pin host memory ── */
    t0 = now_ms();
    CUDA_CHECK(cudaHostRegister((void *)mmap_base, file_size, cudaHostRegisterReadOnly));
    double pin_ms = now_ms() - t0;
    printf("cudaHostRegister: %.1f ms (%.1f GB)\n", pin_ms, file_size / 1e9);

    const uint8_t *d_pinned = NULL;
    CUDA_CHECK(cudaHostGetDevicePointer((void **)&d_pinned, (void *)mmap_base, 0));
    printf("Device pointer: %p\n\n", d_pinned);

    /* ── GPU allocations ── */
    uint32_t *d_offsets, *d_starts, *d_n_elems, *d_cell_sz, *d_capo_starts;
    uint8_t *d_output;
    CUDA_CHECK(cudaMalloc(&d_offsets, n_valid * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_starts, (n_valid + 1) * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_n_elems, n_valid * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_cell_sz, n_valid * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_capo_starts, n_valid * sizeof(uint32_t)));
    CUDA_CHECK(cudaMalloc(&d_output, total_bytes));

    CUDA_CHECK(cudaMemcpy(d_offsets, h_offsets, n_valid * sizeof(uint32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_starts, h_starts, (n_valid + 1) * sizeof(uint32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_n_elems, h_n_elems, n_valid * sizeof(uint32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_cell_sz, h_cell_sz, n_valid * sizeof(uint32_t), cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(d_capo_starts, h_capo_starts, n_valid * sizeof(uint32_t), cudaMemcpyHostToDevice));

    printf("GPU alloc: %.2f MB output buffer\n\n", total_bytes / 1e6);

    /* ── Launch decode kernel ── */
    uint32_t total_threads = (uint32_t)total_elems;
    uint32_t block_size = 256;
    uint32_t grid_size = (total_threads + block_size - 1) / block_size;

    printf("=== SCATTER DECODE ===\n");
    printf("  Kernel: %u threads, %u blocks x %u threads\n",
           total_threads, grid_size, block_size);

    t0 = now_ms();
    scatter_decode_kernel<<<grid_size, block_size>>>(
        d_pinned, d_offsets, d_starts, d_n_elems, d_cell_sz, n_valid, d_output);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());
    double decode_ms = now_ms() - t0;

    double throughput_gb = total_bytes / 1e6 / decode_ms * 1000.0 / 1e9;
    printf("  Decode: %.2f ms  (%.2f GB/s throughput)\n\n", decode_ms, throughput_gb);

    /* ── Verify: raw data integrity ── */
    if (do_verify) {
        printf("=== SIG32 RAW DATA INTEGRITY ===\n");
        uint32_t *h_capo_sizes = (uint32_t *)malloc(n_valid * sizeof(uint32_t));
        for (uint32_t i = 0; i < n_valid; i++)
            h_capo_sizes[i] = capos[i].capo_size;

        uint32_t *d_expected_sig32, *d_capo_sizes;
        uint32_t *d_mismatch;
        uint32_t h_mismatch = 0;
        CUDA_CHECK(cudaMalloc(&d_expected_sig32, n_valid * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&d_capo_sizes, n_valid * sizeof(uint32_t)));
        CUDA_CHECK(cudaMalloc(&d_mismatch, sizeof(uint32_t)));
        CUDA_CHECK(cudaMemcpy(d_expected_sig32, expected_sig32,
                              n_valid * sizeof(uint32_t), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d_capo_sizes, h_capo_sizes,
                              n_valid * sizeof(uint32_t), cudaMemcpyHostToDevice));
        CUDA_CHECK(cudaMemcpy(d_mismatch, &h_mismatch, sizeof(uint32_t), cudaMemcpyHostToDevice));

        t0 = now_ms();
        sig32_raw_verify_kernel<<<(n_valid + 255) / 256, 256>>>(
            d_pinned, d_capo_starts, d_capo_sizes, d_expected_sig32, n_valid, d_mismatch);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaDeviceSynchronize());
        double raw_verify_ms = now_ms() - t0;

        CUDA_CHECK(cudaMemcpy(&h_mismatch, d_mismatch, sizeof(uint32_t), cudaMemcpyDeviceToHost));
        printf("  Raw sig32 verify: %u / %u capos mismatch (%.2f ms)\n\n",
               h_mismatch, n_valid, raw_verify_ms);

        cudaFree(d_expected_sig32);
        cudaFree(d_capo_sizes);
        cudaFree(d_mismatch);
        free(h_capo_sizes);

        if (h_mismatch > 0) {
            fprintf(stderr, "RAW DATA INTEGRITY FAILED — mmap pointer incorrect\n");
            return 1;
        }
        printf("  ✓ RAW DATA INTEGRITY PASSED\n\n");
    }

    /* ── Verify: CPU vs GPU decode (first capo) ── */
    if (do_verify && n_valid > 0) {
        printf("=== CPU REFERENCE CHECK (capo[0]) ===\n");
        uint32_t ref_elems = capos[0].n_elems;
        uint32_t ref_cell = capos[0].cell_size;
        uint8_t *ref_out = (uint8_t *)malloc((uint64_t)ref_elems * ref_cell);
        const uint8_t *cube0 = base + capos[0].cube_offset;

        t0 = now_ms();
        cpu_scatter_decode(cube0, ref_cell, ref_elems, ref_out);
        double cpu_ms = now_ms() - t0;

        /* copy first capo from GPU output */
        uint8_t *gpu_out = (uint8_t *)malloc((uint64_t)ref_elems * ref_cell);
        CUDA_CHECK(cudaMemcpy(gpu_out, d_output,
                              (uint64_t)ref_elems * ref_cell, cudaMemcpyDeviceToHost));

        int match = (memcmp(ref_out, gpu_out, (uint64_t)ref_elems * ref_cell) == 0);
        printf("  CPU decode: %.2f ms\n", cpu_ms);
        printf("  CPU vs GPU: %s\n\n", match ? "MATCH" : "MISMATCH");

        if (!match) {
            /* show first mismatch position */
            for (uint64_t b = 0; b < (uint64_t)ref_elems * ref_cell; b++) {
                if (ref_out[b] != gpu_out[b]) {
                    printf("  First mismatch at byte %lu: CPU=0x%02X GPU=0x%02X\n",
                           (unsigned long)b, ref_out[b], gpu_out[b]);
                    break;
                }
            }
            free(ref_out);
            free(gpu_out);
            fprintf(stderr, "CPU/GPU MISMATCH\n");
            return 1;
        }

        free(ref_out);
        free(gpu_out);
        printf("  ✓ CPU/GPU BITWISE IDENTICAL\n\n");
    }

    /* ── Cleanup ── */
    cudaFree(d_offsets);
    cudaFree(d_starts);
    cudaFree(d_n_elems);
    cudaFree(d_cell_sz);
    cudaFree(d_capo_starts);
    cudaFree(d_output);
    cudaHostUnregister((void *)mmap_base);
    munmap(mmap_base, file_size);
    free(capos);
    free(expected_sig32);
    free(h_offsets);
    free(h_starts);
    free(h_n_elems);
    free(h_cell_sz);
    free(h_capo_starts);

    printf("Done. All resources freed.\n");
    return 0;
}
