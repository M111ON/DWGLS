/* ═══════════════════════════════════════════════════════════════════════════
 * tess_scatter_gpu.h — Reusable Scatter Decode: CPU + GPU dispatch
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Extracted from bench/tess_scatter_decode.cu (proven lossless on Colab T4).
 * Provides a single dispatch API that uses GPU when available, CPU otherwise.
 *
 * Core formula (stride-37 scatter):
 *   slot = (weight_idx * 37) % 20736
 *   read cell_size bytes from cube_data + slot * cell_size
 *   write to output[weight_idx * cell_size]
 *
 * API:
 *   tess_scatter_decode(params)       — auto-dispatch GPU/CPU
 *   tess_scatter_decode_cpu(params)   — force CPU
 *   tess_scatter_verify(params)       — sig32 XOR-fold integrity check
 *   tess_scatter_has_cuda()           — query CUDA availability
 *
 * Compile:
 *   C path:   gcc -O2 -Icore -c (included in any .c file)
 *   CUDA path: nvcc -O3 -std=c++17 -arch=sm_75 -x cu (compile as CUDA)
 *
 * Dependencies: geo_tess_container.h (for TESS_TOTAL_SLOTS, TESS_STRIDE_37)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef TESS_SCATTER_GPU_H
#define TESS_SCATTER_GPU_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "geo_box_axes.h"

/* from geo_tess_container.h */
#ifndef TESS_TOTAL_SLOTS
#define TESS_TOTAL_SLOTS 20736u
#endif
#ifndef TESS_STRIDE_37
#define TESS_STRIDE_37   37u
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * CELL SIZE TABLE (indexed by GGML type)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t tess_cell_size_for_gguf(uint32_t gguf_type) {
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

/* ═══════════════════════════════════════════════════════════════════════════
 * SCATTER DECODE PARAMETERS
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    /* per-capo CubeData byte offsets (from mmap base) */
    const uint32_t *capo_offsets;
    /* output start element index per capo (prefix-sum of n_elems) */
    const uint32_t *capo_starts;
    /* elements per capo */
    const uint32_t *capo_n_elems;
    /* cell_size per capo (may vary across capos) */
    const uint32_t *capo_cell_sz;
    /* output byte offset per capo */
    const uint64_t *capo_out_bytes;
    /* number of capos */
    uint32_t n_capos;
    /* source data pointer (mmap base, pinned for GPU) */
    const uint8_t *src;
    /* output buffer */
    uint8_t *dst;
    /* effective_slots: scatter modulus (20736 at W=0, shrinks at W>0) */
    uint32_t effective_slots;
    /* total elements across all capos */
    uint64_t total_elems;
    /* total output bytes */
    uint64_t total_bytes;
} TessScatterParams;

/* ═══════════════════════════════════════════════════════════════════════════
 * SIG32 XOR-FOLD
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t tess_sig32_xor_fold(uint64_t sig64) {
    return (uint32_t)(sig64 >> 32) ^ (uint32_t)(sig64 & 0xFFFFFFFFU);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CPU SCATTER DECODE (fallback)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void tess_scatter_decode_cpu(const TessScatterParams *p) {
    uint32_t eff = p->effective_slots ? p->effective_slots : TESS_TOTAL_SLOTS;
    for (uint32_t c = 0; c < p->n_capos; c++) {
        uint32_t n = p->capo_n_elems[c];
        uint32_t csz = p->capo_cell_sz[c];
        uint32_t cube_off = p->capo_offsets[c];
        uint64_t out_off = p->capo_out_bytes[c];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t slot = (i * TESS_STRIDE_37) % eff;
            const uint8_t *src = p->src + cube_off + (uint64_t)slot * csz;
            uint8_t *dst = p->dst + out_off + (uint64_t)i * csz;
            memcpy(dst, src, csz);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CPU SIG32 VERIFY (raw capo data integrity)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t tess_scatter_verify_cpu_raw(
    const uint8_t *mmap_base,
    const uint32_t *capo_offsets,
    const uint32_t *capo_sizes,
    const uint32_t *expected_sig32,
    uint32_t n_capos)
{
    uint32_t mismatches = 0;
    for (uint32_t c = 0; c < n_capos; c++) {
        uint32_t sz = capo_sizes[c];
        const uint8_t *raw = mmap_base + capo_offsets[c];
        uint64_t sig64 = 0;
        uint32_t n_words = sz / 8;
        for (uint32_t w = 0; w < n_words; w++) {
            uint64_t word;
            memcpy(&word, raw + w * 8, 8);
            sig64 ^= word;
        }
        for (uint32_t b = n_words * 8; b < sz; b++)
            sig64 ^= (uint64_t)raw[b] << ((b & 7) * 8);
        if (tess_sig32_xor_fold(sig64) != expected_sig32[c]) {
            fprintf(stderr, "  RAW MISMATCH capo[%u]\n", c);
            mismatches++;
        }
    }
    return mismatches;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CPU SIG32 VERIFY (decoded output integrity)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t tess_scatter_verify_cpu_decoded(
    const uint8_t *output,
    const uint32_t *capo_starts,
    const uint32_t *capo_n_elems,
    const uint32_t *capo_cell_sz,
    const uint32_t *expected_sig32,
    uint32_t n_capos)
{
    uint32_t mismatches = 0;
    for (uint32_t c = 0; c < n_capos; c++) {
        uint32_t start = capo_starts[c];
        uint32_t n_elems = capo_n_elems[c];
        uint32_t csz = capo_cell_sz[c];
        uint32_t total_bytes = n_elems * csz;
        const uint8_t *base = output + (uint64_t)start * csz;
        uint64_t sig64 = 0;
        uint32_t n_words = total_bytes / 8;
        for (uint32_t w = 0; w < n_words; w++) {
            uint64_t word;
            memcpy(&word, base + w * 8, 8);
            sig64 ^= word;
        }
        for (uint32_t b = n_words * 8; b < total_bytes; b++)
            sig64 ^= (uint64_t)base[b] << ((b & 7) * 8);
        if (tess_sig32_xor_fold(sig64) != expected_sig32[c]) {
            fprintf(stderr, "  DECODE MISMATCH capo[%u]\n", c);
            mismatches++;
        }
    }
    return mismatches;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CPU REFERENCE DECODE (single capo, for verification)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void tess_scatter_decode_single_cpu(
    const uint8_t *cube_data,
    uint32_t cell_size,
    uint32_t n_elems,
    uint32_t effective_slots,
    uint8_t *output)
{
    uint32_t eff = effective_slots ? effective_slots : TESS_TOTAL_SLOTS;
    for (uint32_t i = 0; i < n_elems; i++) {
        uint32_t slot = (i * TESS_STRIDE_37) % eff;
        memcpy(output + (uint64_t)i * cell_size,
               cube_data + (uint64_t)slot * cell_size,
               cell_size);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CUDA KERNEL (compiled only when __CUDACC__ is defined)
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifdef __CUDACC__

#ifndef TESS_SCATTER_CHECK
#define TESS_SCATTER_CHECK(call) do { \
    cudaError_t _e = (call); \
    if (_e != cudaSuccess) { \
        fprintf(stderr, "CUDA ERROR [%s:%d] %s: %s\n", \
                __FILE__, __LINE__, #call, cudaGetErrorString(_e)); \
        return -1; \
    } \
} while(0)
#endif

__global__ void tess_scatter_decode_kernel(
    const uint8_t *__restrict__ file_base,
    const uint32_t *__restrict__ capo_offsets,
    const uint32_t *__restrict__ capo_starts,
    const uint32_t *__restrict__ capo_n_elems,
    const uint32_t *__restrict__ capo_cell_sz,
    const uint64_t *__restrict__ capo_out_bytes,
    uint32_t n_capos,
    uint32_t effective_slots,
    uint8_t *__restrict__ output)
{
    uint32_t global_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (global_idx >= capo_starts[n_capos - 1] + capo_n_elems[n_capos - 1])
        return;

    uint32_t lo = 0, hi = n_capos;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (capo_starts[mid] <= global_idx) lo = mid + 1;
        else hi = mid;
    }
    uint32_t c = lo - 1;

    uint32_t local_idx = global_idx - capo_starts[c];
    if (local_idx >= capo_n_elems[c]) return;

    uint32_t eff = effective_slots ? effective_slots : TESS_TOTAL_SLOTS;
    uint32_t slot = (local_idx * TESS_STRIDE_37) % eff;
    uint32_t csz = capo_cell_sz[c];
    const uint8_t *src = file_base + capo_offsets[c] + (uint64_t)slot * csz;
    uint8_t *dst = output + capo_out_bytes[c] + (uint64_t)local_idx * csz;
    for (uint32_t b = 0; b < csz; b++)
        dst[b] = src[b];
}

__global__ void tess_scatter_sig32_raw_kernel(
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
    for (uint32_t b = n_words * 8; b < sz; b++)
        sig64 ^= (uint64_t)raw[b] << ((b & 7) * 8);

    if (tess_sig32_xor_fold(sig64) != expected_sig32[c]) {
        atomicAdd(mismatch_count, 1);
        printf("  RAW MISMATCH capo[%u]\n", c);
    }
}

__global__ void tess_scatter_sig32_decoded_kernel(
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
    uint32_t csz = capo_cell_sz[c];
    uint32_t total_bytes = n_elems * csz;

    const uint8_t *base = output + (uint64_t)start * csz;
    uint64_t sig64 = 0;
    uint32_t n_words = total_bytes / 8;
    for (uint32_t w = 0; w < n_words; w++) {
        uint64_t word;
        memcpy(&word, base + w * 8, 8);
        sig64 ^= word;
    }
    for (uint32_t b = n_words * 8; b < total_bytes; b++)
        sig64 ^= (uint64_t)base[b] << ((b & 7) * 8);

    if (tess_sig32_xor_fold(sig64) != expected_sig32[c]) {
        atomicAdd(mismatch_count, 1);
        printf("  DECODE MISMATCH capo[%u]\n", c);
    }
}

/* GPU dispatch: allocate device memory, copy metadata, launch kernel */
static inline int tess_scatter_decode_gpu(const TessScatterParams *p) {
    uint32_t *d_offsets, *d_starts, *d_n_elems, *d_cell_sz;
    uint64_t *d_out_bytes;
    uint8_t *d_output;

    TESS_SCATTER_CHECK(cudaMalloc(&d_offsets, p->n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_starts, (p->n_capos + 1) * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_n_elems, p->n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_cell_sz, p->n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_out_bytes, p->n_capos * sizeof(uint64_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_output, p->total_bytes));

    TESS_SCATTER_CHECK(cudaMemcpy(d_offsets, p->capo_offsets,
        p->n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_starts, p->capo_starts,
        (p->n_capos + 1) * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_n_elems, p->capo_n_elems,
        p->n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_cell_sz, p->capo_cell_sz,
        p->n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_out_bytes, p->capo_out_bytes,
        p->n_capos * sizeof(uint64_t), cudaMemcpyHostToDevice));

    uint32_t block = 256;
    uint32_t grid = (uint32_t)((p->total_elems + block - 1) / block);

    /* If src is mmap'd and pinned, use it directly; otherwise copy */
    const uint8_t *d_src = NULL;
    int needs_unregister = 0;
    {
        cudaPointerAttributes attr;
        cudaError_t err = cudaPointerGetAttributes(&attr, p->src);
        if (err == cudaSuccess && attr.type == cudaMemoryTypeHost) {
            TESS_SCATTER_CHECK(cudaHostRegister((void *)p->src,
                /* rough upper bound */ (size_t)(p->capo_offsets[p->n_capos-1] + 20736*292),
                cudaHostRegisterReadOnly));
            TESS_SCATTER_CHECK(cudaHostGetDevicePointer((void **)&d_src,
                (void *)p->src, 0));
            needs_unregister = 1;
        } else {
            /* src already on device or unified — use directly */
            d_src = p->src;
        }
    }

    tess_scatter_decode_kernel<<<grid, block>>>(
        d_src, d_offsets, d_starts, d_n_elems, d_cell_sz, d_out_bytes,
        p->n_capos, p->effective_slots, d_output);
    cudaError_t launch_err = cudaGetLastError();
    if (launch_err != cudaSuccess) {
        fprintf(stderr, "Kernel launch failed: %s\n", cudaGetErrorString(launch_err));
        if (needs_unregister) cudaHostUnregister((void *)p->src);
        cudaFree(d_offsets); cudaFree(d_starts); cudaFree(d_n_elems);
        cudaFree(d_cell_sz); cudaFree(d_out_bytes); cudaFree(d_output);
        return -2;
    }
    cudaDeviceSynchronize();

    /* Copy result back to host */
    TESS_SCATTER_CHECK(cudaMemcpy(p->dst, d_output,
        p->total_bytes, cudaMemcpyDeviceToHost));

    if (needs_unregister) cudaHostUnregister((void *)p->src);
    cudaFree(d_offsets); cudaFree(d_starts); cudaFree(d_n_elems);
    cudaFree(d_cell_sz); cudaFree(d_out_bytes); cudaFree(d_output);
    return 0;
}

/* GPU sig32 verify (raw) */
static inline int tess_scatter_verify_gpu_raw(
    const uint8_t *mmap_base,
    const uint32_t *capo_offsets,
    const uint32_t *capo_sizes,
    const uint32_t *expected_sig32,
    uint32_t n_capos,
    uint32_t *out_mismatches)
{
    uint32_t *d_offsets, *d_sizes, *d_expected, *d_mismatch;
    uint32_t h_mismatch = 0;

    TESS_SCATTER_CHECK(cudaMalloc(&d_offsets, n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_sizes, n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_expected, n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_mismatch, sizeof(uint32_t)));

    TESS_SCATTER_CHECK(cudaMemcpy(d_offsets, capo_offsets,
        n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_sizes, capo_sizes,
        n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_expected, expected_sig32,
        n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_mismatch, &h_mismatch,
        sizeof(uint32_t), cudaMemcpyHostToDevice));

    /* Pin mmap for GPU access */
    TESS_SCATTER_CHECK(cudaHostRegister((void *)mmap_base,
        (size_t)capo_offsets[n_capos-1] + capo_sizes[n_capos-1] + 4096,
        cudaHostRegisterReadOnly));
    const uint8_t *d_base = NULL;
    TESS_SCATTER_CHECK(cudaHostGetDevicePointer((void **)&d_base,
        (void *)mmap_base, 0));

    tess_scatter_sig32_raw_kernel<<<(n_capos + 255) / 256, 256>>>(
        d_base, d_offsets, d_sizes, d_expected, n_capos, d_mismatch);
    cudaDeviceSynchronize();

    TESS_SCATTER_CHECK(cudaMemcpy(&h_mismatch, d_mismatch,
        sizeof(uint32_t), cudaMemcpyDeviceToHost));

    cudaHostUnregister((void *)mmap_base);
    cudaFree(d_offsets); cudaFree(d_sizes);
    cudaFree(d_expected); cudaFree(d_mismatch);

    *out_mismatches = h_mismatch;
    return 0;
}

/* GPU sig32 verify (decoded output) */
static inline int tess_scatter_verify_gpu_decoded(
    const uint8_t *output,
    const uint32_t *capo_starts,
    const uint32_t *capo_n_elems,
    const uint32_t *capo_cell_sz,
    const uint32_t *expected_sig32,
    uint32_t n_capos,
    uint32_t *out_mismatches)
{
    uint32_t *d_starts, *d_n_elems, *d_cell_sz, *d_expected, *d_mismatch;
    uint32_t h_mismatch = 0;

    TESS_SCATTER_CHECK(cudaMalloc(&d_starts, (n_capos + 1) * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_n_elems, n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_cell_sz, n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_expected, n_capos * sizeof(uint32_t)));
    TESS_SCATTER_CHECK(cudaMalloc(&d_mismatch, sizeof(uint32_t)));

    TESS_SCATTER_CHECK(cudaMemcpy(d_starts, capo_starts,
        (n_capos + 1) * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_n_elems, capo_n_elems,
        n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_cell_sz, capo_cell_sz,
        n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_expected, expected_sig32,
        n_capos * sizeof(uint32_t), cudaMemcpyHostToDevice));
    TESS_SCATTER_CHECK(cudaMemcpy(d_mismatch, &h_mismatch,
        sizeof(uint32_t), cudaMemcpyHostToDevice));

    /* Pin output for GPU access */
    uint64_t total_out = 0;
    for (uint32_t c = 0; c < n_capos; c++)
        total_out += (uint64_t)capo_n_elems[c] * capo_cell_sz[c];
    TESS_SCATTER_CHECK(cudaHostRegister((void *)output,
        (size_t)total_out, cudaHostRegisterReadOnly));
    const uint8_t *d_output = NULL;
    TESS_SCATTER_CHECK(cudaHostGetDevicePointer((void **)&d_output,
        (void *)output, 0));

    tess_scatter_sig32_decoded_kernel<<<(n_capos + 255) / 256, 256>>>(
        d_output, d_starts, d_n_elems, d_cell_sz, d_expected,
        n_capos, d_mismatch);
    cudaDeviceSynchronize();

    TESS_SCATTER_CHECK(cudaMemcpy(&h_mismatch, d_mismatch,
        sizeof(uint32_t), cudaMemcpyDeviceToHost));

    cudaHostUnregister((void *)output);
    cudaFree(d_starts); cudaFree(d_n_elems); cudaFree(d_cell_sz);
    cudaFree(d_expected); cudaFree(d_mismatch);

    *out_mismatches = h_mismatch;
    return 0;
}

#endif /* __CUDACC__ */

/* ═══════════════════════════════════════════════════════════════════════════
 * DISPATCH — auto-select GPU or CPU
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline int tess_scatter_has_cuda(void) {
#ifdef __CUDACC__
    int count = 0;
    cudaError_t err = cudaGetDeviceCount(&count);
    return (err == cudaSuccess && count > 0) ? 1 : 0;
#else
    return 0;
#endif
}

/* Decode: auto-dispatch GPU/CPU.
 * Returns 0 on success, negative on error.
 * If GPU available + src is mmap'd, uses GPU; otherwise CPU. */
static inline int tess_scatter_decode(const TessScatterParams *p) {
#ifdef __CUDACC__
    if (tess_scatter_has_cuda() && p->src) {
        return tess_scatter_decode_gpu(p);
    }
#endif
    tess_scatter_decode_cpu(p);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * BOX-INDEXED SCATTER — compute cube slot from weight index + box position
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Given a weight index i inside a capo's cube, scatter to:
 *   slot = (i * 37) % effective_slots
 *
 * When the capo carries axis/position metadata, box_address gives the
 * inter-box identity (not used in the scatter itself, but returned for
 * the caller's routing/MoE logic).
 */
static inline uint32_t tess_box_scatter_slot(uint32_t weight_idx,
                                              uint32_t effective_slots) {
    uint32_t eff = effective_slots ? effective_slots : TESS_TOTAL_SLOTS;
    return (weight_idx * TESS_STRIDE_37) % eff;
}

/* Given a GBA_Address + weight index, return the scatter slot within that box */
static inline uint32_t tess_gba_scatter_slot(const GBA_Address *addr,
                                              uint32_t weight_idx) {
    (void)addr;  /* GBA box always has full field */
    return tess_box_scatter_slot(weight_idx, TESS_TOTAL_SLOTS);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * CONVENIENCE: build params from .tesspack index
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Given a TESS_PackIndex and list of capos to decode, fill TessScatterParams.
 * Caller provides pre-allocated arrays (or NULL for internal alloc).
 */
#ifndef GEO_TESS_CONTAINER_H
/* forward-declare if geo_tess_container.h not yet included */
typedef struct { int _dummy; } TESS_PackIndex_stub;
#endif

#endif /* TESS_SCATTER_GPU_H */
