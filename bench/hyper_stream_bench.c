/*
 * hyper_stream_bench.c — real-GGUF streaming + random access on GeoFS hyper key-frame
 * ═══════════════════════════════════════════════════════════════════════════════════
 * Fills a GeosVolume with REAL model bytes (middle slice = quantized weights),
 * then measures:
 *   B1 place      — hyper scatter write throughput (seed + stride walk)
 *   B2 seq read   — geos_hyper_read whole-file stream (lossless-gated by memcmp)
 *   B3 rand block — geos_hyper_project_block random 64B access (xorshift pattern)
 *   B4 contiguous — same volume shape but classic sequential layout (geos_create+write)
 *   B5 raw fread  — neutral disk baseline (OS-cached) of the same byte count
 *
 * BUILD: gcc -O2 -Wall -Icore -o build/hyper_stream_bench bench/hyper_stream_bench.c -lm
 * RUN:   ./build/hyper_stream_bench [model.gguf]   (default: I:\model GGUFs)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "geofs_core.h"

/* ── SPEC constants (independent oracle) ───────────────────────────── */
#define SPEC_STRIDE_AXIS1  9u
#define SPEC_GEO_FULL      20736u

/* axis-1 (stride 9) has exactly 9 residue classes; a 2048-block walk fills
 * one class (orbit 2304) — two files in one class must overlap (2048·2 > 2304)
 * → hard cap of 9 hyper files per volume on this axis. */
#define N_FILES        9u
#define FILE_BLOCKS    2048u                          /* orbit 2304 — no wrap */
#define FILE_BYTES     (FILE_BLOCKS * GEOS_BLOCK_SZ)  /* 128 KB */
#define TOTAL_BYTES    ((size_t)N_FILES * FILE_BYTES) /* 1.25 MB = full volume */

#define RAND_ACCESSES  (4u * 1000u * 1000u)           /* 4M random 64B pulls */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#ifdef _WIN32
#define FS_FSEEK _fseeki64
#define FS_FTELL _ftelli64
#else
#define FS_FSEEK fseeko
#define FS_FTELL ftello
#endif

static const char *CANDIDATES[] = {
    "I:\\model\\Qwen2.5-0.5B-Instruct-Q8_0.gguf",
    "I:\\model\\Qwen3-0.6B-Q8_0.gguf",
    "I:\\model\\SmolLM2-360M-Instruct.Q8_0.gguf",
    "I:\\DWGLS\\build\\qwen05-direct.gguf",
    NULL
};

static const char *pick_real_file(int argc, char **argv) {
    if (argc > 1) return argv[1];
    const char *env = getenv("DWGLS_GGUF");
    if (env && *env) return env;
    for (int i = 0; CANDIDATES[i]; i++) {
        FILE *f = fopen(CANDIDATES[i], "rb");
        if (f) { fclose(f); return CANDIDATES[i]; }
    }
    return NULL;
}

static long read_slice_at(const char *path, uint8_t *buf, size_t want, int64_t off) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (FS_FSEEK(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    int64_t sz = FS_FTELL(f);
    if ((int64_t)want > sz) { fclose(f); return -1; }
    if (off < 0) off = 0;
    if (off + (int64_t)want > sz) off = sz - (int64_t)want;
    if (FS_FSEEK(f, off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    return (long)got;
}

/* seeds with pairwise-disjoint residues mod 9 → walks cannot collide */
static int seed_ok(const uint8_t *map, uint32_t seed) {
    for (uint32_t b = 0; b < FILE_BLOCKS; b++) {
        uint32_t addr = (seed + SPEC_STRIDE_AXIS1 * b) % SPEC_GEO_FULL;
        if (addr < GEOS_VOL_DATA_START) return 0;
        if (map[addr / 8] & (1u << (addr % 8))) return 0;
    }
    return 1;
}

static uint32_t xs32(uint32_t *s) {
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

int main(int argc, char **argv) {
    printf("═══ hyper_stream_bench — GeoFS hyper key-frame, real GGUF ═══\n\n");
    const char *path = pick_real_file(argc, argv);
    if (!path) { printf("SKIP: no real GGUF found\n"); return 0; }

    uint8_t *slice = (uint8_t *)malloc(TOTAL_BYTES);
    uint8_t *rb    = (uint8_t *)malloc(FILE_BYTES);
    if (!slice || !rb) { printf("FAIL: malloc\n"); return 1; }

    /* middle slice = real quantized weight bytes (high entropy) */
    if (read_slice_at(path, slice, TOTAL_BYTES, -1) < (long)TOTAL_BYTES) {
        printf("SKIP: %s too small\n", path); return 0;
    }
    printf("  source : %s\n", path);
    printf("  data   : %u bytes (middle slice, weights)\n", (unsigned)TOTAL_BYTES);
    printf("  layout : %u hyper files x %u blocks (axis 1, stride 9)\n\n", N_FILES, FILE_BLOCKS);

    GeosVolume vol;
    geos_volume_init(&vol);

    char names[N_FILES][16];
    uint32_t seeds[N_FILES];

    /* ── B1: hyper scatter place (seed search timed apart from writes) ─
     * NOTE: search must be interleaved with placement — the HWRouter walk
     * is not plain modular arithmetic, so orbits of pending seeds can
     * collide; each seed is only valid against already-placed files. */
    double t_seed = 0.0, t_place = 0.0, t0 = 0.0;
    for (uint32_t k = 0; k < N_FILES; k++) {
        snprintf(names[k], sizeof(names[k]), "w%02u.bin", k);
        uint32_t seed = 300 + k * 1100u;
        double ts = now_ms();
        while (!seed_ok(vol.block_map, seed)) seed++;
        t_seed += now_ms() - ts;
        double tp = now_ms();
        GeosInode *in = geos_hyper_place(&vol, names[k], FILE_BYTES,
                                         slice + k * FILE_BYTES, seed, 1);
        t_place += now_ms() - tp;
        if (!in) { printf("FAIL: place %u\n", k); return 1; }
        seeds[k] = seed;
    }

    /* lossless gate before any timing + SPEC oracle on the address formula */
    for (uint32_t k = 0; k < N_FILES; k++) {
        if (geos_hyper_address(&vol, names[k], 0) != seeds[k] % SPEC_GEO_FULL ||
            geos_hyper_address(&vol, names[k], FILE_BLOCKS - 1) !=
                (seeds[k] + SPEC_STRIDE_AXIS1 * (FILE_BLOCKS - 1)) % SPEC_GEO_FULL) {
            printf("FAIL: address oracle file %u\n", k); return 1;
        }
        if (geos_hyper_read(&vol, names[k], rb, FILE_BYTES) != (int)FILE_BYTES ||
            memcmp(slice + k * FILE_BYTES, rb, FILE_BYTES) != 0) {
            printf("FAIL: lossless gate file %u\n", k); return 1;
        }
    }
    printf("  lossless gate: memcmp PASS (%u files)\n\n", N_FILES);

    double mb_total = (double)TOTAL_BYTES / 1e6;

    /* ── B2: sequential stream read (min of 3 reps to cut OS noise) ─── */
    const int ROUNDS = 20;
    double t_seq = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        t0 = now_ms();
        for (int r = 0; r < ROUNDS; r++)
            for (uint32_t k = 0; k < N_FILES; k++)
                geos_hyper_read(&vol, names[k], rb, FILE_BYTES);
        double t = now_ms() - t0;
        if (t < t_seq) t_seq = t;
    }
    t_seq /= ROUNDS;

    /* ── B3: random 64B block access (hyper scatter) ────────────────── */
    volatile uint64_t sink = 0;
    t0 = now_ms();
    uint32_t st = 0x9e3779b9u;
    for (uint32_t i = 0; i < RAND_ACCESSES; i++) {
        xs32(&st);
        uint32_t k = ((uint64_t)st * N_FILES) >> 32;
        uint32_t b = xs32(&st) & (FILE_BLOCKS - 1);
        const uint8_t *p = geos_hyper_project_block(&vol, names[k], b);
        sink += p[(i + b) & (GEOS_BLOCK_SZ - 1)];
    }
    double t_rand = now_ms() - t0;

    /* ── B3b: inode-resolved random access (name lookup hoisted) ────── */
    GeosInode *hins[N_FILES];
    for (uint32_t k = 0; k < N_FILES; k++)
        hins[k] = geos_find(&vol, names[k]);
    t0 = now_ms();
    for (uint32_t i = 0; i < RAND_ACCESSES; i++) {
        xs32(&st);
        uint32_t k = ((uint64_t)st * N_FILES) >> 32;
        uint32_t b = xs32(&st) & (FILE_BLOCKS - 1);
        HWRouter r; hw_init(&r, hins[k]->block_start, hins[k]->hyper_axis);
        uint32_t addr = hw_at(&r, b);
        sink += vol.data[addr * GEOS_BLOCK_SZ + (i & (GEOS_BLOCK_SZ - 1))];
    }
    double t_rand_hoist = now_ms() - t0;

    /* ── B4: contiguous layout baseline (same store, classic FS) ────── */
    GeosVolume cvol;
    geos_volume_init(&cvol);
    t0 = now_ms();
    for (uint32_t k = 0; k < N_FILES; k++) {
        snprintf(names[k], sizeof(names[k]), "c%02u.bin", k);
        if (!geos_create(&cvol, names[k], FILE_BYTES, slice + k * FILE_BYTES)) {
            printf("FAIL: ccreate %u\n", k); return 1;
        }
        geos_write(&cvol, names[k], slice + k * FILE_BYTES, FILE_BYTES);
    }
    double t_cplace = now_ms() - t0;

    double t_cseq = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        t0 = now_ms();
        for (int r = 0; r < ROUNDS; r++)
            for (uint32_t k = 0; k < N_FILES; k++)
                geos_read(&cvol, names[k], rb, FILE_BYTES);
        double t = now_ms() - t0;
        if (t < t_cseq) t_cseq = t;
    }
    t_cseq /= ROUNDS;

    t0 = now_ms();
    for (uint32_t i = 0; i < RAND_ACCESSES; i++) {
        xs32(&st);
        uint32_t k = ((uint64_t)st * N_FILES) >> 32;
        uint32_t b = xs32(&st) & (FILE_BLOCKS - 1);
        const uint8_t *p = geos_project_block(&cvol, names[k], b);
        sink += p[(i + b) & (GEOS_BLOCK_SZ - 1)];
    }
    double t_crand = now_ms() - t0;

    /* ── B4b: contiguous hoisted (pure arithmetic addressing) ───────── */
    GeosInode *cins[N_FILES];
    for (uint32_t k = 0; k < N_FILES; k++)
        cins[k] = geos_find(&cvol, names[k]);
    t0 = now_ms();
    for (uint32_t i = 0; i < RAND_ACCESSES; i++) {
        xs32(&st);
        uint32_t k = ((uint64_t)st * N_FILES) >> 32;
        uint32_t b = xs32(&st) & (FILE_BLOCKS - 1);
        sink += cvol.data[(cins[k]->block_start + b) * GEOS_BLOCK_SZ
                          + (i & (GEOS_BLOCK_SZ - 1))];
    }
    double t_crand_hoist = now_ms() - t0;

    /* ── B5: raw fread baseline (OS cache warm) ─────────────────────── */
    const int IOROUNDS = 50;
    t0 = now_ms();
    for (int r = 0; r < IOROUNDS; r++) {
        if (read_slice_at(path, rb, FILE_BYTES, -1) < (long)FILE_BYTES) {
            printf("FAIL: fread baseline\n"); return 1;
        }
    }
    double t_io = (now_ms() - t0) / IOROUNDS;

    /* ═══════════ REPORT ═══════════ */
    double mb_file = (double)FILE_BYTES / 1e6;
    printf("═══════════════════════════════════════════════════════════\n");
    printf("GeoFS Hyper Stream Benchmark — %.2f MB in-store (%.2f MB store)\n",
           mb_total, mb_total);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("  B1 hyper place   %10.2f ms   %8.2f MB/s   (scatter write)\n",
           t_place, mb_total / (t_place / 1000.0));
    printf("     seed search   %10.2f ms   (%u files x orbit scan)\n", t_seed, N_FILES);
    printf("  B4 contig place  %10.2f ms   %8.2f MB/s   (linear write)\n",
           t_cplace, mb_total / (t_cplace / 1000.0));
    printf("  B2 hyper seq     %10.2f ms   %8.2f MB/s   (%d rounds x %u files)\n",
           t_seq, mb_total / (t_seq / 1000.0), ROUNDS, N_FILES);
    printf("  B4 contig seq    %10.2f ms   %8.2f MB/s\n",
           t_cseq, mb_total / (t_cseq / 1000.0));
    printf("  B3 hyper rand64  %10.2f ms   %7.1f ns/op   %8.2f M ops/s\n",
           t_rand, t_rand * 1e6 / RAND_ACCESSES, RAND_ACCESSES / (t_rand / 1000.0) / 1000.0);
    printf("     by-name split: geos_find+walk+mem (name strcmp per op)\n");
    printf("  B3b hoisted walk %10.2f ms   %7.1f ns/op   %8.2f M ops/s\n",
           t_rand_hoist, t_rand_hoist * 1e6 / RAND_ACCESSES,
           RAND_ACCESSES / (t_rand_hoist / 1000.0) / 1000.0);
    printf("  B4 contig rand64 %10.2f ms   %7.1f ns/op   %8.2f M ops/s\n",
           t_crand, t_crand * 1e6 / RAND_ACCESSES, RAND_ACCESSES / (t_crand / 1000.0) / 1000.0);
    printf("  B4b hoisted arith%10.2f ms   %7.1f ns/op   %8.2f M ops/s\n",
           t_crand_hoist, t_crand_hoist * 1e6 / RAND_ACCESSES,
           RAND_ACCESSES / (t_crand_hoist / 1000.0) / 1000.0);
    printf("  B5 raw fread128K %10.2f ms   %8.2f MB/s   (disk, cached)\n",
           t_io, mb_file / (t_io / 1000.0));
    printf("  scatter/contig rand ratio: %.2fx\n", t_rand / t_crand);
    printf("  sink=%u (anti-dce)\n", (unsigned)(sink & 0xFFFFFFFFu));
    printf("═══════════════════════════════════════════════════════════\n");

    geos_volume_free(&vol);
    geos_volume_free(&cvol);
    free(slice); free(rb);
    return 0;
}
