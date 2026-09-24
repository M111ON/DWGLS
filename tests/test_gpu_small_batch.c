/* test_gpu_small_batch.c — Small-batch GPU dispatch overhead
 * ═══════════════════════════════════════════════════════════════════════════
 * Claim under test: DRamTile address + GearLock sync + JetBridge coalesce
 * keep per-call overhead low enough that GPU dispatch is viable from
 * batch = 1 chunk (64 B):
 *   A. no per-call init (warm ctx, no malloc, no memset)     < 5 µs/call
 *   B. address compute alone                                 < 500 ns/call
 *   C. small-batch "wants" coalesce at JetBridge             ≥ 10:1
 *      (first bridge at tick 11 → 1 GPU dispatch of 1728 pipes,
 *       then 1 dispatch per 10 ticks)
 *
 * Hard oracles (Part A) come from documented specs, not the functions
 * under test: mmap_offset = dram_addr × 64 (geo_dram_tile.h spec),
 * gear worlds = floor(ops / 162) at boundary (gear_lock.h spec),
 * bridge schedule re-derived from the documented rule "tick reaches
 * 11 → fire → restart at 1" (fibo_spine.h spec).
 *
 * No CUDA required — measures the CPU-side dispatch path a CUDA kernel
 * would be launched from (same pattern as test_gpu_pipeline.c).
 *
 * BUILD: make test-test_gpu_small_batch
 * RUN:   ./build/test-test_gpu_small_batch
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/infra/geo_gpu_pipeline.h"

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

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ~535 KB context — static, never on stack */
static GeoPipelineCtx g_ctx;
static volatile uint32_t g_sink;

static uint32_t xor_ref(const uint8_t *p, uint32_t n) {
    uint8_t x = 0;
    for (uint32_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

/* Reference bridge schedule — independent re-implementation of the
 * documented fibo_spine rule: tick advances 1..11, at 11 fire and
 * restart at 1 (tick 12 skipped). Not a call into fibo_spine_tick. */
static uint32_t ref_bridges(uint32_t n_ticks) {
    uint32_t gt = 0, b = 0;
    for (uint32_t i = 0; i < n_ticks; i++) {
        gt = (gt + 1) % 12;
        if (gt == 11) { b++; gt = 1; }
    }
    return b;
}

/* ── A1: batch = 1 index correctness (64 B = smallest GPU call) ── */
static void test_batch_one(void)
{
    printf("A1: batch = 1 chunk (64 B)\n");
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 7 + 13);

    geo_pipeline_init(&g_ctx);
    uint32_t n = geo_pipeline_build_index(&g_ctx, 0, 64, data, 64);
    CHECK(1, "n == 1 entry", n == 1);
    CHECK(1, "total_bytes == 64", g_ctx.pidx.total_bytes == 64);
    CHECK(1, "epoch == 1", g_ctx.pidx.epoch == 1);

    const GeoPipelineEntry *e = &g_ctx.pidx.entries[0];
    CHECK(1, "chunk_sz == 64", e->chunk_sz == 64);
    CHECK(1, "addr 0 → offset 0×64 == 0", e->dram_addr == 0 && e->byte_offset == 0);
    CHECK(1, "checksum == independent XOR",
          e->ref_checksum == xor_ref(data, 64));

    /* base at last slot: offset oracle 20735 × 64 from spec */
    n = geo_pipeline_build_index(&g_ctx, 20735, 64, data, 64);
    e = &g_ctx.pidx.entries[0];
    CHECK(1, "addr 20735 → offset == 20735×64",
          n == 1 && e->dram_addr == 20735 &&
          e->byte_offset == 20735ULL * 64);
}

/* ── A2: small-batch sweep — entries, offsets, uniqueness ── */
static void test_batch_sweep(void)
{
    printf("A2: batch sweep {1,2,3,7,16,64,256} × base {0,100}\n");
    static const uint32_t sizes[] = {1, 2, 3, 7, 16, 64, 256};
    static const uint32_t bases[] = {0, 100};
    uint8_t data[256 * 64];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 31 + 5);

    int ok_all = 1;
    geo_pipeline_init(&g_ctx);
    for (size_t bi = 0; bi < sizeof bases / sizeof bases[0] && ok_all; bi++) {
        for (size_t si = 0; si < sizeof sizes / sizeof sizes[0] && ok_all; si++) {
            uint32_t base = bases[bi], sz = sizes[si];
            uint32_t n = geo_pipeline_build_index(&g_ctx, base, sz * 64, data, 64);
            if (n != sz || g_ctx.pidx.total_bytes != sz * 64) { ok_all = 0; break; }

            uint8_t seen[256] = {0};  /* sz ≤ 256, base+c never wraps twice */
            for (uint32_t c = 0; c < sz; c++) {
                const GeoPipelineEntry *e = &g_ctx.pidx.entries[c];
                uint32_t addr_exp = (base + c) % DRAM_FULL;
                /* spec: mmap_offset = dram_addr × CHUNK_SZ — recomputed here */
                uint64_t off_exp = (uint64_t)addr_exp * GP_CHUNK_SZ;
                uint32_t ck_exp  = xor_ref(&data[c * 64], 64);
                if (e->dram_addr != addr_exp || e->byte_offset != off_exp ||
                    e->ref_checksum != ck_exp || e->chunk_sz != 64)
                    { ok_all = 0; break; }
                if (addr_exp < 256) {
                    if (seen[addr_exp]) { ok_all = 0; break; }  /* no collision */
                    seen[addr_exp] = 1;
                }
            }
        }
    }
    CHECK(2, "all sizes/bases: addr, offset=addr×64, checksum, unique", ok_all);
}

/* ── A3: warm persistence — build never re-inits the spine ── */
static void test_warm_persist(void)
{
    printf("A3: warm ctx — build does not touch spine state\n");
    uint8_t data[64];
    memset(data, 0x5A, 64);

    geo_pipeline_init(&g_ctx);
    geo_pipeline_tick_n(&g_ctx, 5);              /* tick_count = 5, no bridge yet */
    uint64_t tc = g_ctx.spine.tick_count;
    uint32_t cpu_ops = g_ctx.gear.cpu_ops;
    CHECK(3, "5 ticks: tick_count == 5", tc == 5);
    CHECK(3, "5 ticks: no bridge yet (first at 11)", g_ctx.bridges == 0);

    for (int i = 0; i < 100; i++)                /* 100 small-batch builds */
        geo_pipeline_build_index(&g_ctx, (uint32_t)i, 64, data, 64);

    CHECK(3, "spine.tick_count unchanged by builds", g_ctx.spine.tick_count == tc);
    CHECK(3, "gear.cpu_ops unchanged by builds", g_ctx.gear.cpu_ops == cpu_ops);
    CHECK(3, "epoch == 100 (one increment per build, no reset)",
          g_ctx.pidx.epoch == 100);
    CHECK(3, "is_init still 1", g_ctx.is_init == 1);
}

/* ── A4: GearLock world boundaries under small batches ── */
static void test_gear_small(void)
{
    printf("A4: GearLock — small batches reach exact world boundaries\n");
    CHECK(4, "sacred: 128 × 162 == 20736",
          GEAR_CPU_WORLD * GEAR_GPU_WORLD == 20736u);

    GearLock g;
    memset(&g, 0, sizeof g);
    for (int i = 0; i < 162; i++) gear_gpu_tick(&g, 1);   /* batch=1 × 162 */
    CHECK(4, "162 × batch1: ops == 162, worlds == 1",
          g.gpu_ops == 162 && g.gpu_worlds == 1);

    /* non-divisor small batches 40×4 = 160 (no world yet), +2 crosses */
    memset(&g, 0, sizeof g);
    for (int k = 0; k < 4; k++) gear_gpu_tick(&g, 40);
    CHECK(4, "40×4: ops == 160, worlds still 0",
          g.gpu_ops == 160 && g.gpu_worlds == 0);
    gear_gpu_tick(&g, 2);
    CHECK(4, "+2 crosses: ops == 162, worlds == 1",
          g.gpu_ops == 162 && g.gpu_worlds == 1);
    for (int i = 0; i < 162; i++) gear_gpu_tick(&g, 1);
    CHECK(4, "next 162: worlds == 2 (floor(324/162))",
          g.gpu_ops == 324 && g.gpu_worlds == 2);

    memset(&g, 0, sizeof g);
    for (int i = 0; i < 128; i++) gear_cpu_tick(&g);
    CHECK(4, "128 cpu ticks: worlds == 1",
          g.cpu_ops == 128 && g.cpu_worlds == 1);
}

/* ── A5: JetBridge coalesce — small batches → 1 GPU dispatch per bridge ── */
static void test_bridge_coalesce(void)
{
    printf("A5: JetBridge coalesce — small batches → 1 dispatch per bridge\n");
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 3 + 1);

    /* 11 ticks: bridge fires exactly at tick 11 — then stop BEFORE any
     * further build, so the assertion sees the post-bridge index as-is. */
    geo_pipeline_init(&g_ctx);
    for (int i = 0; i < 11; i++) {
        geo_pipeline_build_index(&g_ctx, (uint32_t)i, 64, data, 64);
        geo_pipeline_tick(&g_ctx);
    }
    CHECK(5, "11 wants → exactly 1 bridge", g_ctx.bridges == 1);
    CHECK(5, "1 bridge → 1 index of all 1728 pipes (1 dispatch)",
          g_ctx.pidx.n_entries == GP_PIPES);
    CHECK(5, "gear.gpu_ops == 1728 (one bridge counted once)",
          g_ctx.gear.gpu_ops == 1728);

    /* 12th tick: still inside same 12-tick cycle — no second bridge */
    geo_pipeline_build_index(&g_ctx, 12, 64, data, 64);
    geo_pipeline_tick(&g_ctx);
    CHECK(5, "tick 12: still 1 bridge (restart-at-1, next at 21)",
          g_ctx.bridges == 1);

    /* long run: schedule vs documented-rule oracle, dispatch reduction */
    geo_pipeline_init(&g_ctx);
    uint32_t wants = 0, bridges = 0;
    for (uint32_t i = 0; i < 120; i++) {
        geo_pipeline_build_index(&g_ctx, i % DRAM_FULL, 64, data, 64);
        wants++;
        bridges += geo_pipeline_tick(&g_ctx);
    }
    CHECK(5, "120 ticks: bridges == ref_bridges(120) == 11",
          bridges == ref_bridges(120) && bridges == 11);
    CHECK(5, "dispatch reduction ≥ 10:1 (120 wants → 11 dispatches)",
          wants / bridges >= 10);
    CHECK(5, "gpu_ops == bridges × 1728",
          g_ctx.gear.gpu_ops == bridges * GP_PIPES);
}

/* ── B1: address compute — header claims ~8.3 ns, bound 500 ns ── */
static void bench_addr(void)
{
    printf("B1: DRamTile address compute\n");
    const uint32_t REP = 50, INNER = 100000;   /* inner loop >> timer granularity */
    double best = 1e18;
    for (uint32_t r = 0; r < REP; r++) {
        uint32_t sink = 0;
        double t0 = now_ns();
        for (uint32_t c = 0; c < INNER; c++)
            sink += dram_addr(c % DRAM_ANCHORS, c & 7u, (c >> 3) & 7u, c & 1u);
        double dt = now_ns() - t0;
        g_sink += sink;
        if (dt < best) best = dt;
    }
    double ns = best / INNER;
    printf("      %8.2f ns/call (min-of-%u, inner=%u)\n", ns, REP, INNER);
    CHECK(6, "address < 500 ns/call", ns < 500.0);
}

/* ── B2: warm vs cold per-call at batch = 1 ── */
static void bench_warm_vs_cold(void)
{
    printf("B2: per-call overhead at batch = 1 (warm vs cold re-init)\n");
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 13 + 7);

    /* warm: persistent ctx, build only — batch INNER calls per sample */
    geo_pipeline_init(&g_ctx);
    const uint32_t REP = 50, INNER = 2000;
    double best_w = 1e18;
    for (uint32_t r = 0; r < REP; r++) {
        uint32_t sink = 0;
        double t0 = now_ns();
        for (uint32_t c = 0; c < INNER; c++)
            sink += geo_pipeline_build_index(&g_ctx, c % DRAM_FULL, 64, data, 64);
        double dt = now_ns() - t0;
        g_sink += sink;
        if (dt < best_w) best_w = dt;
    }

    /* cold: full re-init every call = "normal" setup-per-call path */
    const uint32_t CREP = 50, CINNER = 100;
    double best_c = 1e18;
    for (uint32_t r = 0; r < CREP; r++) {
        uint32_t sink = 0;
        double t0 = now_ns();
        for (uint32_t c = 0; c < CINNER; c++) {
            geo_pipeline_init(&g_ctx);
            sink += geo_pipeline_build_index(&g_ctx, c % DRAM_FULL, 64, data, 64);
        }
        double dt = now_ns() - t0;
        g_sink += sink;
        if (dt < best_c) best_c = dt;
    }
    best_w /= INNER;
    best_c /= CINNER;

    printf("      warm (persistent) : %8.1f ns/call (min-of-%u×%u)\n",
           best_w, REP, INNER);
    printf("      cold (init+build) : %8.1f ns/call (min-of-%u×%u)\n",
           best_c, CREP, CINNER);
    printf("      ratio             : %8.1fx\n", best_c / best_w);
    CHECK(7, "warm < cold (init not paid per call)", best_w < best_c);
    CHECK(7, "warm batch=1 < 5 µs budget (no malloc/memset/log per call)",
          best_w < 5000.0);
}

/* ── B3: warm sweep — receipt across batch sizes ── */
static void bench_sweep(void)
{
    printf("B3: warm ns/call sweep (fixed cost + per-chunk)\n");
    static const uint32_t sizes[] = {1, 4, 16, 64, 256, 1024};
    static uint8_t data[1024 * 64];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 17 + 3);

    geo_pipeline_init(&g_ctx);
    printf("      %8s %12s %12s\n", "chunks", "ns/call", "ns/chunk");
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        uint32_t sz = sizes[si];
        /* keep timed window well above timer granularity */
        const uint32_t REP = 30;
        const uint32_t INNER = (sz <= 16) ? 2000 : (sz <= 256 ? 200 : 50);
        double best = 1e18;
        for (uint32_t r = 0; r < REP; r++) {
            uint32_t sink = 0;
            double t0 = now_ns();
            for (uint32_t c = 0; c < INNER; c++)
                sink += geo_pipeline_build_index(&g_ctx, c % DRAM_FULL,
                                                 sz * 64, data, 64);
            double dt = now_ns() - t0;
            g_sink += sink;
            if (dt < best) best = dt;
        }
        best /= INNER;
        printf("      %8u %12.1f %12.2f\n", sz, best, best / sz);
    }
    /* receipt only — budget assertion lives on batch=1 in B2 */
    CHECK(8, "sweep completed (sink kept)", g_sink != 0);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  Small-Batch GPU Overhead — DRamTile+GearLock+JetBridge\n");
    printf("═══════════════════════════════════════════════════\n\n");

    test_batch_one();
    test_batch_sweep();
    test_warm_persist();
    test_gear_small();
    test_bridge_coalesce();
    bench_addr();
    bench_warm_vs_cold();
    bench_sweep();

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════\n");
    return fail;
}
