/*
 * tools/geo_sparse_serve.c — Phase 1: geometry in a SINGLE SPARSE FILE
 * ════════════════════════════════════════════════════════════════════
 * Replaces the RAM VirtualAlloc window with one NTFS sparse backing
 * file, memory-mapped. Untouched slots are HOLES: they exist as
 * addresses but cost zero disk.
 *
 * Proves the storage-layer decision:
 *   - Explorer sees exactly ONE file
 *   - logical size 2.66 GB · on-disk size ~= payload only
 *   - same lossless guarantees as the RAM window (memcmp + 6-view XOR)
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/geo_sparse_serve tools/geo_sparse_serve.c -lm
 * RUN:   ./build/geo_sparse_serve [model.gguf] [backing.geo]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#ifndef FSCTL_SET_SPARSE
#define FSCTL_SET_SPARSE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 35, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

#include "../core/gguf_reader.h"
#include "../core/kis_cube_views.h"
#include "../core/iso_fold.h"

#define PART_BYTES   (128u * 1024u)
#define N_CUBES      12u
#define MAX_PARTS    (N_CUBES * 1728u)

static uint64_t g_copies = 0;

/* flat part id -> geometric byte offset (through the real fold) */
static inline size_t part_offset(uint32_t f) {
    uint32_t tes = f / ISO_TES_SIZE;
    uint32_t rem = f % ISO_TES_SIZE;
    IsoFold fo   = iso_fold(tes, rem / ISO_TES_SLOTS, rem % ISO_TES_SLOTS);
    return (size_t)iso_unfold(&fo) * PART_BYTES;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";
    const char *backing_path = argc > 2 ? argv[2] : "build\\geo_sparse.geo";

    GgufReader r;
    if (gguf_open((char *)path, &r) != 0) { printf("FAIL open gguf\n"); return 1; }
    printf("=== geo_sparse_serve — single sparse backing ===\n");
    printf("%s · %u tensors\n", path, r.n_tensors);

    uint64_t total_bytes = 0; uint32_t total_parts = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        total_bytes += r.sizes[i];
        total_parts += (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
    }
    printf("parts %u (%.1f MB)\n", total_parts, (double)total_bytes / 1e6);
    if (total_parts > MAX_PARTS) { printf("FAIL exceeds window\n"); return 1; }

    /* ── create ONE sparse backing file ──────────────────────────────── */
    DeleteFileA(backing_path);
    HANDLE h = CreateFileA(backing_path,
        GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("FAIL create file\n"); return 1; }
    DWORD dummy = 0;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dummy, NULL)) {
        printf("WARN: FSCTL_SET_SPARSE failed — file will be dense\n");
    }
    const uint64_t WIN_SZ = (uint64_t)MAX_PARTS * PART_BYTES;
        /* 12 cubes x 1728 x 128KB = 2.66 GB logical */
    LONG hi = (LONG)(WIN_SZ >> 32), lo = (LONG)(WIN_SZ & 0xFFFFFFFFu);
    SetFilePointer(h, lo, &hi, FILE_BEGIN);
    SetEndOfFile(h);

    HANDLE map = CreateFileMappingA(h, NULL, PAGE_READWRITE, hi, lo, NULL);
    if (!map) { printf("FAIL mapping\n"); return 1; }
    uint8_t *win = (uint8_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, WIN_SZ);
    if (!win) { printf("FAIL view\n"); return 1; }

    /* ── BAKE ────────────────────────────────────────────────────────── */
    double t0 = now_ms();
    for (uint32_t fid = 0, i = 0; i < r.n_tensors; i++) {
        const uint8_t *src = r.base + r.data_offset + r.offsets[i];
        uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
        for (uint32_t p = 0; p < np; p++, fid++) {
            uint32_t off = p * PART_BYTES;
            uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
            memcpy(win + part_offset(fid), src + off, len); g_copies++;
        }
    }
    double bake_ms = now_ms() - t0;
    FlushViewOfFile(win, WIN_SZ);
    printf("\nBAKE   %u parts · %.0f ms (%.2f GB/s) · copies=%llu\n",
           total_parts, bake_ms, (double)total_bytes / 1e9 / (bake_ms / 1000.0),
           (unsigned long long)g_copies);

    /* disk reality check */
    LARGE_INTEGER logical;
    logical.HighPart = hi; logical.LowPart = lo;
    DWORD hiDisk = 0;
    DWORD loDisk = GetCompressedFileSizeA(backing_path, &hiDisk);
    uint64_t on_disk = ((uint64_t)hiDisk << 32) | loDisk;
    printf("DISK   logical %.2f GB · on-disk %.2f GB · holes saved %.2f GB (%.0f%% free)\n",
           (double)logical.QuadPart / 1e9, (double)on_disk / 1e9,
           ((double)logical.QuadPart - (double)on_disk) / 1e9,
           100.0 - 100.0 * (double)on_disk / (double)logical.QuadPart);

    /* ── VERIFY memcmp vs source ─────────────────────────────────────── */
    int failures = 0;
    t0 = now_ms();
    uint32_t bad = 0; uint64_t verified = 0;
    {
        uint32_t fid = 0;
        for (uint32_t i = 0; i < r.n_tensors; i++) {
            const uint8_t *src = r.base + r.data_offset + r.offsets[i];
            uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
            for (uint32_t p = 0; p < np; p++, fid++) {
                uint32_t off = p * PART_BYTES;
                uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
                if (memcmp(win + part_offset(fid), src + off, len) != 0) bad++;
                else verified += len;
            }
        }
    }
    printf("VERIFY %.1f MB byte-identical · %u bad · %.0f ms · %s\n",
           (double)verified / 1e6, bad, now_ms() - t0,
           bad ? "LOSSLESS BROKEN" : "lossless");
    failures += bad ? 1 : 0;

    /* ── 6-VIEW SWEEP XOR ────────────────────────────────────────────── */
    uint64_t src_xor = 0;
    for (uint32_t fid = 0, i = 0; i < r.n_tensors; i++) {
        const uint8_t *src = r.base + r.data_offset + r.offsets[i];
        uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
        for (uint32_t p = 0; p < np; p++, fid++) {
            uint32_t off = p * PART_BYTES;
            uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
            static uint8_t blk[PART_BYTES];
            memset(blk, 0, sizeof(blk));
            memcpy(blk, src + off, len);
            for (uint32_t b = 0; b < PART_BYTES; b += 64)
                src_xor ^= *(const uint64_t *)(blk + b)     ^ *(const uint64_t *)(blk + b + 8)
                        ^  *(const uint64_t *)(blk + b + 16) ^ *(const uint64_t *)(blk + b + 24)
                        ^  *(const uint64_t *)(blk + b + 32) ^ *(const uint64_t *)(blk + b + 40)
                        ^  *(const uint64_t *)(blk + b + 48) ^ *(const uint64_t *)(blk + b + 56);
        }
    }
    printf("\n%6s %10s %12s %10s\n", "view", "sweep_ms", "GB/s", "xor_match");
    for (uint32_t v = 0; v < KIS_VIEWS; v++) {
        t0 = now_ms();
        uint64_t xo = 0;
        for (uint32_t k = 0; k < N_CUBES; k++)
            for (uint32_t rr = 0; rr < KIS_CUBE; rr++) {
                uint32_t rp = kis_view6_slot(v, rr);
                uint32_t g  = k * (uint32_t)KIS_CUBE + rp;
                if (g >= total_parts) continue;
                const uint8_t *chunk = win + part_offset(g);
                for (uint32_t b = 0; b < PART_BYTES; b += 64)
                    xo ^= *(const uint64_t *)(chunk + b)     ^ *(const uint64_t *)(chunk + b + 8)
                       ^  *(const uint64_t *)(chunk + b + 16) ^ *(const uint64_t *)(chunk + b + 24)
                       ^  *(const uint64_t *)(chunk + b + 32) ^ *(const uint64_t *)(chunk + b + 40)
                       ^  *(const uint64_t *)(chunk + b + 48) ^ *(const uint64_t *)(chunk + b + 56);
            }
        double ms = now_ms() - t0;
        int ok = (xo == src_xor);
        failures += ok ? 0 : 1;
        printf("%6u %10.1f %12.2f %10s\n",
               v, ms, (double)total_bytes / 1e9 / (ms / 1000.0), ok ? "yes" : "NO");
    }

    printf("\nRESULT: %s · files exposed: 1 · carried state: 0 (geometry in-file)\n",
           failures ? "FAILED" : "SPARSE BACKING LOSSLESS");

    UnmapViewOfFile(win);
    CloseHandle(map);
    CloseHandle(h);
    gguf_close(&r);
    return failures ? 1 : 0;
}
