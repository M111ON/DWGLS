/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_hyper_real.c — GeoFS hyperbolic on REAL data (real GGUF)
 * ═══════════════════════════════════════════════════════════════════════════
 * Round-trips a real model file's raw bytes through hyper key-frame files.
 *
 * INTEGRITY: RDH (not FNV-1a) — rdh_capture() is the data→address walk
 * (collection/rdh/rdh_capture.h). Each real chunk maps to a flat key in the
 * 144×144 field; the round-trip must land on the SAME key. Binary truth is
 * memcmp; RDH is the project's geometric integrity signal (no hash).
 *
 * ORACLE: SPEC stride {1,9} pinned here (mixed-radix 20736 = 2^8·3^4);
 * expected scatter addresses = (seed + 9·b) mod 20736 — pure arithmetic.
 *
 * Data: middle slice of a real GGUF (real quantized weight bytes, high
 * entropy), split into axis-1 hyper files of ≤2048 blocks (orbit 2304).
 * Skips gracefully (exit 0) when no real GGUF is found on this machine.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "geofs_core.h"
/* core/geo_rdh_addr.h (pulled by geofs chain) and collection/rdh/rdh_addr.h
 * both define `rdh_decompose` with different signatures — rename the
 * collection one so rdh_capture() can coexist with GeoFS. */
#define rdh_decompose rdh_capture_rdh_decompose
#include "../collection/rdh/rdh_capture.h"
#undef rdh_decompose

/* ── SPEC constants (independent oracle) ───────────────────────────── */
#define SPEC_STRIDE_AXIS1  9u
#define SPEC_GEO_FULL      20736u
#define SPEC_FREE_INIT     (SPEC_GEO_FULL - GEOS_VOL_DATA_START) /* 20480 */

#define HYPER_BLOCKS       2048u   /* blocks per hyper file (orbit 2304) */
#define HYPER_CHUNK_SZ     (HYPER_BLOCKS * GEOS_BLOCK_SZ)  /* 128 KB */
#define SLICE_BYTES        (1024u * 1024u)                 /* 1 MB slice */
#define N_FILES            (SLICE_BYTES / HYPER_CHUNK_SZ)  /* 8 files */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("  TEST %2d: %-48s ", tests_passed + tests_failed + 1, name); \
    } while(0)

#define PASS() do { printf("✅ PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("❌ FAIL: %s\n", msg); tests_failed++; } while(0)

/* RDH flat key of a real chunk — data walks to its home address */
static int64_t rdh_of(const uint8_t *p, size_t n) {
    return rdh_capture(p, n, &RDH_CAPTURE_144);
}

#ifdef _WIN32
#define FS_FSEEK _fseeki64
#define FS_FTELL _ftelli64
#else
#define FS_FSEEK fseeko
#define FS_FTELL ftello
#endif

/* ── known real GGUF files on this machine (fallback order) ───────── */
static const char *CANDIDATES[] = {
    "I:\\DWGLS\\build\\qwen05-direct.gguf",
    "I:\\DWGLS\\build\\qwen05-reemit.gguf",
    "I:\\llama\\llama.cpp\\models\\ggml-vocab-qwen2.gguf",
    "I:\\llama\\llama.cpp\\models\\ggml-vocab-falcon.gguf",
    "I:\\llama\\llama.cpp\\models\\ggml-vocab-deepseek-coder.gguf",
    NULL
};

/* pick a real GGUF: argv[1] > $DWGLS_GGUF > known model paths */
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

/* read SLICE_BYTES from the MIDDLE of the file (real weight bytes) */
static long read_real_slice(const char *path, uint8_t *buf) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (FS_FSEEK(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    int64_t sz = FS_FTELL(f);
    if (sz < 64) { fclose(f); return -1; }
    int64_t off = sz / 2;
    if (off + SLICE_BYTES > sz) off = sz - SLICE_BYTES;
    if (off < 0) off = 0;
    if (FS_FSEEK(f, off, SEEK_SET) != 0) { fclose(f); return -1; }
    size_t got = fread(buf, 1, SLICE_BYTES, f);
    fclose(f);
    return (long)got;
}

/* seeds with pairwise-disjoint residues mod 9 → walks cannot collide */
static int seed_ok(const uint8_t *map, uint32_t seed) {
    for (uint32_t b = 0; b < HYPER_BLOCKS; b++) {
        uint32_t addr = (seed + SPEC_STRIDE_AXIS1 * b) % SPEC_GEO_FULL;
        if (addr < GEOS_VOL_DATA_START) return 0;
        if (map[addr / 8] & (1u << (addr % 8))) return 0;
    }
    return 1;
}

static int run(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  GeoFS Hyperbolic — REAL GGUF data (RDH integrity)     ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* locate a real file */
    const char *path = pick_real_file(argc, argv);
    if (!path) { printf("  SKIP: no real GGUF found\n\n"); return 0; }

    uint8_t *slice = (uint8_t *)malloc(SLICE_BYTES);
    if (!slice) { printf("  FAIL: malloc slice\n\n"); return 1; }

    long got = read_real_slice(path, slice);
    if (got < (long)HYPER_CHUNK_SZ) {
        printf("  SKIP: %s too small (read %ld bytes)\n\n", path, got);
        free(slice); return 0;
    }
    long n_bytes = (got / (long)HYPER_CHUNK_SZ) * (long)HYPER_CHUNK_SZ;
    int n_files = (int)(n_bytes / (long)HYPER_CHUNK_SZ);

    printf("  source : %s\n", path);
    printf("  slice  : %ld bytes (middle, real weight bytes)\n", n_bytes);
    printf("  files  : %d hyper files x %u blocks (axis 1, scatter)\n\n", n_files, HYPER_BLOCKS);

    GeosVolume vol;
    geos_volume_init(&vol);

    /* ── T1: place each real chunk as a scatter hyper file ─────────── */
    TEST("Place real chunks (scatter, unique seeds)");
    uint32_t seeds[16]; char names[16][16];
    for (int k = 0; k < n_files && k < 16; k++) {
        snprintf(names[k], sizeof(names[k]), "w%02d.bin", k);
        uint32_t seed = 300 + (uint32_t)k * 1100u;
        while (!seed_ok(vol.block_map, seed)) seed++;
        GeosInode *in = geos_hyper_place(&vol, names[k], HYPER_CHUNK_SZ,
                                         slice + (size_t)k * HYPER_CHUNK_SZ,
                                         seed, 1);
        if (!in) { FAIL("place failed"); goto out; }
        seeds[k] = seed;
        if (in->block_start != seed) { FAIL("seed mismatch"); goto out; }
    }
    PASS();

    /* ── T2: MAP — addresses are computed, not stored ──────────────── */
    TEST("Address(b) == (seed + 9·b) mod 20736 for every block");
    for (int k = 0; k < n_files; k++) {
        for (uint32_t b = 0; b < HYPER_BLOCKS; b++) {
            uint32_t addr = geos_hyper_address(&vol, names[k], b);
            if (addr != (seeds[k] + SPEC_STRIDE_AXIS1 * b) % SPEC_GEO_FULL) {
                FAIL("address formula broken"); goto out;
            }
        }
    }
    PASS();

    /* ── T3: binary truth — byte-identical round-trip ──────────────── */
    TEST("Read back byte-identical (memcmp ground truth)");
    uint8_t *rb = (uint8_t *)malloc(HYPER_CHUNK_SZ);
    if (!rb) { FAIL("malloc rb"); goto out; }
    for (int k = 0; k < n_files; k++) {
        if (geos_hyper_read(&vol, names[k], rb, HYPER_CHUNK_SZ) != (int)HYPER_CHUNK_SZ) {
            FAIL("hyper_read size"); free(rb); goto out;
        }
        if (memcmp(slice + (size_t)k * HYPER_CHUNK_SZ, rb, HYPER_CHUNK_SZ) != 0) {
            FAIL("data mismatch"); free(rb); goto out;
        }
    }
    free(rb);
    PASS();

    /* ── T4: RDH integrity — data → same flat key (no FNV-1a) ──────── */
    TEST("RDH capture: read-back lands on same flat key");
    uint8_t *rb2 = (uint8_t *)malloc(HYPER_CHUNK_SZ);
    if (!rb2) { FAIL("malloc rb2"); goto out; }
    for (int k = 0; k < n_files; k++) {
        geos_hyper_read(&vol, names[k], rb2, HYPER_CHUNK_SZ);
        int64_t k_src = rdh_of(slice + (size_t)k * HYPER_CHUNK_SZ, HYPER_CHUNK_SZ);
        int64_t k_rb  = rdh_of(rb2, HYPER_CHUNK_SZ);
        if (k_src != k_rb) {
            printf("chunk %d src=%lld rb=%lld", k, (long long)k_src, (long long)k_rb);
            FAIL("RDH key mismatch"); free(rb2); goto out;
        }
    }
    printf("(%d/16 keys match)\n", n_files);
    free(rb2);
    PASS();

    /* ── T5: per-block RDH sweep (16k independent checks) ──────────── */
    TEST("Per-block RDH sweep over every 64-byte block");
    int total_blocks = n_files * (int)HYPER_BLOCKS;
    int mismatches = 0;
    for (int k = 0; k < n_files; k++) {
        for (uint32_t b = 0; b < HYPER_BLOCKS; b++) {
            const uint8_t *pb = geos_hyper_project_block(&vol, names[k], b);
            if (!pb) { mismatches++; continue; }
            if (rdh_of(pb, GEOS_BLOCK_SZ) !=
                rdh_of(slice + (size_t)k * HYPER_CHUNK_SZ + b * GEOS_BLOCK_SZ, GEOS_BLOCK_SZ))
                mismatches++;
            if (mismatches > 0) break;
        }
        if (mismatches > 0) break;
    }
    if (mismatches) { FAIL("per-block RDH mismatch"); goto out; }
    printf("(%d blocks clean)\n", total_blocks);
    PASS();

    /* ── T6: serialize → deserialize → read → RDH stable ───────────── */
    TEST("Serialize → deserialize → RDH keys stable");
    if (geos_serialize(&vol, "build/test_hyper_real.geofs") != 0) { FAIL("serialize"); goto out; }
    GeosVolume vol2;
    memset(&vol2, 0, sizeof(vol2));
    if (geos_deserialize(&vol2, "build/test_hyper_real.geofs") != 0) { FAIL("deserialize"); goto out; }
    uint8_t *rb3 = (uint8_t *)malloc(HYPER_CHUNK_SZ);
    if (!rb3) { FAIL("malloc rb3"); goto out; }
    for (int k = 0; k < n_files; k++) {
        geos_hyper_read(&vol2, names[k], rb3, HYPER_CHUNK_SZ);
        if (rdh_of(rb3, HYPER_CHUNK_SZ) !=
            rdh_of(slice + (size_t)k * HYPER_CHUNK_SZ, HYPER_CHUNK_SZ)) {
            FAIL("RDH after roundtrip"); free(rb3); geos_volume_free(&vol2); goto out;
        }
    }
    free(rb3); geos_volume_free(&vol2);
    PASS();

    /* ── T7: unplace restores free space; same seeds reusable ──────── */
    TEST("Unplace all → free restored, seeds reusable");
    for (int k = 0; k < n_files; k++) {
        if (geos_delete(&vol, names[k]) != 0) { FAIL("delete"); goto out; }
    }
    if (vol.total_blocks_free != SPEC_FREE_INIT) {
        FAIL("free not fully restored"); goto out;
    }
    uint32_t seed0 = 300;
    while (!seed_ok(vol.block_map, seed0)) seed0++;
    if (!geos_hyper_place(&vol, "reuse.bin", HYPER_CHUNK_SZ, slice, seed0, 1)) {
        FAIL("reuse place"); goto out;
    }
    PASS();

    printf("\n  real-data verdict: lossless (%d blocks, %d files, scatter axis 1)\n",
           total_blocks, n_files);

    geos_volume_free(&vol);
    free(slice);
    printf("\n───────────────────────────────────────\n");
    printf("PASS: %d / %d  FAIL: %d\n", tests_passed, tests_passed + tests_failed, tests_failed);
    printf("═══════════════════════════════════════\n");
    return tests_failed > 0 ? 1 : 0;

out:
    geos_volume_free(&vol);
    free(slice);
    printf("\n───────────────────────────────────────\n");
    printf("PASS: %d / %d  FAIL: %d\n", tests_passed, tests_passed + tests_failed, tests_failed);
    printf("═══════════════════════════════════════\n");
    return 1;
}

int main(int argc, char **argv) {
    return run(argc, argv);
}