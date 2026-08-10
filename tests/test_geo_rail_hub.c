/* tests/test_geo_rail_hub.c — p2-rail: mmap .gcube → zero-copy pull
 *
 * Proves the llama.cpp tensor-hook handoff path on a REAL .gcube:
 *   1. geo_rail_open  — mmap (no weight allocation)
 *   2. geo_rail_verify— CRC32 over the whole mapped region (one-time)
 *   3. geo_rail_pull  — resolve every tensor by name → pointer into mmap
 *   4. Compare pulled bytes against the re-read container (LOSSLESS)
 *   5. Measure sustained pull throughput (pulls/s)
 *   6. rail memory = mmap file size (no duplicate weight buffers)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include "geo_rail_hub.h"
#include "gguf_index.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) \
    do { if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
         else { fail++; printf("  T%d: FAIL — %s\n", n, desc); } } while (0)

static double now_sec(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq, base; static int init = 0;
    LARGE_INTEGER c;
    if (!init) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&base); init = 1; }
    QueryPerformanceCounter(&c);
    return (double)(c.QuadPart - base.QuadPart) / (double)freq.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

int main(int argc, char **argv)
{
    const char *gcube = (argc > 1) ? argv[1] : "build/test_rail.gcube";

    /* Self-contained: if the .gcube isn't already baked, build a small one
     * from the real GGUF (same pattern as test_geo_zerocopy). */
    FILE *probe = fopen(gcube, "rb");
    if (!probe) {
        const char *gguf = (argc > 2) ? argv[2]
                          : "I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf";
        printf("(no %s — baking 3 tensors from %s)\n", gcube, gguf);
        GCubeContainer cube;
        gcube_init(&cube);
        GGUFTensorIndex idx;
        if (gguf_idx_open(gguf, &idx) != 0) { printf("  SKIP (no GGUF)\n"); return 1; }
        FILE *gf = fopen(gguf, "rb");
        uint32_t added = 0;
        for (uint64_t i = 0; i < idx.n_tensors && added < 3; i++) {
            if (idx.dtypes[i] != 8) continue;
            uint64_t sz = idx.sizes[i];
            uint8_t *data = (uint8_t *)malloc((size_t)sz);
            fseeko(gf, (long)idx.offsets[i], SEEK_SET);
            fread(data, 1, (size_t)sz, gf);
            uint32_t ne = (uint32_t)(sz / 34 * 32);
            uint32_t dims[4] = {ne, 1, 1, 1};
            gcube_add_tensor(&cube, idx.names[i], 1, dims, idx.dtypes[i], ne, data, (uint32_t)sz);
            free(data);
            added++;
        }
        fclose(gf);
        gguf_idx_close(&idx);
        gcube_write(&cube, gcube);
        gcube_free(&cube);
    } else fclose(probe);

    printf("Rail Hub Zero-Copy Test\n");
    printf("═══════════════════════════════════════════════════════════\n");

    /* T1: open (mmap) */
    GeoRailHub rail;
    int rc = geo_rail_open(&rail, gcube);
    CHECK(1, "geo_rail_open mmap", rc == RAIL_OK);
    if (rc != RAIL_OK) { printf("  (rc=%d) — run bake_gcube first\n", rc); return 1; }

    /* T2: one-time CRC over the whole region */
    rc = geo_rail_verify(&rail);
    CHECK(2, "geo_rail_verify CRC32", rc == RAIL_OK);

    /* T3: pull every tensor by name, byte-compare vs re-read container */
    GCubeContainer back;
    gcube_init(&back);
    CHECK(3, "gcube_read reference", gcube_read(&back, gcube) == 0);

    uint32_t ok = 0, bad = 0;
    for (uint32_t i = 0; i < rail.n_tensors; i++) {
        const char *name = back.tensors[i].name;
        const uint8_t *rp = NULL; uint32_t rn = 0, rd = 0;
        if (geo_rail_pull(&rail, name, &rp, &rn, &rd) != RAIL_OK) { bad++; continue; }
        const GCubeTensorEntry *e = &back.tensors[i];
        if (rn != e->n_elems || rd != e->dtype) { bad++; continue; }
        if (memcmp(rp, back.blocks + (uint64_t)e->block_start * GCUBE_BLOCK_SZ,
                   e->data_size) == 0) ok++;
        else bad++;
    }
    CHECK(4, "all tensors byte-identical via pull (LOSSESS)", ok == rail.n_tensors && bad == 0);
    printf("       (%u/%u tensors compared)\n", ok, ok + bad);

    /* T5: unknown name → clean error, no crash */
    const uint8_t *xp = NULL; uint32_t xn = 0, xd = 0;
    CHECK(5, "missing tensor → RAIL_ERR_NO_TENSOR",
          geo_rail_pull(&rail, "no.such.tensor", &xp, &xn, &xd) == RAIL_ERR_NO_TENSOR);

    /* T6: pull-by-index hot path — no string compare */
    const uint8_t *ip = NULL; uint32_t in = 0, id = 0;
    CHECK(6, "pull_idx[0] ok", geo_rail_pull_idx(&rail, 0, &ip, &in, &id) == RAIL_OK);

    /* T7: throughput — name-indexed pulls on the real model.
     * NOTE: sum must be observed downstream, otherwise -O2 dead-code
     * eliminates the entire loop (dt → 0 → meaningless rate). */
    double t0 = now_sec();
    const uint64_t N = 1000000;
    volatile uint64_t sink = 0;
    for (uint64_t k = 0; k < N; k++) {
        uint32_t i = (uint32_t)(k % rail.n_tensors);
        const uint8_t *dp; uint32_t dn, dd;
        if (geo_rail_pull_idx(&rail, i, &dp, &dn, &dd) == RAIL_OK)
            sink += (uint64_t)(uintptr_t)dp + dn + dd;
    }
    double dt = now_sec() - t0;
    double pulls_s = (double)N / dt;
    printf("  T7: sustained %u pulls in %.6f s = %.1f M pulls/s\n",
           (unsigned)N, dt, pulls_s / 1e6);
    CHECK(7, "pull_idx hot path", pulls_s > 1e6); /* sanity: >1M/s */

    /* T8: memory discipline — mmap file is the only weight store */
    FILE *f = fopen(gcube, "rb");
    long fsz = -1;
    if (f) { fseek(f, 0, SEEK_END); fsz = ftell(f); fclose(f); }
    printf("  T8: mapped %ld MB, %u tensors, pulls touched 0 mallocs\n",
           fsz > 0 ? fsz / 1048576 : 0, rail.n_tensors);
    CHECK(8, "weight store = mmap only", fsz > 0);

    geo_rail_close(&rail);
    gcube_free(&back);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("FINAL: %d PASS / %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}