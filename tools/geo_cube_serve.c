/*
 * tools/geo_cube_serve.c — bake REAL GGUF into the cube-view geometry,
 * ══════════════════════════════════════════════════════════════════════
 * serve it back through all 6 face-views. No name directory in the hot
 * path — part id → geometric address is closed-form math.
 *
 *   1 part = 128 KB = one slot of the window
 *   part id f → tesseract view : tes=f/1152 · cell=(f%1152)/144 · slot=f%144
 *              → iso_fold      : anchor × hilbert_8x8 × layer (geo_dram_tile)
 *              → byte offset   = dram_addr × PART_BYTES
 *
 *   6 cube views (kis_cube_views.h, S₃ on 12³) reorder the sweep —
 *   XOR checksum over the whole model must be IDENTICAL in every view
 *   and match the zero-padded source stream (order-invariant oracle).
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/geo_cube_serve tools/geo_cube_serve.c -lm
 * RUN:   ./build/geo_cube_serve [model.gguf]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <process.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

#include "../core/gguf_reader.h"
#include "../core/kis_cube_views.h"
#include "../core/iso_fold.h"

#define PART_BYTES   (128u * 1024u)
#define N_CUBES      12u                          /* 12 × 1728 = 20736 */
#define MAX_PARTS    (N_CUBES * (uint32_t)KIS_CUBE)

/* flat part id → geometric byte offset (through the real fold) */
static inline size_t part_offset(uint32_t f) {
    uint32_t tes = f / ISO_TES_SIZE;
    uint32_t rem = f % ISO_TES_SIZE;
    IsoFold fo   = iso_fold(tes, rem / ISO_TES_SLOTS, rem % ISO_TES_SLOTS);
    return (size_t)iso_unfold(&fo) * PART_BYTES;
}

/* XOR one zero-padded part taken from a tensor slice */
static uint64_t xor_padded_part(const uint8_t *src, uint32_t len) {
    static uint8_t blk[PART_BYTES];
    memset(blk, 0, sizeof(blk));
    memcpy(blk, src, len);
    uint64_t x = 0;
    for (uint32_t b = 0; b < PART_BYTES; b += 64)
        x ^= *(const uint64_t *)(blk + b)
           ^ *(const uint64_t *)(blk + b + 8)
           ^ *(const uint64_t *)(blk + b + 16)
           ^ *(const uint64_t *)(blk + b + 24)
           ^ *(const uint64_t *)(blk + b + 32)
           ^ *(const uint64_t *)(blk + b + 40)
           ^ *(const uint64_t *)(blk + b + 48)
           ^ *(const uint64_t *)(blk + b + 56);
    return x;
}

/* ── parallel copy/memcmp workers (bake & verify are fault/copy bound) ── */
#define N_THREADS 8

typedef struct {
    const uint8_t *src;      /* source bytes (gguf mmap) */
    uint8_t       *dst;      /* window position          */
    uint32_t       len;
} CopyJob;

typedef struct {
    CopyJob *jobs;
    uint32_t lo, hi;
    volatile uint32_t bad;   /* verify: mismatch count */
} WorkerArg;

static void worker_copy(WorkerArg *a) {
    for (uint32_t j = a->lo; j < a->hi; j++)
        memcpy(a->jobs[j].dst, a->jobs[j].src, a->jobs[j].len);
}

static unsigned __stdcall worker_verify(void *p) {
    WorkerArg *a = (WorkerArg *)p;
    for (uint32_t j = a->lo; j < a->hi; j++)
        if (memcmp(a->jobs[j].dst, a->jobs[j].src, a->jobs[j].len) != 0)
            a->bad++;
    return 0;
}

static unsigned __stdcall worker_copy_thunk(void *p) {
    worker_copy((WorkerArg *)p);
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1]
        : "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf";

    GgufReader r;
    if (gguf_open((char *)path, &r) != 0) { printf("FAIL open\n"); return 1; }
    printf("=== geo_cube_serve — %s ===\n", path);
    printf("tensors %u · %.1f MB · window %u cubes x %u slots x 128KB\n\n",
           r.n_tensors, (double)r.base_sz / 1e6, N_CUBES, (unsigned)KIS_CUBE);

    /* count parts */
    uint64_t total_bytes = 0; uint32_t total_parts = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        total_bytes += r.sizes[i];
        total_parts += (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
    }
    printf("parts: %u (%.1f MB payload)\n", total_parts, (double)total_bytes / 1e6);
    if (total_parts > MAX_PARTS) { printf("FAIL: exceeds window\n"); return 1; }

    /* sparse window: reserve full space, touch only baked slots */
    uint8_t *win = (uint8_t *)VirtualAlloc(NULL,
        (size_t)MAX_PARTS * PART_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!win) { printf("FAIL: VirtualAlloc\n"); return 1; }

    /* ── BAKE (parallel): build jobs → threaded copy ─────────────────── */
    uint64_t verified = 0; uint32_t bad = 0;
    CopyJob *jobs = (CopyJob *)malloc(sizeof(CopyJob) * total_parts);
    if (!jobs) { printf("FAIL: jobs\n"); return 1; }
    double t0 = now_ms();
    {
        /* job table only — fresh VirtualAlloc pages are already zero, so
           partial-tail padding comes for free; workers do all the copying */
        uint32_t f = 0;
        for (uint32_t i = 0; i < r.n_tensors; i++) {
            const uint8_t *src = r.base + r.data_offset + r.offsets[i];
            uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
            for (uint32_t p = 0; p < np; p++, f++) {
                uint32_t off = p * PART_BYTES;
                uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
                jobs[f].src = src + off;
                jobs[f].dst = win + part_offset(f);
                jobs[f].len = len;
            }
        }
        verified = total_bytes;
        HANDLE th[N_THREADS]; WorkerArg wa[N_THREADS];
        uint32_t per = (total_parts + N_THREADS - 1) / N_THREADS;
        for (uint32_t k = 0; k < N_THREADS; k++) {
            wa[k].jobs = jobs;
            wa[k].lo = k * per; wa[k].hi = (k + 1) * per;
            if (wa[k].hi > total_parts) wa[k].hi = total_parts;
            wa[k].bad = 0;
            if (wa[k].lo < wa[k].hi)
                th[k] = (HANDLE)_beginthreadex(NULL, 0, worker_copy_thunk, &wa[k], 0, NULL);
            else th[k] = NULL;
        }
        for (uint32_t k = 0; k < N_THREADS; k++)
            if (th[k]) { WaitForSingleObject(th[k], INFINITE); CloseHandle(th[k]); }
    }
    double bake_ms = now_ms() - t0;
    printf("BAKE   %u parts · %.0f ms (%.2f GB/s scatter write, %d threads)\n\n",
           total_parts, bake_ms, (double)total_bytes / 1e9 / (bake_ms / 1000.0), N_THREADS);

    /* ── VERIFY (parallel memcmp vs source) ──────────────────────────── */
    t0 = now_ms();
    {
        HANDLE th[N_THREADS]; WorkerArg wa[N_THREADS];
        uint32_t per = (total_parts + N_THREADS - 1) / N_THREADS;
        for (uint32_t k = 0; k < N_THREADS; k++) {
            wa[k].jobs = jobs;
            wa[k].lo = k * per; wa[k].hi = (k + 1) * per;
            if (wa[k].hi > total_parts) wa[k].hi = total_parts;
            wa[k].bad = 0;
            if (wa[k].lo < wa[k].hi)
                th[k] = (HANDLE)_beginthreadex(NULL, 0, worker_verify, &wa[k], 0, NULL);
            else th[k] = NULL;
        }
        for (uint32_t k = 0; k < N_THREADS; k++)
            if (th[k]) { WaitForSingleObject(th[k], INFINITE); CloseHandle(th[k]); }
        for (uint32_t k = 0; k < N_THREADS; k++) bad += wa[k].bad;
    }
    double vf_ms = now_ms() - t0;
    printf("VERIFY %.1f MB byte-identical · %u bad parts · %.0f ms (%.2f GB/s, %s)\n\n",
           (double)verified / 1e6, bad, vf_ms,
           (double)verified / 1e9 / (vf_ms / 1000.0),
           bad ? "LOSSLESS BROKEN" : "lossless");
    if (bad) { VirtualFree(win, 0, MEM_RELEASE); return 1; }

    /* ── 6-VIEW SWEEP — XOR checksum per view == zero-padded source ──── */
    uint64_t src_xor = 0;
    for (uint32_t i = 0; i < r.n_tensors; i++) {
        const uint8_t *src = r.base + r.data_offset + r.offsets[i];
        uint32_t np = (r.sizes[i] + PART_BYTES - 1) / PART_BYTES;
        for (uint32_t p = 0; p < np; p++) {
            uint32_t off = p * PART_BYTES;
            uint32_t len = r.sizes[i] - off; if (len > PART_BYTES) len = PART_BYTES;
            src_xor ^= xor_padded_part(src + off, len);
        }
    }

    printf("%6s %10s %12s %10s\n", "view", "sweep_ms", "GB/s", "xor_match");
    int views_ok = 1;
    for (uint32_t v = 0; v < KIS_VIEWS; v++) {
        t0 = now_ms();
        uint64_t xo = 0;
        for (uint32_t k = 0; k < N_CUBES; k++) {
            for (uint32_t rr = 0; rr < KIS_CUBE; rr++) {
                uint32_t rp = kis_view6_slot(v, rr);
                uint32_t g  = k * (uint32_t)KIS_CUBE + rp;
                if (g >= total_parts) continue;
                const uint8_t *chunk = win + part_offset(g);
                for (uint32_t b = 0; b < PART_BYTES; b += 64)
                    xo ^= *(const uint64_t *)(chunk + b)
                       ^  *(const uint64_t *)(chunk + b + 8)
                       ^  *(const uint64_t *)(chunk + b + 16)
                       ^  *(const uint64_t *)(chunk + b + 24)
                       ^  *(const uint64_t *)(chunk + b + 32)
                       ^  *(const uint64_t *)(chunk + b + 40)
                       ^  *(const uint64_t *)(chunk + b + 48)
                       ^  *(const uint64_t *)(chunk + b + 56);
            }
        }
        double ms = now_ms() - t0;
        int ok = (xo == src_xor);
        if (!ok) views_ok = 0;
        printf("%6u %10.1f %12.2f %10s\n",
               v, ms, (double)total_bytes / 1e9 / (ms / 1000.0),
               ok ? "yes" : "NO");
    }
    printf("\nsix-view lossless: %s\n", views_ok ? "ALL VIEWS MATCH SOURCE" : "MISMATCH");

    VirtualFree(win, 0, MEM_RELEASE);
    gguf_close(&r);
    return views_ok ? 0 : 1;
}
