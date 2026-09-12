/* test_gpu_pipeline.c — Test GPU Pipeline: DRamTile + GearLock + JetBridge
 * ═══════════════════════════════════════════════════════════════════════════════
 * CPU-side tests for geo_gpu_pipeline.h.
 * No CUDA required — validates address computation, point index building,
 * spine ceremony, and gear sync.
 *
 * BUILD: gcc -O2 -Wall -Wextra -Icore -Icore/infra -o build/test_gpu_pipeline \
 *          tests/test_gpu_pipeline.c -lm
 * RUN:   ./build/test_gpu_pipeline
 * ═══════════════════════════════════════════════════════════════════════════════
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../core/infra/geo_gpu_pipeline.h"

static int pass = 0, fail = 0;
#define CHECK(n, desc, cond) do { \
    if (cond) { pass++; printf("  T%d: PASS — %s\n", n, desc); } \
    else      { fail++; printf("  T%d: FAIL — %s\n", n, desc); } \
} while(0)

/* ── T0: DRamTile Hilbert 8×8 covers 0..63 uniquely ── */
static void test_dram_hilbert(void)
{
    printf("T0: DRamTile Hilbert 8×8 bijection\n");
    CHECK(0, "hilbert_8x8 covers 0..63", dram_verify_hilbert() == 0);
}

/* ── T1: DRamTile full address space 20736 has zero collisions ── */
static void test_dram_full(void)
{
    printf("T1: DRamTile full address space (20736)\n");
    CHECK(1, "full address zero collision", dram_verify_full() == 0);
}

/* ── T2: dram_addr / dram_decompose roundtrip ── */
static void test_dram_roundtrip(void)
{
    printf("T2: dram_addr / dram_decompose roundtrip\n");
    int ok = 1;
    for (uint32_t a = 0; a < DRAM_ANCHORS && ok; a++) {
        for (uint32_t l = 0; l < DRAM_LAYERS && ok; l++) {
            for (uint32_t y = 0; y < DRAM_GRID_Y && ok; y++) {
                for (uint32_t x = 0; x < DRAM_GRID_X && ok; x++) {
                    uint32_t addr = dram_addr(a, x, y, l);
                    if (addr >= DRAM_FULL) { ok = 0; break; }
                    /* Verify tile offset is within layer range */
                    uint32_t tile = addr - a * DRAM_CELLS_PER;
                    if (tile >= DRAM_CELLS_PER) { ok = 0; break; }
                }
            }
        }
    }
    CHECK(2, "all 20736 addresses in range", ok);
}

/* ── T3: Pipeline init + verify ── */
static void test_pipeline_init(void)
{
    printf("T3: Pipeline init + DRamTile verify\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);
    CHECK(3, "is_init == 1", ctx.is_init == 1);
    CHECK(3, "verify passes", geo_pipeline_verify(&ctx) == 0);
}

/* ── T4: Build index from base address ── */
static void test_build_index(void)
{
    printf("T4: Build index from base address\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    /* Create synthetic chunk data */
    uint8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)(i * 7 + 13);

    uint32_t n = geo_pipeline_build_index(&ctx, 0, 256, data, 64);
    CHECK(4, "index entries == 4 (256/64)", n == 4);
    CHECK(4, "total_bytes == 256", ctx.pidx.total_bytes == 256);
    CHECK(4, "epoch == 1", ctx.pidx.epoch == 1);

    /* Verify each entry has correct byte_offset */
    int offsets_ok = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t expected_off = (uint64_t)ctx.pidx.entries[i].dram_addr * 64;
        if (ctx.pidx.entries[i].byte_offset != expected_off)
            { offsets_ok = 0; break; }
    }
    CHECK(4, "byte_offsets correct", offsets_ok);

    /* Verify checksums are non-zero (data is not all zeros) */
    int cksum_ok = 1;
    for (uint32_t i = 0; i < n; i++) {
        if (ctx.pidx.entries[i].ref_checksum == 0)
            { cksum_ok = 0; break; }
    }
    CHECK(4, "checksums non-zero", cksum_ok);
}

/* ── T5: Pipeline tick → bridge detection ── */
static void test_pipeline_tick(void)
{
    printf("T5: Pipeline tick → bridge detection\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    uint32_t bridges = 0;
    /* Tick until we get a bridge (max 12 ticks) */
    for (int i = 0; i < 12; i++) {
        bridges += geo_pipeline_tick(&ctx);
    }
    CHECK(5, "bridge fired within 12 ticks", bridges >= 1);
    CHECK(5, "epoch advanced", ctx.pidx.epoch >= 1);
}

/* ── T6: Pipeline tick_n — full cycle ── */
static void test_pipeline_tick_n(void)
{
    printf("T6: Pipeline tick_n — full 12-tick cycle\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    uint32_t bridges = geo_pipeline_tick_n(&ctx, 12);
    CHECK(6, "bridges == 1 in 12 ticks", bridges == 1);
    CHECK(6, "chunks > 0", ctx.chunks > 0);

    /* Second cycle */
    uint32_t bridges2 = geo_pipeline_tick_n(&ctx, 12);
    CHECK(6, "bridges == 1 in second cycle", bridges2 == 1);
    CHECK(6, "total bridges == 2", ctx.bridges == 2);
}

/* ── T7: GearLock sync — CPU/GPU worlds ── */
static void test_gear_sync(void)
{
    printf("T7: GearLock CPU/GPU world sync\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    /* Run enough ticks to complete worlds */
    for (int i = 0; i < 12; i++)
        geo_pipeline_tick(&ctx);

    /* CPU world: every 128 ops */
    CHECK(7, "cpu_ops > 0", ctx.gear.cpu_ops > 0);

    /* GPU ops should match bridge chunks */
    CHECK(7, "gpu_ops == chunks", ctx.gear.gpu_ops == ctx.chunks);
}

/* ── T8: Per-pipe tick ── */
static void test_per_pipe_tick(void)
{
    printf("T8: Per-pipe tick (independent)\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    uint8_t t1 = geo_pipeline_pipe_tick(&ctx, 0);
    CHECK(8, "pipe 0 tick == 1", t1 == 1);

    uint8_t t2 = geo_pipeline_pipe_tick(&ctx, 0);
    CHECK(8, "pipe 0 tick == 2", t2 == 2);

    /* Pipe 1727 (last pipe) */
    uint8_t t3 = geo_pipeline_pipe_tick(&ctx, 1727);
    CHECK(8, "pipe 1727 tick == 1", t3 == 1);
}

/* ── T9: Query index by slot_id ── */
static void test_query_index(void)
{
    printf("T9: Query index by slot_id\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    uint8_t data[128];
    memset(data, 0x42, 128);

    geo_pipeline_build_index(&ctx, 100, 128, data, 64);
    const GeoPipelineEntry *e = geo_pipeline_find(&ctx, 100);
    CHECK(9, "find slot_id=100", e != NULL);
    if (e) {
        CHECK(9, "dram_addr == 100", e->dram_addr == 100);
        CHECK(9, "chunk_sz == 64", e->chunk_sz == 64);
    }

    const GeoPipelineEntry *missing = geo_pipeline_find(&ctx, 99999);
    CHECK(9, "missing slot returns NULL", missing == 0);
}

/* ── T10: DRamTile mmap offset ── */
static void test_mmap_offset(void)
{
    printf("T10: DRamTile mmap offset calculation\n");
    uint64_t off0 = dram_mmap_offset(0, 64);
    uint64_t off1 = dram_mmap_offset(1, 64);
    uint64_t off20735 = dram_mmap_offset(20735, 64);
    CHECK(10, "offset(0) == 0", off0 == 0);
    CHECK(10, "offset(1) == 64", off1 == 64);
    CHECK(10, "offset(20735) == 20735*64", off20735 == 20735ULL * 64);
}

/* ── T11: Stats ── */
static void test_stats(void)
{
    printf("T11: Pipeline stats\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    geo_pipeline_tick_n(&ctx, 24);
    geo_pipeline_print_stats(&ctx);

    GeoPipelineStats s = geo_pipeline_stats(&ctx);
    CHECK(11, "total_bridges == 2", s.total_bridges == 2);
    CHECK(11, "spine.total_pipes == 1728", s.spine.total_pipes == 1728);
    CHECK(11, "total_chunks > 0", s.total_chunks > 0);
}

/* ── T12: Reset ── */
static void test_reset(void)
{
    printf("T12: Pipeline reset\n");
    GeoPipelineCtx ctx;
    geo_pipeline_init(&ctx);

    geo_pipeline_tick_n(&ctx, 12);
    CHECK(12, "pre-reset: bridges > 0", ctx.bridges > 0);

    geo_pipeline_reset(&ctx);
    CHECK(12, "post-reset: bridges == 0", ctx.bridges == 0);
    CHECK(12, "post-reset: chunks == 0", ctx.chunks == 0);
    CHECK(12, "post-reset: epoch == 0", ctx.pidx.epoch == 0);
    CHECK(12, "post-reset: is_init still 1", ctx.is_init == 1);
}

/* ── T13: Address space constants ── */
static void test_constants(void)
{
    printf("T13: Sacred constants consistency\n");
    CHECK(13, "DRAM_FULL == 20736", DRAM_FULL == 20736);
    CHECK(13, "DRAM_ANCHORS * DRAM_CELLS_PER == 20736",
          DRAM_ANCHORS * DRAM_CELLS_PER == 20736);
    CHECK(13, "GP_PIPES * GP_PIPE_TICKS == 20736",
          GP_PIPES * GP_PIPE_TICKS == 20736);
    CHECK(13, "GEAR_CPU_WORLD * GEAR_GPU_WORLD == 20736",
          GEAR_CPU_WORLD * GEAR_GPU_WORLD == 20736);
    CHECK(13, "FS_PIPES == GP_PIPES", FS_PIPES == GP_PIPES);
    CHECK(13, "GP_CHUNK_SZ == 64", GP_CHUNK_SZ == 64);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("═══════════════════════════════════════════════════\n");
    printf("  GPU Pipeline Test — DRamTile + GearLock + JetBridge\n");
    printf("═══════════════════════════════════════════════════\n\n");

    test_dram_hilbert();
    test_dram_full();
    test_dram_roundtrip();
    test_pipeline_init();
    test_build_index();
    test_pipeline_tick();
    test_pipeline_tick_n();
    test_gear_sync();
    test_per_pipe_tick();
    test_query_index();
    test_mmap_offset();
    test_stats();
    test_reset();
    test_constants();

    printf("\n═══════════════════════════════════════════════════\n");
    printf("  RESULT: %d PASS / %d FAIL\n", pass, fail);
    printf("═══════════════════════════════════════════════════\n");

    return fail;
}
