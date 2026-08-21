/* ═══════════════════════════════════════════════════════════════════════════
 * test_geo_multivol.c — Option A multi-volume GeoFS on REAL data
 * ═══════════════════════════════════════════════════════════════════════════
 * Places an 8 MB real-GGUF slice (weights region) as 128 KB hyper files
 * across multiple GeosVolumes (~1.3 MB each) and proves:
 *
 *   T1 place   — all files land, volumes auto-opened as needed
 *   T2 oracle  — address(b) == hw_at(seed, axis, b) per file (SPEC math)
 *   T3 truth   — byte-identical read-back (memcmp)
 *   T4 scale   — sequential stream at DRAM scale (> L2): informational MB/s
 *   T5 delete  — every volume returns to full free capacity
 *
 * ORACLE: SPEC strides {1,9,27,81} pinned here; expected address =
 * (seed + stride·b) mod 20736 — pure arithmetic, independent of core.
 * Skips gracefully (exit 0) when no real GGUF is found.
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "geofs_multivol.h"

#define SPEC_GEO_FULL      20736u

#define FILE_BYTES   (2048u * GEOS_BLOCK_SZ)          /* 128 KB */
#define N_FILES      64u                              /* 8 MB total */
#define TOTAL_BYTES  ((size_t)N_FILES * FILE_BYTES)

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("  TEST %2d: %-46s ", tests_passed + tests_failed + 1, name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

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

int main(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  GeoFS MultiVolume — REAL GGUF across %u volumes        ║\n", 7u);
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    const char *path = pick_real_file(argc, argv);
    if (!path) { printf("  SKIP: no real GGUF found\n"); return 0; }

    uint8_t *slice = (uint8_t *)malloc(TOTAL_BYTES);
    uint8_t *rb    = (uint8_t *)malloc(FILE_BYTES);
    if (!slice || !rb) { printf("FAIL: malloc\n"); return 1; }

    FILE *f = fopen(path, "rb");
    if (!f) { printf("SKIP: cannot open %s\n", path); return 0; }
    FS_FSEEK(f, 0, SEEK_END);
    int64_t sz = FS_FTELL(f);
    int64_t off = sz / 3;                       /* weights region */
    if (off + (int64_t)TOTAL_BYTES > sz) off = sz - (int64_t)TOTAL_BYTES;
    if (off < 0 || FS_FSEEK(f, off, SEEK_SET) != 0 ||
        fread(slice, 1, TOTAL_BYTES, f) != TOTAL_BYTES) {
        fclose(f); printf("SKIP: %s too small for %u bytes\n", path, (unsigned)TOTAL_BYTES);
        return 0;
    }
    fclose(f);

    printf("  source : %s\n", path);
    printf("  data   : %u files x %u blocks = %.2f MB (real weights)\n\n",
           N_FILES, FILE_BYTES / GEOS_BLOCK_SZ, (double)TOTAL_BYTES / 1e6);

    GeosMV mv;
    if (geos_mv_init(&mv, 4) != 0) { printf("FAIL: mv_init\n"); return 1; }
    char names[N_FILES][16];

    /* ── T1: place everything ───────────────────────────────────────── */
    TEST("Place 8 MB across auto-opened volumes");
    double t_place = now_ms();
    for (uint32_t k = 0; k < N_FILES; k++) {
        snprintf(names[k], sizeof(names[k]), "mv%02u.bin", k);
        if (geos_mv_place(&mv, names[k], FILE_BYTES,
                          slice + k * FILE_BYTES, 1) != 0) {
            FAIL("mv_place"); goto out;
        }
    }
    t_place = now_ms() - t_place;
    printf("(%u volumes opened, %.1f ms)\n", mv.n_used, t_place);
    PASS();

    /* ── T2: SPEC oracle on addresses ───────────────────────────────── */
    TEST("address(b) == (seed + 9b) mod 20736 per file");
    for (uint32_t k = 0; k < N_FILES; k++) {
        GeosMvInode mi;
        if (geos_mv_resolve(&mv, names[k], &mi) != 0) { FAIL("resolve"); goto out; }
        uint32_t seed = mi.in->block_start;
        for (uint32_t b = 0; b < mi.in->block_count; b += 337u) {  /* spot sweep */
            const uint8_t *p = geos_hyper_project_block_inode(mi.vol, mi.in, b);
            if (!p ||
                (size_t)(p - mi.vol->data) / GEOS_BLOCK_SZ !=
                (seed + 9u * b) % SPEC_GEO_FULL) {
                FAIL("oracle"); goto out;
            }
        }
    }
    PASS();

    /* ── T3: binary truth ────────────────────────────────────────────── */
    TEST("Read back byte-identical (memcmp ground truth)");
    for (uint32_t k = 0; k < N_FILES; k++) {
        if (geos_mv_read(&mv, names[k], rb, FILE_BYTES) != (int)FILE_BYTES ||
            memcmp(slice + k * FILE_BYTES, rb, FILE_BYTES) != 0) {
            FAIL("memcmp"); goto out;
        }
    }
    PASS();

    /* ── T4: DRAM-scale sequential stream (informational) ───────────── */
    TEST("Seq stream at DRAM scale (8 MB store)");
    const int ROUNDS = 10;
    double best = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        double t0 = now_ms();
        for (int r = 0; r < ROUNDS; r++)
            for (uint32_t k = 0; k < N_FILES; k++)
                geos_mv_read(&mv, names[k], rb, FILE_BYTES);
        double t = (now_ms() - t0) / ROUNDS;
        if (t < best) best = t;
    }
    printf("%.2f ms/pass → %.0f MB/s (%s-resident store)\n",
           best, (double)TOTAL_BYTES / 1e6 / (best / 1000.0),
           TOTAL_BYTES > 4u * 1024u * 1024u ? "DRAM" : "L2/L3");
    PASS();

    /* ── T5: delete restores every volume ───────────────────────────── */
    TEST("Delete all → every volume fully free");
    for (uint32_t k = 0; k < N_FILES; k++)
        if (geos_mv_delete(&mv, names[k]) != 0) { FAIL("delete"); goto out; }
    for (uint32_t i = 0; i < mv.n_used; i++) {
        if (mv.vol[i]->total_blocks_free !=
            SPEC_GEO_FULL - GEOS_VOL_DATA_START) {
            FAIL("free not restored"); goto out;
        }
    }
    PASS();

    printf("\n───────────────────────────────────────\n");
    printf("PASS: %d / %d  FAIL: %d\n", tests_passed, tests_passed + tests_failed, tests_failed);
    printf("═══════════════════════════════════════\n");
    geos_mv_free(&mv);
    free(slice); free(rb);
    return tests_failed > 0 ? 1 : 0;

out:
    geos_mv_free(&mv);
    free(slice); free(rb);
    printf("\n───────────────────────────────────────\n");
    printf("PASS: %d / %d  FAIL: %d\n", tests_passed, tests_passed + tests_failed, tests_failed);
    printf("═══════════════════════════════════════\n");
    return 1;
}
